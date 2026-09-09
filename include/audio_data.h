#ifndef TUNES_AUDIO_DATA_H
#define TUNES_AUDIO_DATA_H

#include <string>
#include <vector>

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
};

struct AlbumOption {
    std::vector<Option> songs;
};

// Sorted output rates advertised by ALSA USB stream descriptors, with fallback.
std::vector<unsigned> output_sample_rates(const std::string &asound_root = "/proc/asound");
std::string default_music_directory();
std::string normalize_music_directory(const std::string &path);
std::string library_cache_path(const std::string &music_path);
std::vector<Option> load_library(const std::string &music_path = {}, bool scan_if_missing = true);
Option read_track(const std::string &path);
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
