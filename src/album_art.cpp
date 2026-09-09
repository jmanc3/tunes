#include "tunes_paths.h"
#include "album_art.h"
#include "audio_data.h"
#include "ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <gdk/gdk.h>
#include <set>
#include <map>
#include <unistd.h>

namespace {
namespace fs = std::filesystem;
constexpr int preview_pixels = 128;
using Pixbuf = std::unique_ptr<GdkPixbuf, decltype(&g_object_unref)>;

Pixbuf load_image(const fs::path &path, int pixels = 0) {
    return Pixbuf(pixels > 0
        ? gdk_pixbuf_new_from_file_at_scale(path.c_str(), pixels, pixels, true, nullptr)
        : gdk_pixbuf_new_from_file(path.c_str(), nullptr), g_object_unref);
}

Pixbuf decode(const std::vector<unsigned char> &bytes) {
    auto loader = gdk_pixbuf_loader_new();
    const bool written = gdk_pixbuf_loader_write(loader, bytes.data(), bytes.size(), nullptr);
    const bool closed = gdk_pixbuf_loader_close(loader, nullptr);
    auto image = written && closed ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
    if (image)
        g_object_ref(image);
    g_object_unref(loader);
    return Pixbuf(image, g_object_unref);
}

Pixbuf resized(GdkPixbuf *image, int pixels) {
    const int width = gdk_pixbuf_get_width(image), height = gdk_pixbuf_get_height(image);
    const double scale = std::min(1.0, static_cast<double>(pixels) / std::max(width, height));
    return Pixbuf(gdk_pixbuf_scale_simple(image, std::max(1, static_cast<int>(width * scale)),
        std::max(1, static_cast<int>(height * scale)), GDK_INTERP_BILINEAR), g_object_unref);
}

std::shared_ptr<const AlbumTexture> texture(GdkPixbuf *image, int pixels) {
    auto result = std::make_shared<AlbumTexture>();
    result->width = gdk_pixbuf_get_width(image);
    result->height = gdk_pixbuf_get_height(image);
    result->pixels = pixels;
    result->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, result->width, result->height);
    auto cr = cairo_create(result->surface);
    gdk_cairo_set_source_pixbuf(cr, image, 0, 0);
    cairo_paint(cr);
    const bool ok = cairo_status(cr) == CAIRO_STATUS_SUCCESS;
    cairo_destroy(cr);
    cairo_surface_flush(result->surface);
    return ok ? result : nullptr;
}

// A unique staging file and rename prevent partial cache entries, including
// when two Tunes processes populate the same album at once.
template<typename Write>
void atomic_write(const fs::path &path, Write write) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error)
        return;
    auto staging = path.string() + ".XXXXXX";
    const int fd = g_mkstemp(staging.data());
    if (fd < 0)
        return;
    close(fd);
    if (write(staging))
        fs::rename(staging, path, error);
    fs::remove(staging, error);
}

void save_png(GdkPixbuf *image, const fs::path &path) {
    atomic_write(path, [&](const std::string &staging) {
        return gdk_pixbuf_save(image, staging.c_str(), "png", nullptr, "compression", "1", nullptr);
    });
}

