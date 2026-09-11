#include "player.h"
#include "audio_conversion.h"
#include <future>
#include <optional>
#include <unordered_set>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "miniaudio.h"
#include <pulse/pulseaudio.h>

namespace {

constexpr ma_format format = ma_format_f32;
constexpr ma_uint32 channels = 2;
constexpr ma_uint64 max_chunk = 4096;

constexpr double transport_fade_ms = 5.0;

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

struct decoder_handle {
    ma_decoder decoder{};
    bool initialized = false;

    ~decoder_handle() {
        if (initialized)
            ma_decoder_uninit(&decoder);
    }
};

struct prepared_decoder {
    std::unique_ptr<decoder_handle> decoder;
    std::vector<float> prefix;
    ma_uint64 prefix_frames = 0;
    ma_uint64 length_frames = 0;
};

prepared_decoder open_decoder(
    const std::string& path,
    ma_uint32 sample_rate,
    ma_uint64 prefix_frames_wanted,
    ma_uint64 seek_frame = 0
) {
    prepared_decoder result;

    auto decoder = std::make_unique<decoder_handle>();
    const ma_decoder_config config =
        ma_decoder_config_init(format, channels, sample_rate);

    if (ma_decoder_init_file(path.c_str(), &config, &decoder->decoder) != MA_SUCCESS)
        return result;

    decoder->initialized = true;

    ma_decoder_get_length_in_pcm_frames(
        &decoder->decoder,
        &result.length_frames
    );

    if (seek_frame != 0 &&
        ma_decoder_seek_to_pcm_frame(&decoder->decoder, seek_frame) != MA_SUCCESS) {
        return {};
    }

    if (prefix_frames_wanted != 0) {
        result.prefix.resize(
            static_cast<std::size_t>(prefix_frames_wanted * channels)
        );

        ma_decoder_read_pcm_frames(
            &decoder->decoder,
            result.prefix.data(),
            prefix_frames_wanted,
            &result.prefix_frames
        );

        result.prefix.resize(
            static_cast<std::size_t>(result.prefix_frames * channels)
        );
    }

    result.decoder = std::move(decoder);
    return result;
}

} // namespace


struct Player::Impl {
    enum class mode_type {
        current,
        crossfade,
        buffered_head,
        done
    };

    explicit Impl(double crossfade_ms, ma_uint32 output_rate = 48000)
        : sample_rate(output_rate),
          transport_fade_frames(static_cast<ma_uint64>(output_rate * transport_fade_ms / 1000.0)),
          crossfade_ms(crossfade_ms),
          fade_frames(
              crossfade_ms <= 0.0
                  ? 0
                  : static_cast<ma_uint64>(
                        static_cast<double>(sample_rate) * crossfade_ms / 1000.0
                    )
          ),
          tail(static_cast<std::size_t>(
              std::max<ma_uint64>(fade_frames, 1) * channels
          )),
          head(static_cast<std::size_t>(
              std::max<ma_uint64>(fade_frames, 1) * channels
          )),
          combine(static_cast<std::size_t>(
              (std::max<ma_uint64>(fade_frames, 1) + max_chunk) * channels
          )),
          scratch(static_cast<std::size_t>(max_chunk * channels)) {

        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = format;
        config.playback.channels = channels;
        config.sampleRate = sample_rate;
        config.dataCallback = &Impl::audio_callback;
        config.pUserData = this;

        if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
            set_error("Failed to initialize miniaudio playback device.");
            return;
        }

