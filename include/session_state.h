#pragma once

#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

struct SessionState {
    std::string music_root;
    std::vector<std::string> queue;
    std::size_t current_index = std::numeric_limits<std::size_t>::max();
    std::string current_path;
    double seconds = 0;
    float volume = 1;
    float unmuted_volume = .75f;
    // Logical pixels, keyed by canonical library directory.
    std::map<std::string, double> scroll_offsets;
    // Logical pixels; zero uses the default window size.
    int window_width = 0;
    int window_height = 0;
    unsigned sample_rate = 48000;
    bool rescan_on_launch = true;
    bool operator==(const SessionState &) const = default;
};

std::filesystem::path session_state_path();
std::filesystem::path library_session_path(const std::string &music_root);
SessionState load_library_session(const std::string &music_root, const SessionState &last_session);
SessionState load_session(const std::filesystem::path &path);
bool save_session(const std::filesystem::path &path, const SessionState &state);
// Drops unavailable files while preserving queue order and duplicate entries.
void remove_missing_tracks(SessionState &state);
