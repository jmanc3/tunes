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

std::vector<Option> load_library();
std::vector<Option> rescan_library(const std::string &music_path, const std::string &cache_path);
int getDiscNumber(const std::string &filePath);
std::string seconds_to_mmss(int seconds);
// Writes embedded artwork to outputBase plus its image extension.
bool extract_album_art(const std::string &filePath, const std::string &outputBase);

#endif