        device_initialized = true;
        // Query this device's server before the audio thread starts using its mainloop.
        if (device.pContext && device.pContext->backend == ma_backend_pulseaudio &&
            device.pulse.pPulseContext && device.pulse.pMainLoop) {
            auto *context = static_cast<pa_context *>(device.pulse.pPulseContext);
            auto *loop = static_cast<pa_mainloop *>(device.pulse.pMainLoop);
            auto *operation = pa_context_get_server_info(context,
                [](pa_context *, const pa_server_info *info, void *userdata) {
                    *static_cast<bool *>(userdata) = info && info->server_name &&
                        std::string_view(info->server_name).find("PipeWire") != std::string_view::npos;
                }, &pipewire_backend);
            if (operation) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING &&
                       std::chrono::steady_clock::now() < deadline) {
                    if (pa_mainloop_iterate(loop, 0, nullptr) < 0)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                pa_operation_cancel(operation);
                pa_operation_unref(operation);
            }
        }
        worker = std::thread(&Impl::prepare_loop, this);
    }

    ~Impl() {
        if (device_initialized)
            ma_device_stop(&device);

        {
            std::lock_guard<std::mutex> lock(mutex);
            quitting = true;
        }
        cv.notify_all();

        if (worker.joinable())
            worker.join();

        if (device_initialized)
            ma_device_uninit(&device);
    }

    static void audio_callback(
        ma_device* device,
        void* output,
        const void*,
        ma_uint32 frame_count
    ) {
        auto* self = static_cast<Impl*>(device->pUserData);
        self->render(static_cast<float*>(output), frame_count);
    }

    void render(float* out, ma_uint32 frame_count) {
        std::lock_guard<std::mutex> lock(mutex);

        ma_uint64 written = 0;

        while (written < frame_count) {
            const ma_uint64 remaining = frame_count - written;
            float* dst = out + written * channels;
            ma_uint64 produced = 0;

            switch (mode) {
                case mode_type::current:
                    produced = render_current(dst, remaining);
                    break;

                case mode_type::crossfade:
                    produced = render_crossfade(dst, remaining);
                    break;

                case mode_type::buffered_head:
                    produced = render_buffered_head(dst, remaining);
                    break;

                case mode_type::done:
                    if (next_decoder) {
                        // The next decoder arrived after the previous one ended.
                        // Start from its prefetched beginning rather than dropping it.
                        promote_next(false);
                        continue;
                    }
                    break;
            }

            written += produced;

            if (produced == 0 && mode == mode_type::done)
                break;
        }

        if (written < frame_count) {
            std::memset(
                out + written * channels,
                0,
                sizeof(float) *
                    static_cast<std::size_t>((frame_count - written) * channels)
            );
        }

        apply_output_gain(out, frame_count);
    }

    void apply_output_gain(float* out, ma_uint32 frame_count) {
        const float volume_gain = volume.load(std::memory_order_relaxed);

        for (ma_uint32 frame = 0; frame < frame_count; ++frame) {
            float transport_gain = transport_gain_current;

            if (transport_fade_direction != 0 && transport_fade_remaining != 0) {
                const float remaining =
                    static_cast<float>(transport_fade_remaining) /
                    static_cast<float>(transport_fade_frames);

                transport_gain = transport_fade_direction < 0
                    ? remaining
                    : 1.0f - remaining;

                --transport_fade_remaining;

                if (transport_fade_remaining == 0) {
                    transport_gain_current =
                        transport_fade_direction < 0 ? 0.0f : 1.0f;
                    transport_fade_direction = 0;

                    if (transport_gain_current == 0.0f) {
                        fade_out_complete = true;
                        cv.notify_all();
                    }
                }
            }

            const float gain = volume_gain * transport_gain;

            for (ma_uint32 ch = 0; ch < channels; ++ch)
                out[frame * channels + ch] *= gain;
        }
    }

    ma_uint64 output_and_keep(
        float* out,
        const float* in,
        ma_uint64 count,
        ma_uint64 keep
    ) {
        const ma_uint64 total = tail_count + count;

        if (tail_count != 0) {
            std::memcpy(
                combine.data(),
                tail.data(),
                sizeof(float) *
                    static_cast<std::size_t>(tail_count * channels)
            );
        }

        if (count != 0) {
            std::memcpy(
                combine.data() + tail_count * channels,
                in,
                sizeof(float) * static_cast<std::size_t>(count * channels)
            );
        }

        keep = std::min(keep, total);
        const ma_uint64 output_count = total - keep;

        if (output_count != 0) {
            std::memcpy(
                out,
                combine.data(),
                sizeof(float) *
                    static_cast<std::size_t>(output_count * channels)
            );
        }

        if (keep != 0) {
            std::memcpy(
                tail.data(),
                combine.data() + output_count * channels,
                sizeof(float) * static_cast<std::size_t>(keep * channels)
            );
        }

        tail_count = keep;
        return output_count;
    }

    ma_uint64 render_current(float* out, ma_uint64 count) {
        if (!current_decoder) {
            mode = mode_type::done;
            return 0;
        }

        ma_uint64 total = 0;

        while (total < count && mode == mode_type::current) {
            const ma_uint64 chunk = std::min(count - total, max_chunk);
            ma_uint64 read = 0;

            const ma_result result = ma_decoder_read_pcm_frames(
                &current_decoder->decoder,
                scratch.data(),
                chunk,
                &read
            );

            const bool ended = result == MA_AT_END || read < chunk;

            if (!ended) {
                const ma_uint64 emitted = output_and_keep(
                    out + total * channels,
                    scratch.data(),
                    read,
                    fade_frames
                );

                total += emitted;
                current_time_frames += emitted;
                continue;
            }

            // The buffered tail also needs output space. Drain any excess on the
            // next callback instead of writing beyond this callback's buffer.
            const ma_uint64 available = count - total;
            const ma_uint64 overflow = tail_count + read > available
                ? tail_count + read - available : 0;

            if (next_decoder) {
                const ma_uint64 fade = std::min({
                    fade_frames,
                    tail_count + read,
                    head_count
                });

                const ma_uint64 emitted = output_and_keep(
                    out + total * channels,
                    scratch.data(),
                    read,
                    std::max(fade, overflow)
                );

                total += emitted;
                current_time_frames += emitted;

                if (tail_count > fade)
                    continue;
                if (fade != 0)
                    begin_crossfade();
                else
                    promote_next(true);
            } else {
                const ma_uint64 emitted = output_and_keep(
                    out + total * channels,
                    scratch.data(),
                    read,
                    overflow
                );

                total += emitted;
                current_time_frames += emitted;
                if (tail_count == 0)
                    mode = mode_type::done;
            }
        }

        return total;
    }

    void begin_crossfade() {
        active_fade_frames = std::min(tail_count, head_count);
        fade_position = 0;

        if (active_fade_frames == 0) {
            promote_next(true);
            return;
        }

        if (tail_count > active_fade_frames) {
            const ma_uint64 offset = tail_count - active_fade_frames;

            std::memmove(
                tail.data(),
                tail.data() + offset * channels,
                sizeof(float) *
                    static_cast<std::size_t>(active_fade_frames * channels)
            );

            tail_count = active_fade_frames;
        }

        mode = mode_type::crossfade;
    }

    ma_uint64 render_crossfade(float* out, ma_uint64 count) {
        const ma_uint64 n =
            std::min(count, active_fade_frames - fade_position);

        constexpr float half_pi = 1.57079632679f;

        for (ma_uint64 i = 0; i < n; ++i) {
            const ma_uint64 pos = fade_position + i;

            const float t = active_fade_frames <= 1
                ? 1.0f
                : static_cast<float>(pos) /
                      static_cast<float>(active_fade_frames - 1);

            const float a = std::cos(t * half_pi);
            const float b = std::sin(t * half_pi);

            for (ma_uint32 ch = 0; ch < channels; ++ch) {
                out[i * channels + ch] =
                    tail[pos * channels + ch] * a +
                    head[pos * channels + ch] * b;
            }
        }

        fade_position += n;
        current_time_frames += n;

        if (fade_position == active_fade_frames)
            promote_next(true);

        return n;
    }

    ma_uint64 render_buffered_head(float* out, ma_uint64 count) {
        if (head_play_position >= head_count) {
            finish_buffered_head();
            return 0;
        }

        const ma_uint64 n = std::min(count, head_count - head_play_position);

        std::memcpy(
            out,
            head.data() + head_play_position * channels,
            sizeof(float) * static_cast<std::size_t>(n * channels)
        );

        head_play_position += n;
        current_time_frames += n;

        if (head_play_position == head_count)
            finish_buffered_head();

        return n;
    }

    void finish_buffered_head() {
        head_play_position = 0;
        head_count = 0;
        active_fade_frames = 0;
        fade_position = 0;
        tail_count = 0;
        mode = mode_type::current;
        request_prepare_next_locked();
    }

    void promote_next(bool crossfade_was_rendered) {
        if (!next_decoder) {
            mode = mode_type::done;
            return;
        }

        current_decoder = std::move(next_decoder);
        current_index = next_index;
        current_path = std::move(next_path);
        current_length_frames = next_length_frames;

        next_index = no_index;
        next_length_frames = 0;
        tail_count = 0;

        const ma_uint64 already_played =
            crossfade_was_rendered ? active_fade_frames : 0;

        current_time_frames = already_played;
        head_play_position = already_played;
        active_fade_frames = 0;
        fade_position = 0;

        if (head_play_position < head_count) {
            mode = mode_type::buffered_head;
        } else {
            head_play_position = 0;
            head_count = 0;
            mode = mode_type::current;
            request_prepare_next_locked();
        }
    }

    void invalidate_next_locked() {
        ++prepare_ticket;
        prepare_requested = false;

        next_decoder.reset();
        next_index = no_index;
        next_path.clear();
        next_length_frames = 0;
        head_count = 0;
        head_play_position = 0;
        active_fade_frames = 0;
        fade_position = 0;
    }

    void clear_playback_locked() {
        invalidate_next_locked();

        current_decoder.reset();
        current_index = no_index;
        current_path.clear();
        current_length_frames = 0;
        current_time_frames = 0;
        tail_count = 0;
        mode = mode_type::done;
    }

    bool load_current_locked(
        const std::string& path,
        std::size_t index,
        ma_uint64 seek_frame
    ) {
        prepared_decoder prepared = open_decoder(path, sample_rate, fade_frames, seek_frame);
        if (!prepared.decoder)
            return false;

        invalidate_next_locked();

        current_decoder = std::move(prepared.decoder);
        current_index = index;
        current_path = path;
        current_length_frames = prepared.length_frames;
        current_time_frames = seek_frame;

        tail_count = prepared.prefix_frames;
        if (tail_count != 0) {
            std::memcpy(
                tail.data(),
                prepared.prefix.data(),
                sizeof(float) * static_cast<std::size_t>(tail_count * channels)
            );
        }

        mode = mode_type::current;
        request_prepare_next_locked();
        return true;
    }

    bool load_first_playable_locked() {
        for (std::size_t i = 0; i < live_queue.size(); ++i) {
            if (pending_conversions.contains(live_queue[i])) return false;
            if (load_current_locked(live_queue[i], i, 0)) {
                clear_error_locked();
                return true;
            }

            set_error_locked("Could not open: " + live_queue[i]);
        }

        clear_playback_locked();
        return false;
    }

    void request_prepare_next_locked() {
        ++prepare_ticket;
        prepare_requested = true;
        cv.notify_one();
    }

    void prepare_loop() {
        for (;;) {
            std::uint64_t ticket = 0;
            std::size_t base_index = no_index;
            std::vector<std::pair<std::size_t, std::string>> candidates;

            {
                std::unique_lock<std::mutex> lock(mutex);

                cv.wait(lock, [&] {
                    return quitting || prepare_requested;
                });

                if (quitting)
                    return;

                prepare_requested = false;
                ticket = prepare_ticket;
                base_index = current_index;

                if (current_index != no_index) {
                    for (std::size_t i = current_index + 1; i < live_queue.size(); ++i) {
                        // Preserve album order when the next track is still converting.
                        if (pending_conversions.contains(live_queue[i])) break;
                        candidates.emplace_back(i, live_queue[i]);
                    }
                }
            }

            prepared_decoder prepared;
            std::size_t prepared_index = no_index;
            std::string prepared_path;

            for (const auto& candidate : candidates) {
                prepared = open_decoder(candidate.second, sample_rate, fade_frames);

                if (prepared.decoder) {
                    prepared_index = candidate.first;
                    prepared_path = candidate.second;
                    break;
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex);

                if (quitting)
                    return;

                if (ticket != prepare_ticket || base_index != current_index)
                    continue;

                next_decoder.reset();
                next_index = no_index;
                next_path.clear();
                next_length_frames = 0;
                head_count = 0;

                if (prepared.decoder) {
                    next_decoder = std::move(prepared.decoder);
                    next_index = prepared_index;
                    next_path = std::move(prepared_path);
                    next_length_frames = prepared.length_frames;
                    head_count = prepared.prefix_frames;

                    if (head_count != 0) {
                        std::memcpy(
                            head.data(),
                            prepared.prefix.data(),
                            sizeof(float) *
                                static_cast<std::size_t>(head_count * channels)
                        );
                    }
                }
            }
        }
    }

    bool start_device() {
        if (!device_initialized)
            return false;

        {
            std::lock_guard<std::mutex> lock(mutex);
            transport_gain_current = 0.0f;
            transport_fade_direction = 1;
            transport_fade_remaining = transport_fade_frames;
            fade_out_complete = false;
        }

        if (ma_device_start(&device) != MA_SUCCESS) {
            set_error("Failed to start miniaudio playback device.");
            return false;
        }

        playing.store(true, std::memory_order_release);
        return true;
    }

    void pause_device() {
        if (!device_initialized ||
            !playing.load(std::memory_order_acquire)) {
            return;
        }

        {
            std::unique_lock<std::mutex> lock(mutex);

            transport_fade_direction = -1;
            transport_fade_remaining = transport_fade_frames;
            fade_out_complete = false;

            cv.wait(lock, [&] {
                return fade_out_complete;
            });
        }

        ma_device_stop(&device);
        playing.store(false, std::memory_order_release);
    }

    bool start(const std::vector<std::string>& editable_queue) {
        if (!device_initialized)
            return false;

        if (playing.load(std::memory_order_acquire))
            return true;

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (!current_decoder) {
                live_queue = editable_queue;

                if (live_queue.empty()) {
                    set_error_locked("The queue is empty.");
                    return false;
                }

                if (!load_first_playable_locked())
                    return false;
            }
        }

        return start_device();
    }

    void pause() {
        pause_device();
    }

    void stop() {
        pause_device();

        std::lock_guard<std::mutex> lock(mutex);

        if (!current_decoder || current_index == no_index)
            return;

        if (!load_current_locked(current_path, current_index, 0))
            set_error_locked("Failed to reset current file: " + current_path);
        else
            clear_error_locked();
    }

    bool seek_to_frame(ma_uint64 target_frame) {
        if (!device_initialized)
            return false;

        const bool was_playing = playing.load(std::memory_order_acquire);
        if (was_playing)
            pause_device();

        bool ok = false;

        {
            std::lock_guard<std::mutex> lock(mutex);

            if (!current_decoder || current_index == no_index) {
                set_error_locked("There is no current track to seek.");
            } else if (current_length_frames == 0) {
                set_error_locked("Could not determine the current track length.");
            } else {
                target_frame = std::min(target_frame, current_length_frames);

                const std::string path = current_path;
                const std::size_t index = current_index;

                ok = load_current_locked(path, index, target_frame);

                if (ok)
                    clear_error_locked();
                else
                    set_error_locked("Failed to seek current file: " + path);
            }
        }

        if (was_playing && !start_device())
            return false;

        return ok;
    }

    bool seek(float position) {
        position = std::clamp(position, 0.0f, 1.0f);

        ma_uint64 length = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            length = current_length_frames;
        }

        if (length == 0) {
            set_error("Could not determine the current track length.");
            return false;
        }

        const ma_uint64 target_frame = static_cast<ma_uint64>(std::llround(
            static_cast<double>(length) * static_cast<double>(position)
        ));

        return seek_to_frame(target_frame);
    }

    bool seek_relative_seconds(double seconds) {
        ma_uint64 current = 0;
        ma_uint64 length = 0;

        {
            std::lock_guard<std::mutex> lock(mutex);
            current = current_time_frames;
            length = current_length_frames;
        }

        if (length == 0) {
            set_error("Could not determine the current track length.");
            return false;
        }

        const double delta_frames =
            seconds * static_cast<double>(sample_rate);

        const double target = std::clamp(
            static_cast<double>(current) + delta_frames,
            0.0,
            static_cast<double>(length)
        );

        return seek_to_frame(static_cast<ma_uint64>(std::llround(target)));
    }

    bool play_queue_index(
        const std::vector<std::string>& editable_queue,
        std::size_t index
    ) {
        if (!device_initialized)
            return false;

        if (index >= editable_queue.size()) {
            set_error("Track index is outside the queue.");
            return false;
        }

        const bool was_playing = playing.load(std::memory_order_acquire);
        if (was_playing)
            pause_device();

        bool ok = false;

        {
            std::lock_guard<std::mutex> lock(mutex);
            live_queue = editable_queue;

            ok = load_current_locked(live_queue[index], index, 0);

            if (ok)
                clear_error_locked();
            else
                set_error_locked("Could not open: " + live_queue[index]);
        }

        if (!ok) {
            if (was_playing)
                start_device();
            return false;
        }

        return start_device();
    }

    bool restore_session(const std::vector<std::string> &tracks, std::size_t index, double seconds) {
        pause_device();
        std::lock_guard<std::mutex> lock(mutex);
        clear_playback_locked();
        live_queue = tracks;
        if (tracks.empty())
            return true;
        if (index >= tracks.size())
            index = 0;
        if (!load_current_locked(tracks[index], index, 0)) {
            if (!load_first_playable_locked()) {
                set_error_locked("Could not restore any queued track.");
                return false;
            }
            seconds = 0;
        }
        if (std::isfinite(seconds) && seconds > 0 && current_length_frames > 0) {
            const double frames = std::min(seconds * sample_rate, static_cast<double>(current_length_frames - 1));
            // Keep the successfully opened track at its beginning if seeking fails.
            load_current_locked(current_path, current_index, static_cast<ma_uint64>(frames));
        }
        clear_error_locked();
        return true;
    }

    void queue_changed(const std::vector<std::string>& editable_queue) {
        if (!device_initialized)
            return;

        // Common case: only future queue items changed. The currently playing
        // item is still at the same index, so no decoder/device restart is needed.
        {
            std::lock_guard<std::mutex> lock(mutex);

            if (current_decoder &&
                current_index < editable_queue.size() &&
                editable_queue[current_index] == current_path) {
                live_queue = editable_queue;
                invalidate_next_locked();
                request_prepare_next_locked();
                clear_error_locked();
                return;
            }
        }

        const bool was_playing = playing.load(std::memory_order_acquire);
        if (was_playing)
            pause_device();

        {
            std::lock_guard<std::mutex> lock(mutex);
            live_queue = editable_queue;

            if (live_queue.empty()) {
                clear_playback_locked();
                clear_error_locked();
            } else if (current_decoder) {
                std::size_t new_index = no_index;

                const auto it = std::find(
                    live_queue.begin(),
                    live_queue.end(),
                    current_path
                );

                if (it != live_queue.end()) {
                    new_index = static_cast<std::size_t>(
                        std::distance(live_queue.begin(), it)
                    );
                }

                ma_uint64 resume_frame = current_time_frames;

                if (new_index == no_index) {
                    new_index = std::min(current_index, live_queue.size() - 1);
                    resume_frame = 0;
                }

                if (!load_current_locked(
                        live_queue[new_index],
                        new_index,
                        resume_frame
                    )) {
                    if (!load_first_playable_locked())
                        set_error_locked("No playable files remain in the queue.");
                } else {
                    clear_error_locked();
                }
            } else {
                load_first_playable_locked();
            }
        }

        if (was_playing) {
            bool have_current = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                have_current = static_cast<bool>(current_decoder);
            }

            if (have_current)
                start_device();
        }
    }

    std::string get_current_path() const {
        std::lock_guard<std::mutex> lock(mutex);
        return current_path;
    }

    std::size_t get_current_index() const {
        std::lock_guard<std::mutex> lock(mutex);
        return current_index;
    }

    double get_current_time_seconds() const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<double>(current_time_frames) /
               static_cast<double>(sample_rate);
    }

    double get_total_time_seconds() const {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<double>(current_length_frames) /
               static_cast<double>(sample_rate);
    }

    float get_seek_position() const {
        std::lock_guard<std::mutex> lock(mutex);

        if (current_length_frames == 0)
            return 0.0f;

        const double position =
            static_cast<double>(current_time_frames) /
            static_cast<double>(current_length_frames);

        return static_cast<float>(std::clamp(position, 0.0, 1.0));
    }

    std::string get_last_error() const {
        std::lock_guard<std::mutex> lock(mutex);
        return error;
    }

    void set_error(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        set_error_locked(text);
    }

    void set_error_locked(const std::string& text) {
        error = text;
    }

    void clear_error_locked() {
        error.clear();
    }

    ma_device device{};
    bool device_initialized = false;
    bool pipewire_backend = false;

    const ma_uint32 sample_rate;
    const ma_uint64 transport_fade_frames;
    const double crossfade_ms;
    const ma_uint64 fade_frames;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::thread worker;
    bool quitting = false;

    std::vector<std::string> live_queue;
    std::unordered_set<std::string> pending_conversions;

    std::unique_ptr<decoder_handle> current_decoder;
    std::unique_ptr<decoder_handle> next_decoder;

    std::size_t current_index = no_index;
    std::size_t next_index = no_index;

    std::string current_path;
    std::string next_path;

    ma_uint64 current_length_frames = 0;
    ma_uint64 next_length_frames = 0;
    ma_uint64 current_time_frames = 0;

    std::vector<float> tail;
    std::vector<float> head;
    std::vector<float> combine;
    std::vector<float> scratch;

    ma_uint64 tail_count = 0;
    ma_uint64 head_count = 0;
    ma_uint64 head_play_position = 0;

    ma_uint64 active_fade_frames = 0;
    ma_uint64 fade_position = 0;

    mode_type mode = mode_type::done;

    std::uint64_t prepare_ticket = 0;
    bool prepare_requested = false;

    std::atomic<float> volume{1.0f};
    std::atomic<bool> playing{false};

    float transport_gain_current = 1.0f;
    int transport_fade_direction = 0;
    ma_uint64 transport_fade_remaining = 0;
    bool fade_out_complete = false;

    std::string error;
};


