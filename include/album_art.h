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
    // Shares resident artwork with the grid and requests original resolution.
    Handle create_preview(const Handle &source);
    // UI-thread calls: enqueue only, with no filesystem access or decoding.
    // pixels == 0 preloads just the preview; -1 loads the original resolution.
    void request(const Handle &entry, int pixels);
    // Retains loaded textures and pending requests for reuse until cache destruction.
    void release(const Handle &entry);
    std::shared_ptr<const AlbumTexture> image(const Handle &entry) const;
    std::shared_ptr<const AlbumTexture> preview(const Handle &entry) const;
    // Includes missing/failed artwork so startup can always finish.
    bool preview_ready(const Handle &entry) const;
    // True once display-sized detail is available, or artwork cannot be loaded.
    bool detail_ready(const Handle &entry, int pixels) const;
    bool take_changed();
    bool pending() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
