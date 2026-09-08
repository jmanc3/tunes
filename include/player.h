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

    // Starts playback or resumes after pause().
    // First start is raw. Resuming the same track after pause() fades in.
    bool start();

    // Pauses the same track with a short fade-out to avoid a click.
    void pause();

    // Hard-stops and returns the current song to 0:00.
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

    // Immediately jumps to an existing queue item and starts it.
    // Track changes are hard cuts: no fade-out, crossfade or fade-in.
    bool play_queued_item(std::size_t index);

    // Immediately plays the next queued item.
    // If a track is currently audible, it is briefly faded to silence first
    // to avoid a click; the next track then hard-starts with no fade-in.
    // If idle, the next/first queued item starts raw immediately.
    bool play_next();

    // Force-plays a path now while preserving queue continuation.
    //
    // Example, while album_1 is playing:
    //   [album_1, album_2, album_3]
    //   play_track("single.flac")
    // becomes:
    //   [album_1, single.flac, album_2, album_3]
    //
    // single.flac starts immediately with a hard cut, then album_2 follows normally.
    bool play_track(const std::string& path);

    // Linear gain: 0.0 = silent, 1.0 = normal.
    void set_volume(float volume) noexcept;
    float volume() const noexcept;

    std::string current_path() const;
    std::size_t current_index() const;

    double current_time_seconds() const;
    double total_time_seconds() const;

    std::string last_error() const;

private:
    struct Impl;

    std::vector<std::string> queue_;
    std::unique_ptr<Impl> impl_;
};