struct Player::Conversion {
    struct Job {
        std::string path;
        std::future<AudioConversionResult> result;
        std::optional<AudioConversionResult> outcome;
        std::atomic<double> seconds{0};
        bool finished = false;
        std::string error;
    };
    // Only the UI thread edits jobs and the requested playback action.
    std::vector<std::shared_ptr<Job>> jobs;
    std::string requested_path;
    int action = 0; // none, start, select, restore, restore and start
    std::size_t index = 0;
    double seconds = 0;
    std::string error;
};

Player::Player(double crossfade_ms)
    : impl_(std::make_unique<Impl>(std::max(0.0, crossfade_ms))) {
}

unsigned Player::sample_rate() const noexcept {
    return impl_->sample_rate;
}

bool Player::uses_pipewire() const noexcept {
    return impl_->pipewire_backend;
}

bool Player::set_sample_rate(unsigned rate) {
    if (rate < 8000 || rate > 384000) {
        impl_->set_error("Sample rate must be between 8000 and 384000 Hz.");
        return false;
    }
    if (rate == sample_rate())
        return true;
    auto candidate = std::make_unique<Impl>(impl_->crossfade_ms, rate);
    if (!candidate->device_initialized) {
        impl_->set_error("Could not open audio output at " + std::to_string(rate) + " Hz. Previous rate retained.");
        return false;
    }
    const bool playing = is_playing();
    pause();
    const auto position = playback_position();
    candidate->volume.store(volume());
    if (!candidate->restore_session(queue_, position.index, position.seconds) ||
        (playing && !candidate->start(queue_))) {
        const auto error = candidate->get_last_error();
        candidate.reset();
        if (playing)
            start();
        impl_->set_error("Could not change output rate: " + error);
        return false;
    }
    impl_.swap(candidate);
    sync_conversion_queue();
    return true;
}

