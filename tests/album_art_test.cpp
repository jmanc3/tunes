#include "album_art.h"
#include "audio_data.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace std::chrono_literals;
static std::vector<unsigned char> embedded;
static std::atomic<int> reads{0};
static std::mutex gate_mutex;
static std::condition_variable gate;
static bool blocked = false;

// The file-reading boundary is controlled so we can hold workers in extraction
// while proving UI requests and texture snapshots still return immediately.
EmbeddedArtwork read_album_art(const std::string &path) {
    ++reads;
    std::unique_lock lock(gate_mutex);
    gate.wait_for(lock, 5s, [] { return !blocked; });
    if (path.ends_with("missing.flac"))
        return {};
    return {embedded, ".png"};
}

static void check(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<typename Predicate>
static void wait_for(Predicate ready) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!ready()) {
        check(std::chrono::steady_clock::now() < end, "artwork job timed out");
        std::this_thread::sleep_for(2ms);
    }
}

static std::vector<unsigned char> bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

int main() {
    auto temporary = g_dir_make_tmp("tunes-cache-test-XXXXXX", nullptr);
    check(temporary, "temporary directory creation failed");
    const fs::path base(temporary);
    g_free(temporary);
    try {
        auto original = gdk_pixbuf_new(GDK_COLORSPACE_RGB, true, 8, 1600, 1200);
        gdk_pixbuf_fill(original, 0x33669980);
        gchar *buffer = nullptr;
        gsize length = 0;
        check(gdk_pixbuf_save_to_buffer(original, &buffer, &length, "png", nullptr, nullptr), "encode fixture");
        embedded.assign(buffer, buffer + length);
        g_free(buffer);
        g_object_unref(original);
        const auto track = base / "song.flac";
        std::ofstream(track) << "track";
        const auto cache_path = base / "cache";
        fs::path album_path;

        {
            AlbumArtCache cache(cache_path);
            auto entry = cache.create({track.string()});
            blocked = true;
            cache.request(entry, 256);
            wait_for([] { return reads.load() > 0; });
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 1000; ++i) {
                cache.request(entry, 256);
                check(!cache.image(entry), "image published before decoding");
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            check(elapsed < 250ms, "UI calls waited for extraction");
            std::cout << "1000 requests during blocked extraction: "
                      << std::chrono::duration<double, std::milli>(elapsed).count() << " ms\n";
            {
                std::lock_guard lock(gate_mutex);
                blocked = false;
            }
            gate.notify_all();
            wait_for([&] { return !cache.pending(); });
            auto image = cache.image(entry);
            check(image && image->width == 256 && image->height == 192, "display texture dimensions");
            check(reads == 1, "duplicate extraction jobs");
            album_path = fs::directory_iterator(cache_path)->path();
            check(bytes(album_path / "original.img") == embedded, "original artwork bytes changed");
            auto full = gdk_pixbuf_new_from_file((album_path / "full.png").c_str(), nullptr);
            check(full && gdk_pixbuf_get_width(full) == 1600 && gdk_pixbuf_get_height(full) == 1200,
                  "lossless cache was downscaled");
            const auto pixel = gdk_pixbuf_get_pixels(full);
            check(pixel[0] == 0x33 && pixel[1] == 0x66 && pixel[2] == 0x99 && pixel[3] == 0x80,
                  "lossless cache changed pixels");
            g_object_unref(full);
            cache.release(entry);
            check(cache.image(entry)->pixels == 128, "offscreen detail texture retained");
        }

        {
            AlbumArtCache cache(cache_path);
            auto entry = cache.create({track.string()});
            const auto start = std::chrono::steady_clock::now();
            cache.request(entry, 0);
            wait_for([&] { return !cache.pending(); });
            check(cache.image(entry) && cache.image(entry)->pixels == 128, "warm preview load");
            check(reads == 1, "warm cache re-extracted source");
            std::cout << "Warm preview ready: " << std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count() << " ms\n";
            // A corrupt large cache should recover using the untouched original.
            std::ofstream(album_path / "full.png", std::ios::trunc) << "broken";
            cache.request(entry, 512);
            wait_for([&] { return !cache.pending(); });
            check(cache.image(entry)->width == 512, "large cache recovery");
            check(reads == 1, "recovery ignored original byte cache");
            cache.request(entry, 1024);
            cache.release(entry);
            cache.request(entry, 384);
            wait_for([&] {
                cache.request(entry, 384);
                return !cache.pending();
            });
            check(cache.image(entry)->width == 384, "resize/release race published a stale texture");
        }

        std::ofstream(track, std::ios::app) << "changed";
        {
            AlbumArtCache cache(cache_path);
            auto entry = cache.create({track.string()});
            cache.request(entry, 256);
            wait_for([&] { return !cache.pending(); });
            check(cache.image(entry) && reads == 2, "source changes did not invalidate cache");
        }

        const auto missing = base / "missing.flac";
        std::ofstream(missing) << "no artwork";
        for (int i = 0; i < 2; ++i) {
            AlbumArtCache cache(cache_path);
            auto entry = cache.create({missing.string()});
            cache.request(entry, 256);
            wait_for([&] { return !cache.pending(); });
            check(!cache.image(entry), "missing artwork should use a placeholder");
        }
        check(reads == 3, "missing artwork was repeatedly extracted");

        // Cache-write failure must still produce a texture for this session.
        std::ofstream(base / "not-a-directory") << "file";
        {
            AlbumArtCache cache(base / "not-a-directory");
            auto entry = cache.create({track.string()});
            cache.request(entry, 256);
            wait_for([&] { return !cache.pending(); });
            check(cache.image(entry) != nullptr, "cache-write failure lost artwork");
        }
        {
            AlbumArtCache cache(base / "shutdown");
            for (int i = 0; i < 100; ++i)
                cache.request(cache.create({track.string()}), 256);
            // Destruction cancels queued work; workers never access UI objects.
        }
        fs::remove_all(base);
        std::cout << "Album artwork cache checks passed\n";
    } catch (...) {
        {
            std::lock_guard lock(gate_mutex);
            blocked = false;
        }
        gate.notify_all();
        fs::remove_all(base);
        throw;
    }
}
