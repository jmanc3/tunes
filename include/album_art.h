#pragma once

#include <cairo.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Immutable Cairo image surfaces are built entirely on workers and shared with
// the renderer only after publication.
struct AlbumTexture {
    cairo_surface_t *surface = nullptr;
    int width = 0;
    int height = 0;
    int pixels = 0;
    ~AlbumTexture();
};

class AlbumArtCache {
public:
    struct Entry;
    using Handle = std::shared_ptr<Entry>;

    explicit AlbumArtCache(std::filesystem::path directory = {});
    ~AlbumArtCache();
    Handle create(std::vector<std::string> tracks);
    Handle clone(const Handle &source);
    // Independent request sharing the same disk cache, unaffected by grid eviction.
    Handle create_preview(const Handle &source);
    // UI-thread calls: enqueue only, with no filesystem access or decoding.
    // pixels == 0 preloads just the preview; -1 loads the original resolution.
    void request(const Handle &entry, int pixels);
    void release(const Handle &entry);
    std::shared_ptr<const AlbumTexture> image(const Handle &entry) const;
    bool take_changed();
    bool pending() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