void Player::sync_conversion_queue() {
    std::lock_guard lock(impl_->mutex);
    impl_->pending_conversions.clear();
    if (conversion_) {
        for (const auto &job : conversion_->jobs)
            if (!job->finished) impl_->pending_conversions.insert(job->path);
    }
}

bool Player::prepare_conversion(int action, std::size_t index, double seconds) {
    if (!conversion_) conversion_ = std::make_unique<Conversion>();
    auto &c = *conversion_;
    c.error.clear();
    if (std::all_of(c.jobs.begin(), c.jobs.end(), [](const auto &job) { return job->finished; }))
        c.jobs.clear();

    if (action == 1 && c.action != 0) {
        // Resume a pending selection/restoration at its requested position.
        action = c.action == 3 ? 4 : c.action;
        index = c.index;
        seconds = c.seconds;
    } else if (action == 1) {
        index = current_index();
        if (index >= queue_.size()) index = 0;
    }
    if (index >= queue_.size()) index = 0;
    for (auto &path : queue_) {
        path = preferred_audio_path(path);
        const auto existing = std::find_if(c.jobs.begin(), c.jobs.end(),
            [&](const auto &job) { return job->path == path; });
        if (existing != c.jobs.end() || !needs_audio_conversion(path)) continue;
        auto job = std::make_shared<Conversion::Job>();
        job->path = path;
        c.jobs.push_back(std::move(job));
    }

    if (action != 0) {
        // Prepare the newly selected album in playback order before older queued work.
        // Running jobs keep their objects and continue uninterrupted.
        std::vector<std::shared_ptr<Conversion::Job>> ordered;
        ordered.reserve(c.jobs.size());
        for (std::size_t offset = 0; offset < queue_.size(); ++offset) {
            const auto &path = queue_[(index + offset) % queue_.size()];
            const auto job = std::find_if(c.jobs.begin(), c.jobs.end(),
                [&](const auto &entry) { return entry->path == path; });
            if (job != c.jobs.end() && std::find(ordered.begin(), ordered.end(), *job) == ordered.end()) {
                if ((*job)->finished && !(*job)->error.empty()) {
                    (*job)->finished = false;
                    (*job)->error.clear();
                    (*job)->seconds.store(0);
                }
                ordered.push_back(*job);
            }
        }
        for (const auto &job : c.jobs)
            if (std::find(ordered.begin(), ordered.end(), job) == ordered.end()) ordered.push_back(job);
        c.jobs = std::move(ordered);
    }

    bool waiting = false;
    if (action != 0) {
        c.action = 0;
        c.requested_path.clear();
        if (!queue_.empty()) {
            const auto selected = std::find_if(c.jobs.begin(), c.jobs.end(), [&](const auto &job) {
                return job->path == queue_[index] && !job->finished;
            });
            if (selected != c.jobs.end()) {
                c.action = action;
                c.index = index;
                c.seconds = seconds;
                c.requested_path = queue_[index];
                // A newly selected track goes ahead of queued background work.
                std::rotate(c.jobs.begin(), selected, selected + 1);
                waiting = true;
            }
        }
    } else if (c.action != 0) {
        // Queue edits retain a pending selection only while it is still present.
        if (c.index >= queue_.size() || queue_[c.index] != c.requested_path) {
            const auto it = std::find(queue_.begin(), queue_.end(), c.requested_path);
            if (it == queue_.end()) c.action = 0;
            else c.index = static_cast<std::size_t>(it - queue_.begin());
        }
        waiting = c.action != 0;
    }
    sync_conversion_queue();
    launch_conversions();
    return waiting;
}

