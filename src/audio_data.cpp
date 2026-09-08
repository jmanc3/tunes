#include "audio_data.h"

#include <vector>

#include "ThreadPool.h"

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <fstream>
#include <future>
#include <fileref.h>
#include <attachedpictureframe.h>
#include <flacfile.h>
#include <flacpicture.h>
#include <id3v2tag.h>
#include <mpegfile.h>
#include <mp4coverart.h>
#include <vorbisfile.h>
#include <xiphcomment.h>
#include <unordered_map>
#include <mp4file.h>
#include <mp4tag.h>
#include <tag.h>

static std::string toLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return lower;
}


// Helper to read 4-byte little-endian unsigned int safely
static uint32_t readUInt32LE(const char *data) {
    return (static_cast<uint8_t>(data[0])) |
           (static_cast<uint8_t>(data[1]) << 8) |
           (static_cast<uint8_t>(data[2]) << 16) |
           (static_cast<uint8_t>(data[3]) << 24);
}

// Case-insensitive string comparison (portable)
static bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

static int extractDiscNumberFromFlac(const std::string &filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file.\n";
        return -1;
    }

    // Check FLAC header
    char header[4];
    file.read(header, 4);
    if (file.gcount() != 4 || std::string(header, 4) != "fLaC") {
        std::cerr << "Not a valid FLAC file.\n";
        return -1;
    }

    bool lastBlock = false;
    while (!lastBlock && file) {
        // Read metadata block header
        unsigned char blockHeader[4];
        file.read(reinterpret_cast<char*>(blockHeader), 4);
        if (file.gcount() != 4) {
            std::cerr << "Failed to read metadata block header.\n";
            return -1;
        }

        lastBlock = blockHeader[0] & 0x80;
        uint8_t blockType = blockHeader[0] & 0x7F;
        uint32_t blockSize = (blockHeader[1] << 16) | (blockHeader[2] << 8) | blockHeader[3];

        if (blockType == 4) {  // VORBIS_COMMENT
            std::vector<char> blockData(blockSize);
            file.read(blockData.data(), blockSize);
            if (file.gcount() != static_cast<std::streamsize>(blockSize)) {
                std::cerr << "Failed to read Vorbis comment block.\n";
                return -1;
            }

            const char *ptr = blockData.data();
            const char *end = ptr + blockSize;

            // Read vendor string length (4 bytes LE)
            if (ptr + 4 > end) return -1;
            uint32_t vendorLen = readUInt32LE(ptr);
            ptr += 4;
            if (ptr + vendorLen > end) return -1;
            ptr += vendorLen;  // Skip vendor string

            // Read user comment list length (4 bytes LE)
            if (ptr + 4 > end) return -1;
            uint32_t userCommentListLen = readUInt32LE(ptr);
            ptr += 4;

            for (uint32_t i = 0; i < userCommentListLen; ++i) {
                if (ptr + 4 > end) return -1;
                uint32_t commentLen = readUInt32LE(ptr);
                ptr += 4;
                if (ptr + commentLen > end) return -1;
                std::string comment(ptr, commentLen);
                ptr += commentLen;

                auto eqPos = comment.find('=');
                if (eqPos != std::string::npos) {
                    std::string key = comment.substr(0, eqPos);
                    std::string value = comment.substr(eqPos + 1);
                    if (iequals(key, "DISCNUMBER")) {
                        try {
                            return std::stoi(value);
                        } catch (...) {
                            std::cerr << "Failed to parse DISCNUMBER value.\n";
                            return -1;
                        }
                    }
                }
            }
            // No DISCNUMBER found
            return -1;
        } else {
            // Skip block
            file.seekg(blockSize, std::ios::cur);
        }
    }

    std::cerr << "No Vorbis comment block found.\n";
    return -1;
}


// Reads 4-byte synchsafe int (7 bits per byte)
static uint32_t readSynchsafeInt(const unsigned char *data) {
    return ((data[0] & 0x7F) << 21) |
           ((data[1] & 0x7F) << 14) |
           ((data[2] & 0x7F) << 7) |
           (data[3] & 0x7F);
}

// Reads 4-byte big-endian int
static uint32_t readUInt32BE(const unsigned char *data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           (static_cast<uint32_t>(data[3]));
}

