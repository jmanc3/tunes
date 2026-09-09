#include "player.h"
#include "miniaudio.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <stdexcept>
#include <thread>

static int device_starts = 0;
// Exercise real decoders and transport state while intercepting only the audio
// device boundary. A restoration that starts output fails this test.
extern "C" ma_result __wrap_ma_device_init(ma_context *, const ma_device_config *, ma_device *) { return MA_SUCCESS; }
extern "C" void __wrap_ma_device_uninit(ma_device *) {}
extern "C" ma_result __wrap_ma_device_stop(ma_device *) { return MA_SUCCESS; }
extern "C" ma_result __wrap_ma_device_start(ma_device *) { ++device_starts; return MA_ERROR; }

static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    auto temporary = g_dir_make_tmp("tunes-player-session-XXXXXX", nullptr);
    check(temporary, "temporary directory");
    const std::filesystem::path base(temporary);
    g_free(temporary);
    const auto path = base / "track.wav";
    {
        std::ofstream out(path, std::ios::binary);
        auto word = [&](unsigned value, int bytes) {
            for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
        };
        out << "RIFF"; word(36 + 192000, 4); out << "WAVEfmt "; word(16, 4);
        word(1, 2); word(1, 2); word(48000, 4); word(96000, 4); word(2, 2); word(16, 2);
        out << "data"; word(192000, 4);
        for (int i = 0; i < 96000; ++i) word(0, 2);
    }
    {
        Player player;
        const std::vector<std::string> queue{path.string(), path.string(), path.string()};
        check(player.restore_session(queue, 1, .625), "restore failed");
        check(!player.is_playing() && device_starts == 0, "restore started audio");
        auto position = player.playback_position();
        check(position.index == 1 && position.path == path && std::abs(position.seconds - .625) < .001,
              "restored queue index/time mismatch");
        check(player.queue() == queue, "restoration changed queue order or duplicates");
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(player.playback_position().seconds == position.seconds, "paused playback advanced");
        check(player.restore_session(queue, 2, 100), "restore past track end failed");
        check(player.current_time_seconds() < player.total_time_seconds(), "position not clamped");
        check(player.restore_session({(base / "missing.wav").string(), path.string()}, 0, 1), "fallback failed");
        check(player.current_index() == 1 && player.current_time_seconds() == 0, "fallback position wrong");
        check(player.restore_session({}, 0, 0) && player.current_path().empty(), "empty restoration failed");
        check(device_starts == 0, "a restore path started the audio device");
    }
    std::filesystem::remove_all(base);
    std::cout << "Real decoder restoration stays paused and preserves queue/index/position\n";
}