void save_bytes(const std::vector<unsigned char> &bytes, const fs::path &path) {
    atomic_write(path, [&](const std::string &staging) {
        std::ofstream out(staging, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close();
        return static_cast<bool>(out);
    });
}

std::vector<fs::path> covers(const std::vector<std::string> &tracks) {
    std::vector<fs::path> result;
    std::set<fs::path> seen;
    for (const auto &track : tracks) {
        auto folder = fs::path(track).parent_path();
        for (const auto &dir : {folder, folder.parent_path()}) {
            if (!seen.insert(dir).second)
                continue;
            for (const auto *name : {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg"})
                result.push_back(dir / name);
        }
    }
    return result;
}

std::string fingerprint(const std::vector<std::string> &tracks, const std::vector<fs::path> &sidecars) {
    auto checksum = g_checksum_new(G_CHECKSUM_SHA256);
    auto add = [&](const fs::path &path) {
        std::error_code error;
        auto stamp = fs::last_write_time(path, error).time_since_epoch().count();
        const auto size = fs::file_size(path, error);
        const std::string value = path.string() + '\0' + std::to_string(stamp) + ':' + std::to_string(size);
        g_checksum_update(checksum, reinterpret_cast<const guchar *>(value.data()), value.size() + 1);
    };
    for (const auto &track : tracks)
        add(track);
    for (const auto &path : sidecars)
        add(path);
    std::string key = g_checksum_get_string(checksum);
    g_checksum_free(checksum);
    return key;
}
}

AlbumTexture::~AlbumTexture() {
    if (surface)
        cairo_surface_destroy(surface);
}

struct AlbumArtCache::Entry {
    std::vector<std::string> tracks;
    fs::path directory; // published by preview_ready's release store
    std::atomic<bool> requested{false};
    std::atomic<bool> preview_ready{false};
    std::atomic<bool> detail_loading{false};
    std::atomic<bool> detail_failed{false};
    std::atomic<int> desired_pixels{0};
    std::atomic<std::shared_ptr<const AlbumTexture>> preview;
    std::atomic<std::shared_ptr<const AlbumTexture>> detail;
};

struct AlbumArtCache::Impl {
    fs::path directory;
    // UI-thread ownership keeps loaded artwork alive for this cache's lifetime,
    // even after cards or the full-size preview drop their handles.
    std::map<std::vector<std::string>, Handle> entries;
    std::atomic<bool> stopping{false};
    std::atomic<bool> changed{false};
    std::atomic<unsigned> jobs{0};
    // Cache hits have their own lane, so extraction cannot starve previews.
    // Destruction joins preview workers before destroying the detail pool.
    ThreadPool detail_pool{std::clamp(std::thread::hardware_concurrency(), 1u, 4u)};
    ThreadPool preview_pool{2};

    explicit Impl(fs::path path) : directory(std::move(path)) {}
    ~Impl() { stopping = true; }

    template<typename Work>
    void submit(ThreadPool &pool, Work work) {
        if (stopping)
            return;
        ++jobs;
        pool.enqueue([this, work = std::move(work)] {
            try {
                if (!stopping)
                    work();
            } catch (const std::exception &) {
                // A bad file or an unavailable cache must not stop the UI.
            }
            changed = true;
            --jobs;
        });
    }

    void publish_detail(const Handle &entry, GdkPixbuf *image, int pixels) {
        if (pixels == 0 || stopping)
            return;
        auto scaled = pixels < 0 ? Pixbuf(static_cast<GdkPixbuf *>(g_object_ref(image)), g_object_unref) : resized(image, pixels);
        if (scaled) {
            if (auto loaded = texture(scaled.get(), pixels)) {
                entry->detail.store(std::move(loaded));
                changed = true;
            }
        }
    }

    // Prefer embedded artwork; preserve its exact bytes as well as a lossless,
    // full-resolution decoded PNG. Folder artwork is the fallback.
    Pixbuf source(const Handle &entry, const std::vector<fs::path> &sidecars) {
        auto image = load_image(entry->directory / "full.png");
        if (image)
            return image;
        image = load_image(entry->directory / "original.img");
        if (image)
            return image;
        for (const auto &track : entry->tracks) {
            if (stopping)
                return {nullptr, g_object_unref};
            const auto art = read_album_art(track);
            if (art.bytes.empty())
                continue;
            image = decode(art.bytes);
            if (image) {
                save_bytes(art.bytes, entry->directory / "original.img");
                return image;
            }
        }
        for (const auto &path : sidecars) {
            image = load_image(path);
            if (image)
                return image;
        }
        return {nullptr, g_object_unref};
    }

    void build(const Handle &entry, const std::vector<fs::path> &sidecars) {
        auto image = source(entry, sidecars);
        if (image) {
            auto preview = resized(image.get(), preview_pixels);
            if (preview) {
                entry->preview.store(texture(preview.get(), preview_pixels));
                changed = true;
            }
            publish_detail(entry, image.get(), entry->desired_pixels.load());
            // Publish before compression / disk writes so the first view is ready sooner.
            if (!stopping) {
                if (preview)
                    save_png(preview.get(), entry->directory / "preview.png");
                save_png(image.get(), entry->directory / "full.png");
            }
        } else if (!stopping) {
            save_bytes({}, entry->directory / "missing");
        }
        entry->preview_ready = true;
        detail(entry);
    }

    void detail(const Handle &entry) {
        const int pixels = entry->desired_pixels;
        auto current = entry->detail.load();
        if (stopping || entry->detail_failed || !entry->preview_ready || !entry->preview.load() || pixels == 0 ||
            (current && (current->pixels == -1 || (pixels > 0 && current->pixels >= pixels))) || entry->detail_loading.exchange(true))
            return;
        submit(detail_pool, [this, entry] {
            const int pixels = entry->desired_pixels;
            if (pixels != 0) {
                auto image = load_image(entry->directory / "full.png", pixels);
                if (!image) {
                    image = source(entry, covers(entry->tracks));
                    if (image && !stopping)
                        save_png(image.get(), entry->directory / "full.png");
                }
                if (image)
                    publish_detail(entry, image.get(), pixels);
                if (!image)
                    entry->detail_failed = true;
            }
            entry->detail_loading = false;
            if (entry->desired_pixels != pixels)
                detail(entry);
        });
    }

    void preview(const Handle &entry) {
        submit(preview_pool, [this, entry] {
            const auto sidecars = covers(entry->tracks);
            entry->directory = directory / fingerprint(entry->tracks, sidecars);
            auto image = load_image(entry->directory / "preview.png", preview_pixels);
            if (image) {
                entry->preview.store(texture(image.get(), preview_pixels));
                entry->preview_ready = true;
                changed = true;
                detail(entry);
            } else {
                std::error_code error;
                if (fs::exists(entry->directory / "missing", error)) {
                    entry->preview_ready = true;
                    return;
                }
                submit(detail_pool, [this, entry, sidecars] { build(entry, sidecars); });
            }
        });
    }
};

AlbumArtCache::AlbumArtCache(fs::path directory)
    : impl_(std::make_unique<Impl>(directory.empty()
          ? tunes_cache_directory() / "art-v1" : std::move(directory))) {}
AlbumArtCache::~AlbumArtCache() = default;

AlbumArtCache::Handle AlbumArtCache::create(std::vector<std::string> tracks) {
    if (auto found = impl_->entries.find(tracks); found != impl_->entries.end())
        return found->second;
    auto entry = std::make_shared<Entry>();
    entry->tracks = std::move(tracks);
    impl_->entries.emplace(entry->tracks, entry);
    return entry;
}

AlbumArtCache::Handle AlbumArtCache::create_preview(const Handle &source) {
    auto entry = clone(source);
    request(entry, -1);
    return entry;
}

AlbumArtCache::Handle AlbumArtCache::clone(const Handle &source) {
    return source;
}

void AlbumArtCache::request(const Handle &entry, int pixels) {
    const int current = entry->desired_pixels;
    if (current != -1 && (pixels == -1 || pixels > current))
        entry->desired_pixels = pixels;
    if (!entry->requested.exchange(true))
        impl_->preview(entry);
    else
        impl_->detail(entry);
}

void AlbumArtCache::release(const Handle &) {
    // Requests and both image sizes stay resident until the cache is destroyed.
    // In-flight work also finishes so revisiting an album never restarts it.
}

std::shared_ptr<const AlbumTexture> AlbumArtCache::image(const Handle &entry) const {
    auto image = entry->detail.load();
    return image ? image : entry->preview.load();
}

bool AlbumArtCache::take_changed() { return impl_->changed.exchange(false); }
bool AlbumArtCache::pending() const { return impl_->jobs.load() != 0; }
