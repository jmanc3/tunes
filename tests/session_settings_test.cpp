#include "session_state.h"

#include <fstream>
#include <glib.h>
#include <stdexcept>

static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    auto temporary = g_dir_make_tmp("tunes-session-settings-XXXXXX", nullptr);
    check(temporary, "temporary directory creation failed");
    const std::filesystem::path base(temporary);
    g_free(temporary);
    try {
        const auto first = base / "first.session", second = base / "second.session";
        SessionState state;
        check(state.rescan_on_launch && load_session(first).rescan_on_launch, "rescan must default to on");
        state.rescan_on_launch = false;
        check(save_session(first, state), "save disabled setting failed");
        check(load_session(first) == state, "disabled setting did not round-trip");
        state.rescan_on_launch = true;
        check(save_session(second, state), "save enabled setting failed");
        check(load_session(second) == state && !load_session(first).rescan_on_launch,
              "session preferences were not isolated");
        // Old files end after the sample rate; absence of the new field means on.
        std::ifstream input(first);
        std::string legacy((std::istreambuf_iterator<char>(input)), {});
        input.close();
        legacy.resize(legacy.rfind("\n0\n") + 1);
        std::ofstream(first, std::ios::trunc) << legacy;
        check(load_session(first) == state, "legacy session did not default to enabled");
        state.expanded_album_track = "album/track with \"quotes\" and\na newline.flac";
        check(save_session(first, state) && load_session(first) == state,
              "expanded album did not round-trip");
        check(load_session(second).expanded_album_track.empty(), "expanded album leaked between sessions");
        state.expanded_album_track.clear();
        check(save_session(first, state) && load_session(first).expanded_album_track.empty(),
              "collapsed album was not saved");
        state.rescan_on_launch = false;
        check(save_session(first, state), "toggle off failed");
        state.rescan_on_launch = true;
        check(save_session(first, state) && load_session(first) == state, "toggle back on failed");
    } catch (...) {
        std::filesystem::remove_all(base);
        throw;
    }
    std::filesystem::remove_all(base);
}
