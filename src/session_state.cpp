#include "tunes_paths.h"
#include "session_state.h"
#include "audio_data.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <glib.h>
#include <iomanip>
#include <set>
#include <unistd.h>

std::filesystem::path session_state_path() {
    return tunes_cache_directory() / "session-v1";
}

std::filesystem::path library_session_path(const std::string &music_root) {
    const auto root = normalize_music_directory(music_root);
    auto hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, root.c_str(), root.size());
    const auto path = session_state_path().parent_path() / "libraries" / (std::string(hash) + ".session");
    g_free(hash);
    return path;
}

SessionState load_library_session(const std::string &music_root, const SessionState &last_session) {
    const auto root = normalize_music_directory(music_root);
    auto state = load_session(library_session_path(root));
    if (!state.music_root.empty() && normalize_music_directory(state.music_root) == root)
        return state;
    // Migrate the old global session only when it belongs to this library.
    if (!last_session.music_root.empty() && normalize_music_directory(last_session.music_root) == root)
        return last_session;
    state = {};
    state.music_root = root;
    state.volume = last_session.volume;
    state.unmuted_volume = last_session.unmuted_volume;
    state.window_width = last_session.window_width;
    state.window_height = last_session.window_height;
    state.sample_rate = last_session.sample_rate;
    return state;
}

SessionState load_session(const std::filesystem::path &path) {
    std::ifstream in(path, std::ios::binary);
    SessionState state;
    std::string version;
    std::size_t count = 0;
    if (!std::getline(in, version) || version != "tunes-session-v1" ||
        !(in >> std::quoted(state.music_root) >> state.current_index >> std::quoted(state.current_path)
             >> state.seconds >> state.volume >> state.unmuted_volume >> count) || count > 1000000 ||
        !std::isfinite(state.seconds) || !std::isfinite(state.volume) || !std::isfinite(state.unmuted_volume))
        return {};
    state.seconds = std::max(0.0, state.seconds);
    state.volume = std::clamp(state.volume, 0.0f, 1.0f);
    state.unmuted_volume = std::clamp(state.unmuted_volume, .01f, 1.0f);
    for (std::size_t i = 0; i < count; ++i) {
        std::string track;
        if (!(in >> std::quoted(track)))
            return {};
        state.queue.push_back(std::move(track));
    }
    if (!(in >> count) || count > 100000)
        return {};
    for (std::size_t i = 0; i < count; ++i) {
        std::string root;
        double offset = 0;
        if (!(in >> std::quoted(root) >> offset) || !std::isfinite(offset))
            return {};
        state.scroll_offsets[root] = std::min(0.0, offset);
    }
    // Older sessions end after the scroll offsets.
    int width = 0, height = 0;
    if (in >> width >> height && width > 0 && height > 0) {
        state.window_width = width;
        state.window_height = height;
    }
    unsigned rate = 0;
    if (in >> rate && rate >= 8000 && rate <= 384000)
        state.sample_rate = rate;
    int rescan = 1;
    if (in >> rescan && (rescan == 0 || rescan == 1))
        state.rescan_on_launch = rescan != 0;
    std::string expanded_album_track;
    if (in >> std::quoted(expanded_album_track))
        state.expanded_album_track = std::move(expanded_album_track);
    // Optional extension: old sessions and old readers keep their v1 prefix.
    std::string extension, expanded_playlist;
    if (!(in >> extension) || extension != "playlists-v1")
        return state;
    if (!(in >> std::quoted(expanded_playlist) >> count) || count > 100000)
        return state;
    std::vector<PlaylistState> playlists;
    std::set<std::string> identities;
    std::size_t total_tracks = 0;
    for (std::size_t i = 0; i < count; ++i) {
        PlaylistState playlist;
        std::size_t tracks = 0;
        if (!(in >> std::quoted(playlist.id) >> std::quoted(playlist.name) >> tracks) ||
            playlist.id.empty() || !identities.insert(playlist.id).second ||
            tracks > 1000000 - total_tracks)
            return state;
        total_tracks += tracks;
        for (std::size_t j = 0; j < tracks; ++j) {
            std::string track;
            if (!(in >> std::quoted(track)))
                return state;
            playlist.tracks.push_back(std::move(track));
        }
        playlists.push_back(std::move(playlist));
    }
    state.playlists = std::move(playlists);
    state.expanded_playlist_id = std::move(expanded_playlist);
    if (!(in >> extension) || extension != "playlist-art-v1")
        return state;
    if (!(in >> count) || count > state.playlists.size())
        return state;
    std::map<std::string, std::string> artwork;
    for (std::size_t i = 0; i < count; ++i) {
        std::string id, file;
        if (!(in >> std::quoted(id) >> std::quoted(file)) || !identities.contains(id) || !artwork.emplace(id, file).second)
            return state;
    }
    for (auto &playlist : state.playlists)
        if (auto found = artwork.find(playlist.id); found != artwork.end())
            playlist.art_file = std::move(found->second);
    return state;
}

