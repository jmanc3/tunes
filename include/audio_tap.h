#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

// Single-producer/single-consumer float ring for copying audio off the
// real-time thread. The audio callback writes; one other thread reads. Neither
// side locks or allocates. When the reader falls behind, new samples are
// dropped rather than overwriting unread ones.
class AudioTap {
public:
    explicit AudioTap(std::size_t capacity_pow2 = 1u << 15)
        : buffer_(capacity_pow2), mask_(capacity_pow2 - 1) {}

    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_release); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Producer side.
    void write(const float *samples, std::size_t count) noexcept {
        if (!enabled()) return;
        const auto head = head_.load(std::memory_order_relaxed);
        const auto tail = tail_.load(std::memory_order_acquire);
        count = std::min(count, buffer_.size() - (head - tail));
        for (std::size_t i = 0; i < count; ++i)
            buffer_[(head + i) & mask_] = samples[i];
        head_.store(head + count, std::memory_order_release);
    }

    // Consumer side.
    std::size_t read(float *out, std::size_t max) noexcept {
        const auto tail = tail_.load(std::memory_order_relaxed);
        const auto head = head_.load(std::memory_order_acquire);
        const auto count = std::min(max, head - tail);
        for (std::size_t i = 0; i < count; ++i)
            out[i] = buffer_[(tail + i) & mask_];
        tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    // Consumer side: discards everything written so far.
    void clear() noexcept { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release); }

private:
    std::vector<float> buffer_;
    std::size_t mask_;
    std::atomic<bool> enabled_{false};
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};