static int extractDiscNumberFromMp3(const std::string &filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open file.\n";
        return -1;
    }

    // Check ID3 header
    unsigned char header[10];
    file.read(reinterpret_cast<char*>(header), 10);
    if (file.gcount() != 10 || std::string(reinterpret_cast<char*>(header), 3) != "ID3") {
        std::cerr << "No ID3v2 tag found.\n";
        return -1;
    }

    uint8_t versionMajor = header[3];
    uint8_t versionMinor = header[4];
    uint8_t flags = header[5];
    uint32_t tagSize = readSynchsafeInt(&header[6]);

    // Optional extended header for ID3v2.3+ (skip if present)
    if ((flags & 0x40) != 0) {
        unsigned char extHeader[4];
        file.read(reinterpret_cast<char*>(extHeader), 4);
        if (file.gcount() != 4) return -1;
        uint32_t extSize = readUInt32BE(extHeader);
        file.seekg(extSize - 4, std::ios::cur);
    }

    uint32_t bytesRead = 0;
    while (bytesRead + 10 <= tagSize) {
        unsigned char frameHeader[10];
        file.read(reinterpret_cast<char*>(frameHeader), 10);
        if (file.gcount() != 10) break;

        std::string frameID(reinterpret_cast<char*>(frameHeader), 4);
        uint32_t frameSize;
        if (versionMajor == 4) {
            frameSize = readSynchsafeInt(&frameHeader[4]);
        } else {
            frameSize = readUInt32BE(&frameHeader[4]);
        }

        uint16_t frameFlags = (frameHeader[8] << 8) | frameHeader[9];
        bytesRead += 10;

        if (frameSize == 0) break;  // Padding

        if (bytesRead + frameSize > tagSize) {
            // Corrupt frame
            break;
        }

        if (frameID == "TPOS") {
            std::vector<char> frameData(frameSize);
            file.read(frameData.data(), frameSize);
            if (file.gcount() != static_cast<std::streamsize>(frameSize)) {
                std::cerr << "Failed to read TPOS frame data.\n";
                return -1;
            }

            // First byte: encoding (0 = ISO-8859-1, 1 = UTF-16 etc.)
            std::string text(frameData.begin() + 1, frameData.end());
            try {
                size_t pos = 0;
                int disc = std::stoi(text, &pos);
                return disc;
            } catch (...) {
                std::cerr << "Failed to parse TPOS value.\n";
                return -1;
            }
        } else {
            // Skip this frame
            file.seekg(frameSize, std::ios::cur);
        }
        bytesRead += frameSize;
    }

    std::cerr << "TPOS frame not found.\n";
    return -1;
}

int getDiscNumber(const std::string &filePath) {
    int disc = extractDiscNumberFromFlac(filePath);
    if (disc != -1) {
        return disc;
    }

    disc = extractDiscNumberFromMp3(filePath);
    if (disc != -1) {
        return disc;
    }

    TagLib::MP4::File mp4File(filePath.c_str());
    if (mp4File.isValid()) {
        TagLib::MP4::Tag *tag = mp4File.tag();
        if (tag) {
            const auto &items = tag->itemMap();
            if (items.contains("disk")) {
                TagLib::MP4::Item item = items["disk"];
                if (item.isValid() && item.toIntPair().first > 0) {
                    return item.toIntPair().first;
                }
            }
        }
    }

    // Check for MP4 (M4A) files
    /*
    TagLib::MP4::File mp4File(filePath.c_str());
    if (mp4File.isValid()) {
        TagLib::MP4::Tag *tag = mp4File.tag();
        if (tag) {
            if (tag->itemListMap().contains("disk")) {
                TagLib::MP4::Item item = tag->itemListMap()["disk"];
                if (!item.isEmpty() && item.toIntPair().first > 0) {
                    return item.toIntPair().first;
                }
            }
        }
    }
*/

    return 0;
}


static void write_to(std::ofstream &file, std::string header, std::string body) {
    file << header;
    file << body;
    file << std::endl;
}

