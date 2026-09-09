#include "session_state.h"
#include "audio_data.h"

#include <fstream>
#include <glib.h>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

static void wav(const fs::path &path) {
    std::ofstream out(path, std::ios::binary);
    auto word = [&](unsigned value, int size) {
        for (int i = 0; i < size; ++i)
            out.put(static_cast<char>((value >> (i * 8)) & 255));
    };
    out << "RIFF"; word(36 + 96000, 4); out << "WAVEfmt "; word(16, 4);
    word(1, 2); word(1, 2); word(48000, 4); word(96000, 4); word(2, 2); word(16, 2);
    out << "data"; word(96000, 4);
    for (int i = 0; i < 48000; ++i) word(0, 2);
}

int main() {
    std::vector<Option> tracks;
    auto track = [&](const char *path, const char *album) {
        Option option;
        option.full = path;
        option.album = album;
        tracks.push_back(option);
    };
    track("/music/a/untagged.wav", "");
    track("/music/a/known.wav", "Known");
    track("/elsewhere/b/unknown.wav", "Unknown");
    track("/music/c/known.wav", "Known");
    track("/another/untagged.wav", "");
    const auto albums = to_albums(tracks);
    check(albums.size() == 3, "unknown albums split or known albums merged");
    check(albums[0].songs[0].full == tracks[1].full && albums[1].songs[0].full == tracks[3].full,
          "known album order changed");
    check(albums.back().songs.size() == 3, "unknown tracks not grouped last");
    for (const auto &song : albums.back().songs)
        check(song.album == "Unknown" && song.album_all_lower == "unknown", "unknown labels not normalized");
    check(albums.back().songs[0].full == tracks[0].full && albums.back().songs[2].full == tracks[4].full,
          "unknown track order changed");
    std::vector<Option> empty;
    check(to_albums(empty).empty(), "empty library gained an unknown album");

    auto temporary = g_dir_make_tmp("tunes-session-test-XXXXXX", nullptr);
    check(temporary, "temporary directory");
    const fs::path base(temporary);
    g_free(temporary);
    g_setenv("HOME", base.c_str(), true);
    g_setenv("XDG_STATE_HOME", (base / "state-home").c_str(), true);
    g_setenv("XDG_CACHE_HOME", (base / "cache").c_str(), true);
    fs::create_directories(base / "library-a");
    fs::create_directories(base / "library-b");
    wav(base / "library-a" / "first.wav");
    wav(base / "library-b" / "second.wav");
    check(session_state_path() == base / ".cache" / "tunes" / "session-v1", "session escaped cache directory");
    const auto root_a = normalize_music_directory((base / "library-a").string());
    const auto root_b = normalize_music_directory((base / "library-b").string());
    fs::create_directory_symlink(root_a, base / "alias");
    check(library_cache_path(root_a) != library_cache_path(root_b), "library caches collide");
    check(library_cache_path(root_a) == library_cache_path((base / "alias").string()), "alias split a library cache");
    auto a = load_library(root_a), b = load_library(root_b);
    check(a.size() == 1 && b.size() == 1, "folder library scans failed");
    check(a[0].full != b[0].full, "folder library tracks mixed");
    check(load_library(root_a)[0].full == a[0].full, "switching roots overwrote cached tracks");

    // A cache-only open must not scan or write a missing cache on the UI thread.
    fs::create_directories(base / "library-new");
    wav(base / "library-new" / "new.wav");
    const auto new_root = normalize_music_directory((base / "library-new").string());
    check(load_library(new_root, false).empty(), "cache-only load scanned a new library");
    check(!fs::exists(library_cache_path(new_root)), "cache-only load wrote a cache");
    auto initial_scan = rescan_library(new_root, library_cache_path(new_root));
    check(initial_scan.size() == 1, "initial rescan failed");
    fs::remove(base / "library-new" / "new.wav");
    wav(base / "library-new" / "replacement.wav");
    auto refreshed = rescan_library(new_root, library_cache_path(new_root));
    check(refreshed.size() == 1 && refreshed[0].full != initial_scan[0].full,
          "rescan did not reflect additions and removals");
    check(load_library(new_root, false)[0].full == refreshed[0].full, "rescan cache was not replaced");
    fs::rename(base / "library-new", base / "library-away");
    bool scan_failed = false;
    try {
        rescan_library(new_root, library_cache_path(new_root));
    } catch (const std::exception &) {
        scan_failed = true;
    }
    check(scan_failed, "missing library scan did not report failure");
    check(load_library(new_root, false)[0].full == refreshed[0].full, "failed scan destroyed the old cache");
    fs::rename(base / "library-away", base / "library-new");
    fs::remove(base / "library-new" / "replacement.wav");
    check(rescan_library(new_root, library_cache_path(new_root)).empty(), "rescan retained deleted albums");

    const std::vector<unsigned> fallback_rates{44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    const auto proc = base / "asound";
    check(output_sample_rates(proc.string()) == fallback_rates, "missing ALSA descriptors lost fallback rates");
    fs::create_directories(proc / "card0");
    fs::create_directories(proc / "card1");
    std::ofstream(proc / "card0" / "stream0") << "Playback:\n  Rates: 96000, 44100, 96000, 384000\n"
        "Capture:\n  Rates: 32000\n";
    std::ofstream(proc / "card1" / "stream1") << "Playback:\n  Rates: 352800, 48000, invalid, 0, 999999\n";
    check(output_sample_rates(proc.string()) == std::vector<unsigned>({44100, 48000, 96000, 352800, 384000}),
          "ALSA output rates not sorted, deduplicated, or filtered");
    fs::remove_all(proc);
    fs::create_directories(proc / "card0");
    std::ofstream(proc / "card0" / "stream0") << "Rates: invalid, 0\n";
    check(output_sample_rates(proc.string()) == fallback_rates, "invalid descriptors lost fallback rates");

    SessionState state;
    state.music_root = root_b;
    state.queue = {a[0].full, b[0].full, a[0].full, "a path with \"quotes\" and\na newline.wav"};
    state.current_index = 2;
    state.current_path = a[0].full;
    state.seconds = .625;
    state.volume = 0;
    state.unmuted_volume = .6f;
    state.window_width = 1234;
    state.window_height = 789;
    state.sample_rate = 96000;
    state.scroll_offsets = {{root_a, -1532.25}, {root_b, -82.5}};
    check(library_session_path(root_a) != library_session_path(root_b), "library sessions collide");
    check(library_session_path(root_a) == library_session_path((base / "alias").string()),
          "alias split a library session");
    auto fresh = load_library_session(root_a, state);
    check(fresh.queue.empty() && fresh.seconds == 0, "new library inherited previous playback");
    check(fresh.window_width == state.window_width && fresh.volume == state.volume,
          "new library lost window/volume preferences");
    check(load_library_session(root_b, state) == state, "matching legacy session not restored");
    auto state_a = state;
    state_a.music_root = root_a;
    state_a.queue = {a[0].full};
    state_a.current_index = 0;
    state_a.seconds = .25;
    check(save_session(library_session_path(root_a), state_a), "library A save failed");
    check(save_session(library_session_path(root_b), state), "library B save failed");
    check(load_library_session(root_a, state) == state_a, "library A restored previous run instead");
    check(load_library_session(root_b, state_a) == state, "library B restored previous run instead");
    const auto old_session = base / "state-home" / "tunes" / "session-v1";
    check(save_session(old_session, state), "legacy fixture save failed");
    const auto old_library = old_session.parent_path() / "libraries" / "legacy.session";
    check(save_session(old_library, state_a), "legacy library fixture save failed");
    migrate_legacy_sessions();
    check(load_session(session_state_path()) == state && !fs::exists(old_session), "global session migration failed");
    check(load_session(session_state_path().parent_path() / "libraries" / "legacy.session") == state_a &&
          !fs::exists(old_library), "library session migration failed");
    migrate_legacy_sessions();
    check(load_session(session_state_path()) == state, "repeated migration changed saved state");
    const auto file = base / "state" / "session";
    check(save_session(file, state), "save failed");
    auto restored = load_session(file);
    check(restored == state, "session round-trip changed data");
    {
        std::ifstream input(file);
        std::string contents((std::istreambuf_iterator<char>(input)), {});
        const auto dimensions = contents.rfind("1234 789\n");
        check(dimensions != std::string::npos, "window dimensions not saved");
        std::ofstream(file, std::ios::trunc) << contents.substr(0, dimensions);
        auto legacy = load_session(file);
        auto expected = state;
        expected.window_width = expected.window_height = 0;
        expected.sample_rate = 48000;
        check(legacy == expected, "legacy session compatibility failed");
        std::ofstream(file, std::ios::app) << "-1 789\n";
        check(load_session(file) == expected, "invalid window dimensions accepted");
    }
    remove_missing_tracks(restored);
    check(restored.queue.size() == 3 && restored.current_index == 2 && restored.seconds == .625,
          "duplicate queue entry/position lost during missing-file filtering");
    restored.current_index = 1;
    restored.current_path = b[0].full;
    fs::remove(b[0].full);
    remove_missing_tracks(restored);
    check(restored.queue.size() == 2 && restored.current_index == 1 && restored.seconds == 0,
          "missing current track did not select the next remaining entry");
    fs::remove(a[0].full);
    remove_missing_tracks(restored);
    check(restored.queue.empty() && restored.current_path.empty(), "empty queue not handled");
    check(save_session(file, state), "atomic replacement failed");
    std::ofstream(file, std::ios::trunc) << "tunes-session-v1\n\"truncated";
    check(load_session(file).queue.empty(), "partial session accepted");
    check(load_session(base / "absent").music_root.empty(), "missing session defaults");
    fs::remove_all(base);
    std::cout << "Session persistence, missing tracks, and library cache isolation passed\n";
}
