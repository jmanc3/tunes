#ifndef TUNES_AUDIO_DATA_H
#define TUNES_AUDIO_DATA_H

#include <string>
#include <vector>
#include <filesystem>

struct Option {
    std::string full;
    std::string name;
    std::string artist;
    std::string album;
    std::string album_all_lower;
    std::string genre;
    std::string year;
    std::string length;
    std::string track;
    std::string disc;
    int track_num = 1000;
    int disc_num = 1000;
    std::string album_artist;
};

struct AlbumOption {
    std::vector<Option> songs;
};

// Everything shown in the song info dialog: tags plus file and stream details.
struct TrackDetails {
    bool valid = false;
    std::string path;
    std::string title, artist, album, album_artist, composer, genre, comment;
    int year = 0, track = 0, disc = 0;
    std::string format;        // Codec name, such as "FLAC" or "MP3".
    bool lossless = false;
    int length_seconds = 0;
    int sample_rate = 0;       // Hz.
    int bits_per_sample = 0;   // 0 when the format does not have a fixed depth.
    int channels = 0;
    int bitrate = 0;           // kbps.
    unsigned long long file_size = 0;
};
TrackDetails read_track_details(const std::string &path);

// Sorted output rates advertised by ALSA USB stream descriptors, with fallback.
std::vector<unsigned> output_sample_rates(const std::string &asound_root = "/proc/asound");
std::string default_music_directory();
std::string normalize_music_directory(const std::string &path);
std::string library_cache_path(const std::string &music_path);
// False when an existing cache predates the current format (for example, no
// album artists). A missing cache counts as current.
bool library_cache_current(const std::string &music_path);
std::vector<Option> load_library(const std::string &music_path = {}, bool scan_if_missing = true);
Option read_track(const std::string &path);
// False for files a library scan must not list: hidden and AppleDouble files,
// partial conversions, and FLAC copies made from another file beside them.
bool is_library_track_candidate(const std::filesystem::path &path);
std::vector<AlbumOption> to_albums(std::vector<Option> &playable);
std::vector<Option> rescan_library(const std::string &music_path, const std::string &cache_path);
int getDiscNumber(const std::string &filePath);
std::string seconds_to_mmss(int seconds);
struct EmbeddedArtwork {
    std::vector<unsigned char> bytes;
    std::string extension;
};
// Original embedded bytes, without decoding or recompressing the image.
EmbeddedArtwork read_album_art(const std::string &filePath);
// Writes embedded artwork to outputBase plus its image extension.
bool extract_album_art(const std::string &filePath, const std::string &outputBase);

#endif