void Player::launch_conversions() {
    if (!conversion_) return;
    auto &c = *conversion_;
    std::size_t running = 0;
    for (const auto &job : c.jobs) if (job->result.valid()) ++running;
    for (const auto &job : c.jobs) {
        // Keep a slot available for a newly selected album while background work continues.
        if (running >= (c.action != 0 ? 2u : 1u)) break;
        if (job->finished || job->result.valid() || job->outcome) continue;
        try {
            job->result = std::async(std::launch::async, [job = job.get()] {
                return convert_to_flac(job->path, [job](double elapsed) { job->seconds.store(elapsed); });
            });
            ++running;
        } catch (...) {
            // Optional background work must not disrupt playback if a thread cannot start.
            job->outcome = AudioConversionResult{{}, "Automatic conversion is temporarily unavailable."};
        }
    }
}

Player::ConversionProgress Player::conversion_progress() const {
    ConversionProgress progress;
    if (!conversion_) return progress;
    const auto &c = *conversion_;
    progress.error = c.error;
    progress.total = c.jobs.size();
    std::shared_ptr<Conversion::Job> displayed;
    for (const auto &job : c.jobs) {
        if (job->finished) ++progress.track;
        else {
            progress.active = true;
            if (job->result.valid() && (!displayed || job->path == c.requested_path)) displayed = job;
        }
    }
    if (progress.active) ++progress.track;
    if (displayed) {
        progress.path = displayed->path;
        progress.seconds = displayed->seconds.load();
    }
    return progress;
}