static void cache_creation_thread(std::string cache_path, std::string path_to_search) {
    namespace fs = std::filesystem;

    unsigned int threads = std::thread::hardware_concurrency();
    if (threads == 0)
        threads = 8;
    ThreadPool pool(threads);
    std::vector< std::future<Option> > results;

    std::vector<std::string> albums;
    for (const auto& entry : fs::recursive_directory_iterator(path_to_search)) {
        if (fs::is_regular_file(entry.path())) {
            std::string full_path = entry.path().string();

            results.emplace_back(pool.enqueue([full_path, entry] {
                Option o;
                TagLib::FileRef tag_file(full_path.c_str());
                if (tag_file.isNull()) {
                   return o;
                }
                TagLib::Tag *tag = tag_file.tag();
                if (tag) {
                    o.full = full_path;
                    o.name = tag->title().to8Bit(true);  // Convert to std::string
                    if (o.name.empty()) {
                        o.name = entry.path().filename().string();
                    }
                    o.artist = tag->artist().to8Bit(true);  // Convert to std::string
                    o.album = tag->album().to8Bit(true);  // Convert to std::string
                    o.genre = tag->genre().to8Bit(true);  // Convert to std::string
                    o.disc = std::to_string(getDiscNumber(full_path));
                    o.year = std::to_string((int) tag->year());  // Convert to std::string
                    o.track = std::to_string((int) tag->track());  // Convert to std::string
                }

                TagLib::AudioProperties *properties = tag_file.audioProperties();
                if (properties) {
                   o.length = std::to_string(properties->lengthInSeconds());
                }
                return o;

                //options.push_back(o);
            }));
        }
    }

    std::vector<Option> options;
    for (auto && result: results) {
        options.push_back(result.get());
    }

    std::ofstream file;
    file.exceptions(std::ios::failbit | std::ios::badbit);
    file.open(cache_path);
    file << "version: 1" << std::endl;
    for (auto o: options) {
        if (o.full.empty())
            continue;
        write_to(file, "Path: ", o.full);
        //file << "Path: " << full_path.c_str() << std::end;
        write_to(file, "Title: ", o.name);
        write_to(file, "Artist: ", o.artist);
        write_to(file, "Album: ", o.album);
        write_to(file, "Genre: ", o.genre);
        write_to(file, "Year: ", o.year);
        write_to(file, "Length: ", o.length);
        write_to(file, "Track: ", o.track);
        write_to(file, "Disc: ", o.disc);
    }

    file.close();
}

static void load_from_cache(std::string cache_path, std::vector<Option> &options) {
#ifdef TRACY_ENABLE
    ZoneScopedN("From cache");
#endif
    // TODO: if version #1 not found, create from cache first, and then load it here
    // TODO: if version #1 not found, create from cache first, and then load it here
    // TODO: if version #1 not found, create from cache first, and then load it here
    // TODO: if version #1 not found, create from cache first, and then load it here

    std::ifstream file(cache_path); // Replace with your actual file name
    std::string line;
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string year;
    std::string length;
    std::string track;
    std::string disc;
    bool last = false;
    while (std::getline(file, line)) {
        if (line.find("Path:") != std::string::npos) {
            line.erase(0, 6);
            path = line;
        } else if (line.find("Title:") != std::string::npos) {
            line.erase(0, 7);
            title = line;
        } else if (line.find("Artist:") != std::string::npos) {
            line.erase(0, 8);
            artist = line;
        } else if (line.find("Album:") != std::string::npos) {
            line.erase(0, 7);
            album = line;
        } else if (line.find("Genre:") != std::string::npos) {
            line.erase(0, 7);
            genre = line;
        } else if (line.find("Year:") != std::string::npos) {
            line.erase(0, 6);
            year = line;
        } else if (line.find("Length:") != std::string::npos) {
            line.erase(0, 8);
            length = line;
        } else if (line.find("Track:") != std::string::npos) {
            line.erase(0, 7);
            track = line;
        }else if (line.find("Disc:") != std::string::npos) {
            line.erase(0, 6);
            disc = line;
            last = true;
        }

        if (last) {
            last = false;
            options.push_back({path, title, artist, album, toLower(album), genre, year, length, track, disc});
            path = title = artist = album = genre = year = length = track = "";
        }
    }
    file.close();
}

