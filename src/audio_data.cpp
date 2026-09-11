#include "audio_conversion.h"
#include "tunes_paths.h"
#include "audio_data.h"
#include <glib.h>

#include <vector>

#include "ThreadPool.h"

#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#endif

#include <algorithm>
#include <charconv>
#include <set>
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

Option read_track(const std::string &path) {
    Option o;
    TagLib::FileRef tag_file(path.c_str());
    if (tag_file.isNull() || !tag_file.file()->isValid())
        return o;
    o.full = path;
    if (auto *tag = tag_file.tag()) {
        o.name = tag->title().to8Bit(true);
        o.artist = tag->artist().to8Bit(true);
        o.album = tag->album().to8Bit(true);
        o.album_all_lower = toLower(o.album);
        o.genre = tag->genre().to8Bit(true);
        o.disc = std::to_string(getDiscNumber(path));
        o.year = std::to_string(tag->year());
        o.track = std::to_string(tag->track());
    }
    if (o.name.empty())
        o.name = std::filesystem::path(path).filename().string();
    if (auto *properties = tag_file.audioProperties())
        o.length = std::to_string(properties->lengthInSeconds());
    return o;
}

static void cache_creation_thread(std::string cache_path, std::string path_to_search) {
    namespace fs = std::filesystem;

    const auto threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    ThreadPool pool(threads);
    std::vector< std::future<Option> > results;

    std::vector<std::string> albums;
    for (const auto& entry : fs::recursive_directory_iterator(path_to_search, fs::directory_options::skip_permission_denied)) {
        if (fs::is_regular_file(entry.path())) {
            std::string full_path = entry.path().string();
            if (full_path.find(".flac.tmp-") != std::string::npos) continue;

            results.emplace_back(pool.enqueue([full_path] { return read_track(full_path); }));
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

std::vector<unsigned> output_sample_rates(const std::string &asound_root) {
    namespace fs = std::filesystem;
    const std::vector<unsigned> fallback{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    std::set<unsigned> rates;
    std::error_code error;
    fs::directory_iterator cards(asound_root, fs::directory_options::skip_permission_denied, error);
    const fs::directory_iterator end;
    for (; !error && cards != end; cards.increment(error)) {
        if (!cards->path().filename().string().starts_with("card"))
            continue;
        std::error_code stream_error;
        fs::directory_iterator streams(cards->path(), fs::directory_options::skip_permission_denied, stream_error);
        for (; !stream_error && streams != end; streams.increment(stream_error)) {
            if (!streams->path().filename().string().starts_with("stream"))
                continue;
            std::ifstream input(streams->path());
            std::string line;
            bool capture = false;
            while (std::getline(input, line)) {
                const auto first = line.find_first_not_of(" \t");
                if (first == std::string::npos)
                    continue;
                const std::string_view text(line.data() + first, line.size() - first);
                if (text.starts_with("Playback:")) capture = false;
                if (text.starts_with("Capture:")) capture = true;
                if (capture || !text.starts_with("Rates:"))
                    continue;
                std::string values(text.substr(6));
                std::replace(values.begin(), values.end(), ',', ' ');
                std::istringstream tokens(values);
                std::string token;
                while (tokens >> token) {
                    unsigned rate = 0;
                    const auto [last, result] = std::from_chars(token.data(), token.data() + token.size(), rate);
                    if (result == std::errc{} && last == token.data() + token.size() && rate >= 8000 && rate <= 384000)
                        rates.insert(rate);
                }
            }
        }
    }
    return rates.empty() ? fallback : std::vector<unsigned>(rates.begin(), rates.end());
}

std::string default_music_directory() {
    return (std::filesystem::path(g_get_home_dir()) / "Music").string();
}

std::string normalize_music_directory(const std::string &path) {
    namespace fs = std::filesystem;
    const auto input = fs::path(path.empty() ? default_music_directory() : path);
    std::error_code error;
    auto normalized = fs::weakly_canonical(input, error);
    if (error)
        normalized = fs::absolute(input).lexically_normal();
    return normalized.string();
}

std::string library_cache_path(const std::string &music_path) {
    const auto root = normalize_music_directory(music_path);
    auto hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, root.c_str(), root.size());
    const auto path = tunes_cache_directory() / "libraries" / (std::string(hash) + ".cache");
    g_free(hash);
    return path.string();
}

std::vector<Option> load_library(const std::string &music_path, bool scan_if_missing) {
    std::vector<Option> options;
    const auto root = normalize_music_directory(music_path);
    const auto cache_path = library_cache_path(root);
    try {
        if (std::filesystem::exists(cache_path))
            load_from_cache(cache_path, options);
        else if (scan_if_missing)
            options = rescan_library(root, cache_path);
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

EmbeddedArtwork read_album_art(const std::string& filePath) {
    TagLib::FileRef ref(filePath.c_str(), false);
    if (!ref.file() || !ref.file()->isValid())
        return {};

    TagLib::ByteVector imageData;
    std::string mime;
    bool selected_front = false;
    auto consider = [&](const TagLib::ByteVector &bytes, const std::string &type, bool front) {
        if (!bytes.isEmpty() && (imageData.isEmpty() || (front && !selected_front) ||
            (front == selected_front && bytes.size() > imageData.size()))) {
            imageData = bytes;
            mime = type;
            selected_front = front;
        }
    };

    // Open the audio file once, without reading its audio properties. Prefer a
    // front cover over other artwork, and preserve the encoded image verbatim.
    if (auto *mpeg = dynamic_cast<TagLib::MPEG::File *>(ref.file())) {
        if (auto *tag = mpeg->ID3v2Tag()) {
            for (auto *frame : tag->frameListMap()["APIC"]) {
                if (auto *pic = dynamic_cast<TagLib::ID3v2::AttachedPictureFrame *>(frame))
                    consider(pic->picture(), pic->mimeType().to8Bit(true),
                             pic->type() == TagLib::ID3v2::AttachedPictureFrame::FrontCover);
            }
        }
    } else if (auto *flac = dynamic_cast<TagLib::FLAC::File *>(ref.file())) {
        for (auto *pic : flac->pictureList())
            consider(pic->data(), pic->mimeType().to8Bit(true), pic->type() == TagLib::FLAC::Picture::FrontCover);
    } else if (auto *mp4 = dynamic_cast<TagLib::MP4::File *>(ref.file())) {
        if (auto *tag = mp4->tag(); tag && tag->itemMap().contains("covr")) {
            for (const auto &art : tag->itemMap()["covr"].toCoverArtList())
                consider(art.data(), art.format() == TagLib::MP4::CoverArt::PNG ? "image/png" : "image/jpeg", true);
        }
    } else if (auto *xiph = dynamic_cast<TagLib::Ogg::XiphComment *>(ref.file()->tag())) {
        for (auto *pic : xiph->pictureList())
            consider(pic->data(), pic->mimeType().to8Bit(true), pic->type() == TagLib::FLAC::Picture::FrontCover);
    }

    if (imageData.isEmpty())
        return {};
    const auto *bytes = reinterpret_cast<const unsigned char *>(imageData.data());
    return {{bytes, bytes + imageData.size()}, get_extension_from_mime(mime)};
}

bool extract_album_art(const std::string& filePath, const std::string& outputBase) {
    const auto art = read_album_art(filePath);
    if (art.bytes.empty())
        return false;
    std::ofstream outFile(outputBase + art.extension, std::ios::binary);
    outFile.write(reinterpret_cast<const char *>(art.bytes.data()), art.bytes.size());
    outFile.close();
    return static_cast<bool>(outFile);
}

std::vector<AlbumOption> to_albums(std::vector<Option> &playable) {
    // Cached scans may predate conversion. Resolve companions before grouping.
    std::unordered_map<std::string, bool> seen;
    std::vector<Option> preferred;
    for (auto option : playable) {
        option.full = preferred_audio_path(option.full);
        if (seen.emplace(option.full, true).second) preferred.push_back(std::move(option));
    }
    playable = std::move(preferred);
    struct DiscFolders {
        std::unordered_map<int, std::string> disc_owners;
        std::string first_folder;
        bool has_multiple_folders = false;
        bool valid = true;
    };
    std::unordered_map<std::string, std::unordered_map<std::string, DiscFolders>> sibling_albums;
    for (const auto &option : playable) {
        if (option.album.empty() || option.album == "Unknown")
            continue;
        const auto folder = std::filesystem::path(option.full).lexically_normal().parent_path();
        const std::string &album = option.album;
        auto &siblings = sibling_albums[folder.parent_path().string()][album];
        const auto folder_name = folder.string();
        if (siblings.first_folder.empty())
            siblings.first_folder = folder_name;
        else if (siblings.first_folder != folder_name)
            siblings.has_multiple_folders = true;

        int disc = 0;
        std::istringstream disc_tag(option.disc);
        if (!(disc_tag >> disc) || disc <= 0) {
            siblings.valid = false;
            continue;
        }
        auto [it, inserted] = siblings.disc_owners.try_emplace(disc, folder_name);
        if (!inserted && it->second != folder_name)
            siblings.valid = false;
    }

    std::vector<AlbumOption> album_options;
    AlbumOption unknown;
    std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>> album_indices;

    for (const auto &option : playable) {
        if (option.album.empty() || option.album == "Unknown") {
            unknown.songs.push_back(option);
            unknown.songs.back().album = "Unknown";
            unknown.songs.back().album_all_lower = "unknown";
            continue;
        }
        const std::string &album = option.album;
        auto folder = std::filesystem::path(option.full).lexically_normal().parent_path();
        const auto &siblings = sibling_albums.at(folder.parent_path().string()).at(album);
        // Combine sibling folders only when their disc numbers do not overlap.
        if (siblings.valid && siblings.has_multiple_folders)
            folder = folder.parent_path();
        auto &folder_albums = album_indices[folder.string()];
        auto [it, inserted] = folder_albums.try_emplace(album, album_options.size());
        if (inserted)
            album_options.emplace_back();

        auto &songs = album_options[it->second].songs;
        songs.push_back(option);
    }

    if (!unknown.songs.empty())
        album_options.push_back(std::move(unknown));
    return album_options;
}