bool Player::poll_conversion() {
    if (!conversion_) return false;
    auto &c = *conversion_;
    bool completed = false, queue_updated = false, selected_ready = false, selected_failed = false;
    for (const auto &job : c.jobs) {
        if (!job->outcome && (!job->result.valid() ||
            job->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) continue;
        completed = true;
        job->finished = true;
        AudioConversionResult result;
        if (job->outcome) {
            result = std::move(*job->outcome);
            job->outcome.reset();
        } else {
            // Contain unexpected worker failures too; ordinary conversion failures
            // arrive as status values and never use exceptions for control flow.
            try { result = job->result.get(); }
            catch (...) { result.error = "Automatic conversion could not finish."; }
        }
        if (result) {
            const auto &path = result.path;
            for (auto &queued : queue_) {
                if (queued == job->path) { queued = path; queue_updated = true; }
            }
            if (c.action != 0 && c.requested_path == job->path) {
                selected_ready = c.index < queue_.size() && queue_[c.index] == path;
                if (!selected_ready) c.action = 0;
            }
        } else {
            job->error = c.error = result.error;
            if (std::find(queue_.begin(), queue_.end(), job->path) != queue_.end()) {
                impl_->set_error(c.error);
                queue_updated = true;
            }
            if (c.action != 0 && c.requested_path == job->path) {
                selected_failed = true;
                c.action = 0;
            }
        }
    }
    if (completed) sync_conversion_queue();
    if (selected_ready) {
        const auto action = std::exchange(c.action, 0);
        bool ok;
        if (action == 3 || action == 4) {
            ok = impl_->restore_session(queue_, c.index, c.seconds);
            if (ok && action == 4) ok = impl_->start(queue_);
        } else ok = impl_->play_queue_index(queue_, c.index);
        if (!ok) c.error = impl_->get_last_error();
    } else if (completed && c.action == 0 && !selected_failed && queue_updated) {
        // Updating future paths preserves the current decoder and playback position.
        impl_->queue_changed(queue_);
    }
    launch_conversions();
    return completed;
}

Player::~Player() {
    if (conversion_) {
        for (const auto &job : conversion_->jobs)
            if (job->result.valid()) job->result.wait();
    }
}

std::vector<std::string>& Player::queue() noexcept {
    return queue_;
}

const std::vector<std::string>& Player::queue() const noexcept {
    return queue_;
}

void Player::queue_changed() {
    if (!prepare_conversion(0)) impl_->queue_changed(queue_);
}

bool Player::start() {
    if (prepare_conversion(1)) return true;
    return impl_->start(queue_);
}

void Player::pause() {
    if (conversion_ && conversion_->action != 0) conversion_->action = 3;
    impl_->pause();
}

void Player::stop() {
    if (conversion_) conversion_->action = 0;
    impl_->stop();
}

bool Player::is_playing() const noexcept {
    return impl_->playing.load(std::memory_order_acquire);
}

bool Player::seek(float position) {
    return impl_->seek(position);
}

bool Player::seek_relative_seconds(double seconds) {
    return impl_->seek_relative_seconds(seconds);
}

bool Player::seek_to_start() {
    return seek(0.0f);
}

float Player::seek_position() const {
    return impl_->get_seek_position();
}

bool Player::play_queued_item(std::size_t index) {
    if (index >= queue_.size()) return false;
    if (prepare_conversion(2, index)) return true;
    return impl_->play_queue_index(queue_, index);
}

bool Player::restore_session(std::vector<std::string> tracks, std::size_t index, double seconds) {
    queue_ = std::move(tracks);
    if (prepare_conversion(3, index, seconds)) return true;
    return impl_->restore_session(queue_, index, seconds);
}

Player::Position Player::playback_position() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->current_path, impl_->current_index,
        static_cast<double>(impl_->current_time_frames) / impl_->sample_rate};
}

bool Player::play_track(const std::string& path) {
    const std::size_t playing_index = current_index();
    const std::size_t insert_index = playing_index == no_index
        ? 0
        : std::min(playing_index + 1, queue_.size());

    queue_.insert(
        queue_.begin() + static_cast<std::ptrdiff_t>(insert_index),
        path
    );

    queue_changed();

    if (play_queued_item(insert_index))
        return true;

    // Do not leave a failed force-play insertion in the public queue.
    queue_.erase(
        queue_.begin() + static_cast<std::ptrdiff_t>(insert_index)
    );
    queue_changed();
    return false;
}

void Player::set_volume(float value) noexcept {
    impl_->volume.store(
        std::clamp(value, 0.0f, 1.0f),
        std::memory_order_release
    );
}

float Player::volume() const noexcept {
    return impl_->volume.load(std::memory_order_acquire);
}

std::string Player::current_path() const {
    return impl_->get_current_path();
}

std::size_t Player::current_index() const {
    return impl_->get_current_index();
}

double Player::current_time_seconds() const {
    return impl_->get_current_time_seconds();
}

double Player::total_time_seconds() const {
    return impl_->get_total_time_seconds();
}

std::string Player::last_error() const {
    return impl_->get_last_error();
}
