#include "player.h"
#include "miniaudio.h"
#include <pulse/pulseaudio.h>
#include <glib.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

static bool pulse_available = false;
static int starts = 0;
static bool stale_audio = false;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

extern "C" ma_result __real_ma_device_init(ma_context*, const ma_device_config*, ma_device*);
extern "C" ma_result __real_ma_device_start(ma_device*);
extern "C" ma_result __wrap_ma_device_init(ma_context* context, const ma_device_config* config, ma_device* device) {
    const auto result = __real_ma_device_init(context, config, device);
    pulse_available = result == MA_SUCCESS && device->pContext &&
        device->pContext->backend == ma_backend_pulseaudio;
    return result;
}

// Inspect the actual server queue immediately before miniaudio uncorks it.
// The device thread is stopped here, so it cannot race mainloop iteration.
extern "C" ma_result __wrap_ma_device_start(ma_device* device) {
    if (!pulse_available) return MA_ERROR;
    auto* stream = static_cast<pa_stream*>(device->pulse.pStreamPlayback);
    auto* loop = static_cast<pa_mainloop*>(device->pulse.pMainLoop);
    if (pa_stream_is_corked(stream) != 1) return MA_ERROR;
    auto* operation = pa_stream_update_timing_info(stream, nullptr, nullptr);
    if (!operation) return MA_ERROR;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING &&
           std::chrono::steady_clock::now() < deadline) {
        if (pa_mainloop_iterate(loop, 0, nullptr) < 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool done = pa_operation_get_state(operation) == PA_OPERATION_DONE;
    pa_operation_cancel(operation);
    pa_operation_unref(operation);
    const auto* timing = pa_stream_get_timing_info(stream);
    if (!done || !timing || timing->read_index_corrupt || timing->write_index_corrupt) return MA_ERROR;
    const auto queued = timing->write_index - timing->read_index;
    std::cout << "Start " << ++starts << ": " << queued << " queued bytes\n";
    stale_audio |= queued > 0;
    return __real_ma_device_start(device);
}

static void write_silence(const std::filesystem::path& path) {
    std::ofstream out(path, std::ios::binary);
    auto word = [&](unsigned value, int bytes) {
        for (int i = 0; i < bytes; ++i) out.put(static_cast<char>((value >> (8 * i)) & 255));
    };
    out << "RIFF"; word(36 + 192000, 4); out << "WAVEfmt "; word(16, 4);
    word(1, 2); word(1, 2); word(48000, 4); word(96000, 4); word(2, 2); word(16, 2);
    out << "data"; word(192000, 4);
    for (int i = 0; i < 96000; ++i) word(0, 2);
    check(out.good(), "could not write silent fixture");
}

int main() {
    auto* temporary = g_dir_make_tmp("tunes-player-pause-XXXXXX", nullptr);
    check(temporary, "temporary directory failed");
    struct TemporaryDirectory {
        std::filesystem::path path;
        ~TemporaryDirectory() { std::filesystem::remove_all(path); }
    } directory{temporary};
    g_free(temporary);
    const auto first = directory.path / "first.wav";
    const auto second = directory.path / "second.wav";
    write_silence(first);
    write_silence(second);
    Player player;
    if (!pulse_available) {
        std::cout << "SKIP: requires a running PulseAudio or PipeWire PulseAudio server\n";
        return 77;
    }
    check(player.play_track(first.string()), "initial playback failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    player.pause();
    check(!player.is_playing(), "pause did not stop playback");
    check(player.play_track(second.string()), "selection while paused failed");
    check(player.current_path() == second.string(), "wrong selected track");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    player.pause();
    check(player.start(), "resume failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(player.play_track(first.string()), "selection while playing failed");
    player.pause();
    check(starts == 4 && !stale_audio, "old server-buffered audio would replay on restart");
}
