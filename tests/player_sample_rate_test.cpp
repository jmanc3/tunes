#include "player.h"
#include "miniaudio.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <glib.h>
#include <stdexcept>
#include <thread>
#include <iostream>

static ma_context context;
extern "C" ma_result __real_ma_device_init(ma_context *, const ma_device_config *, ma_device *);
extern "C" ma_result __real_ma_device_start(ma_device *);
extern "C" ma_result __wrap_ma_device_init(ma_context *, const ma_device_config *config, ma_device *device) {
    if (config->sampleRate == 176400) return MA_ERROR;
    return __real_ma_device_init(&context, config, device);
}
extern "C" ma_result __wrap_ma_device_start(ma_device *device) {
    if (device->sampleRate == 88200) return MA_ERROR;
    return __real_ma_device_start(device);
}
static void check(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
int main() {
    // Real miniaudio device callbacks, with a null backend so no speakers are needed.
    ma_backend backend = ma_backend_null;
    check(ma_context_init(&backend, 1, nullptr, &context) == MA_SUCCESS, "null context init failed");
    auto temporary = g_dir_make_tmp("tunes-rate-test-XXXXXX", nullptr);
    check(temporary, "temporary directory failed");
    std::filesystem::path directory(temporary);
    g_free(temporary);
    auto file = directory / "track.wav";
    {
        std::ofstream out(file, std::ios::binary);
        auto word = [&](unsigned v, int n) {
            for (int i = 0; i < n; ++i) out.put(static_cast<char>((v >> (8 * i)) & 255));
        };
        out << "RIFF"; word(36 + 480000, 4); out << "WAVEfmt "; word(16, 4);
        word(1, 2); word(1, 2); word(48000, 4); word(96000, 4); word(2, 2); word(16, 2);
        out << "data"; word(480000, 4);
        for (int i = 0; i < 240000; ++i) word(0, 2);
    }
    {
        Player player;
        const std::vector<std::string> queue{file.string(), file.string()};
        check(player.restore_session(queue, 1, .625), "restore failed");
        player.set_volume(.37f);
        check(player.set_sample_rate(44100), "paused rate change failed");
        check(player.sample_rate() == 44100 && !player.is_playing(), "paused state/rate lost");
        check(std::abs(player.current_time_seconds() - .625) < .002, "paused position changed");
        check(std::abs(player.total_time_seconds() - 5) < .01, "resampled duration incorrect");
        check(player.seek_relative_seconds(1), "resampled seek failed");
        check(std::abs(player.current_time_seconds() - 1.625) < .002, "resampled seek duration incorrect");
        check(player.start(), "start failed");
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        auto before = player.current_time_seconds();
        check(player.set_sample_rate(96000), "playing rate change failed");
        check(player.is_playing() && player.sample_rate() == 96000, "playing state lost");
        check(std::abs(player.current_time_seconds() - before) < .25, "playing position jumped");
        check(player.queue() == queue && player.current_index() == 1 && player.volume() == .37f,
              "queue, index or volume changed");
        check(!player.set_sample_rate(176400), "device init rejection ignored");
        check(player.sample_rate() == 96000 && player.is_playing(), "init failure did not retain playback");
        check(!player.set_sample_rate(88200), "device start rejection ignored");
        check(player.sample_rate() == 96000 && player.is_playing(), "start failure did not roll back playback");
        check(!player.set_sample_rate(0), "invalid rate accepted");
        player.pause();
        check(player.set_sample_rate(192000), "high rate change failed");
        check(!player.is_playing() && player.sample_rate() == 192000, "high rate paused state lost");
    }
    std::filesystem::remove_all(directory);
    ma_context_uninit(&context);
    std::cout << "Sample rate switching, timing and rollback passed with real miniaudio callbacks\n";
}