std::vector<Option> rescan_library(const std::string &music_path, const std::string &cache_path) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(music_path))
        throw std::runtime_error("Music directory is missing or inaccessible");
    fs::create_directories(fs::path(cache_path).parent_path());
    // A unique sibling file keeps the old cache intact until the entire scan succeeds.
    std::string pattern = cache_path + ".XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data());
    if (fd == -1)
        throw std::runtime_error("Cannot create library cache");
    close(fd);
    const std::string temporary(name.data());
    try {
        cache_creation_thread(temporary, music_path);
        std::vector<Option> options;
        load_from_cache(temporary, options);
        fs::rename(temporary, cache_path);
        return options;
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

std::vector<Option> load_library() {
    std::vector<Option> options;
    const char *home = getenv("HOME");
    if (!home)
        return options;
    const std::string cache_path = std::string(home) + "/.cache/tunes.cache";
    try {
        if (std::filesystem::exists(cache_path))
            load_from_cache(cache_path, options);
        else
            options = rescan_library(std::string(home) + "/Music", cache_path);
    } catch (const std::exception &e) {
        std::cerr << "Library load failed: " << e.what() << std::endl;
    }
    return options;
}


std::string seconds_to_mmss(int seconds) {
    int minutes = seconds / 60;
    int secs = seconds % 60;

    std::ostringstream oss;
    oss << minutes << ":"
        << std::setw(2) << std::setfill('0') << secs;

    return oss.str();
}

static std::string get_extension_from_mime(const std::string& mime) {
    static std::unordered_map<std::string, std::string> mimeToExt = {
        {"image/jpeg", ".jpg"},
        {"image/png",  ".png"},
        {"image/gif",  ".gif"},
        {"image/bmp",  ".bmp"},
        {"image/tiff", ".tiff"}
    };
    auto it = mimeToExt.find(mime);
    return (it != mimeToExt.end()) ? it->second : ".bin";
}

bool extract_album_art(const std::string& filePath, const std::string& outputBase) {
    TagLib::FileRef ref(filePath.c_str());
    if (!ref.file() || !ref.file()->isValid()) {
        std::cerr << "Invalid or unsupported file: " << filePath << std::endl;
        return false;
    }

    std::string extension = ".bin";
    TagLib::ByteVector imageData;
    std::string mime;

    // Try MP3 (ID3v2)
    if (auto* mpeg = dynamic_cast<TagLib::MPEG::File*>(ref.file())) {
        auto* id3 = mpeg->ID3v2Tag();
        if (id3) {
            auto frames = id3->frameListMap()["APIC"];
            if (!frames.isEmpty()) {
                auto* pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame*>(frames.front());
                if (pic) {
                    imageData = pic->picture();
                    mime = pic->mimeType().to8Bit(true);
                }
            }
        }
    }

    // Try FLAC
    if (imageData.isEmpty()) {
        TagLib::FLAC::File flac(filePath.c_str());
        if (flac.isValid()) {
            auto pics = flac.pictureList();
            if (!pics.isEmpty()) {
                auto* pic = pics.front();
                if (pic) {
                    imageData = pic->data();
                    mime = pic->mimeType().to8Bit(true);
                }
            }
        }
    }

    // Try MP4/M4A
    if (imageData.isEmpty()) {
        TagLib::MP4::File mp4(filePath.c_str());
        if (mp4.isValid()) {
            auto tag = mp4.tag();
            if (tag && tag->itemMap().contains("covr")) {
                const auto covr = tag->itemMap()["covr"].toCoverArtList();
                if (!covr.isEmpty()) {
                    const auto& art = covr.front();
                    imageData = art.data();
                    mime = art.format() == TagLib::MP4::CoverArt::PNG ? "image/png" : "image/jpeg";
                }
            }
        }
    }


    // Try OGG Vorbis/Opus
    if (imageData.isEmpty()) {
        TagLib::Ogg::Vorbis::File ogg(filePath.c_str());
        if (ogg.isValid()) {
            auto tag = ogg.tag();
            auto xiph = dynamic_cast<TagLib::Ogg::XiphComment*>(tag);
            if (xiph && xiph->pictureList().size() > 0) {
                auto* pic = xiph->pictureList().front();
                if (pic) {
                    imageData = pic->data();
                    mime = pic->mimeType().to8Bit(true);
                }
            }
        }
    }


    if (imageData.isEmpty()) {
        std::cerr << "No album art found in file: " << filePath << std::endl;
        return false;
    }

    extension = get_extension_from_mime(mime);
    std::string outputPath = outputBase + extension;

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to write to: " << outputPath << std::endl;
        return false;
    }

    outFile.write(imageData.data(), imageData.size());
    outFile.close();

    return static_cast<bool>(outFile);
}
