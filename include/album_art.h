#pragma once

#include "drawing/context.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Immutable pixel images are built on workers and shared after publication.
struct AlbumTexture : drawing::Image {
    int pixels = 0;
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

// UI-thread prefetch cursor. Only one background album's loading chain is
// submitted at a time, after foreground work has drained. Visibility/fades stay
// with the UI; this prepares immutable sharp images without starting animations.
class AlbumArtPrefetch {
public:
    void add(AlbumArtCache::Handle album);
    void reset();
    bool advance(AlbumArtCache &cache, int pixels, bool foreground_ready);

private:
    std::vector<AlbumArtCache::Handle> albums_;
    std::size_t next_ = 0;
    int pixels_ = 0;
};