bool save_session(const std::filesystem::path &path, const SessionState &state) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return false;
    std::string staging = path.string() + ".XXXXXX";
    const int fd = g_mkstemp(staging.data());
    if (fd < 0)
        return false;
    close(fd);
    std::ofstream out(staging, std::ios::binary);
    out << std::setprecision(17) << "tunes-session-v1\n" << std::quoted(state.music_root) << '\n'
        << state.current_index << '\n' << std::quoted(state.current_path) << '\n'
        << state.seconds << ' ' << state.volume << ' ' << state.unmuted_volume << '\n' << state.queue.size() << '\n';
    for (const auto &track : state.queue)
        out << std::quoted(track) << '\n';
    out << state.scroll_offsets.size() << '\n';
    for (const auto &[root, offset] : state.scroll_offsets)
        out << std::quoted(root) << ' ' << offset << '\n';
    out << state.window_width << ' ' << state.window_height << '\n' << state.sample_rate << '\n'
        << state.rescan_on_launch << '\n' << std::quoted(state.expanded_album_track) << '\n';
    out << "playlists-v1\n" << std::quoted(state.expanded_playlist_id) << '\n' << state.playlists.size() << '\n';
    for (const auto &playlist : state.playlists) {
        out << std::quoted(playlist.id) << '\n' << std::quoted(playlist.name) << '\n' << playlist.tracks.size() << '\n';
        for (const auto &track : playlist.tracks)
            out << std::quoted(track) << '\n';
    }
    out << "playlist-art-v1\n" << std::count_if(state.playlists.begin(), state.playlists.end(),
        [](const auto &playlist) { return !playlist.art_file.empty(); }) << '\n';
    for (const auto &playlist : state.playlists)
        if (!playlist.art_file.empty())
            out << std::quoted(playlist.id) << ' ' << std::quoted(playlist.art_file) << '\n';
    out.close();
    const bool written = static_cast<bool>(out);
    if (written)
        std::filesystem::rename(staging, path, error);
    const bool saved = written && !error;
    std::filesystem::remove(staging, error);
    return saved;
}

void remove_missing_tracks(SessionState &state) {
    const auto missing = std::numeric_limits<std::size_t>::max();
    if (state.current_index >= state.queue.size() || state.queue[state.current_index] != state.current_path) {
        const auto found = std::find(state.queue.begin(), state.queue.end(), state.current_path);
        state.current_index = found == state.queue.end() ? missing : found - state.queue.begin();
    }
    std::vector<std::string> kept;
    std::size_t selected = missing;
    std::size_t next = missing;
    for (std::size_t i = 0; i < state.queue.size(); ++i) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(state.queue[i], error))
            continue;
        if (i == state.current_index)
            selected = kept.size();
        if (next == missing && i > state.current_index)
            next = kept.size();
        kept.push_back(state.queue[i]);
    }
    if (selected == missing) {
        selected = next == missing ? 0 : next;
        state.seconds = 0;
    }
    state.queue = std::move(kept);
    state.current_index = state.queue.empty() ? missing : selected;
    state.current_path = state.queue.empty() ? "" : state.queue[state.current_index];
}
