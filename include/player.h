#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

class Player {
public:
    explicit Player(double crossfade_ms = 5.0);
    ~Player();

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;
    Player(Player&&) = delete;
    Player& operator=(Player&&) = delete;

    // Edit this vector however you want, then call queue_changed().
    std::vector<std::string>& queue() noexcept;
    const std::vector<std::string>& queue() const noexcept;

    // Synchronizes queue() with playback.
    // If the current item is unchanged, playback is not interrupted.
    void queue_changed();

    // Starts playback or resumes after pause(). Format conversion is asynchronous;
    // true means accepted. Call poll_conversion() regularly to finish preparation.
    bool start();

    // Smoothly fades out and pauses at the current position.
    void pause();

    // Pauses and returns the current song to 0:00.
    void stop();

    bool is_playing() const noexcept;

    // Normalized seek. position is clamped to [0.0, 1.0].
    bool seek(float position);

    // Moves relative to the current position, clamped to the song bounds.
    // Examples:
    //   player.seek_relative_seconds(10.0);   // +10 seconds
    //   player.seek_relative_seconds(-10.0);  // -10 seconds
    bool seek_relative_seconds(double seconds);

    bool seek_to_start();

    // Current normalized position in [0.0, 1.0].
    // Returns 0 when there is no current song or its length is unknown.
    float seek_position() const;

    // Jumps to a queue item as soon as it is ready; other conversions continue in the background.
    bool play_queued_item(std::size_t index);

    // Loads a listening session without ever starting the audio device.
    bool restore_session(std::vector<std::string> tracks, std::size_t index, double seconds);
    struct Position {
        std::string path;
        std::size_t index;
        double seconds;
    };
    Position playback_position() const;

    // Force-plays a path now while preserving queue continuation.
    //
    // Example, while album_1 is playing:
    //   [album_1, album_2, album_3]
    //   play_track("single.flac")
    // becomes:
    //   [album_1, single.flac, album_2, album_3]
    //
    // single.flac starts immediately, then album_2 follows normally.
    bool play_track(const std::string& path);

    struct ConversionProgress {
        bool active = false;
        std::string path;
        std::string error;
        std::size_t track = 0, total = 0;
        double seconds = 0;
    };
    ConversionProgress conversion_progress() const;
    // Called on the UI thread to apply completed background preparation.
    bool poll_conversion();

    // Linear gain: 0.0 = silent, 1.0 = normal.
    void set_volume(float volume) noexcept;
    float volume() const noexcept;

    // Reconfigures output and decoders, preserving playback; failure keeps the old rate.
    bool set_sample_rate(unsigned rate);
    unsigned sample_rate() const noexcept;
    bool uses_pipewire() const noexcept;

    std::string current_path() const;
    std::size_t current_index() const;

    double current_time_seconds() const;
    double total_time_seconds() const;

    std::string last_error() const;

private:
    struct Impl;
    struct Conversion;
    bool prepare_conversion(int action, std::size_t index = 0, double seconds = 0);
    void launch_conversions();
    void sync_conversion_queue();
    std::unique_ptr<Conversion> conversion_;

    std::vector<std::string> queue_;
    std::unique_ptr<Impl> impl_;
};
