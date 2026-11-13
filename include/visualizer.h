#pragma once

#include <memory>
#include <string>

namespace drawing { class Context; }
class Player;

// projectM (MilkDrop-compatible) music visualizer rendered into a texture and
// composited by the drawing context. All methods run on the UI thread. GL
// resources are created lazily inside draw() and released by collect(), both
// of which must be called while the window's GL context is current (i.e.
// during painting).
class Visualizer {
public:
    Visualizer();
    ~Visualizer();
    Visualizer(const Visualizer &) = delete;
    Visualizer &operator=(const Visualizer &) = delete;

    // Starts feeding audio. Rendering starts on the next draw().
    void open(Player *player);
    // Stops the audio feed; GL resources are released by the next collect().
    void close();
    bool is_open() const noexcept;

    // Draws one frame filling (x, y, width, height). Returns false when the
    // visualizer cannot render (no OpenGL, or projectM failed to start).
    bool draw(drawing::Context *cr, double x, double y, double width, double height);
    // Frees projectM and its textures once closed. Call every frame.
    void collect(drawing::Context *cr);

    void next_preset();
    void previous_preset();
    void random_preset();

    // Display name of the current preset ("" for the built-in idle preset).
    std::string preset_name() const;
    std::size_t preset_count() const;
    // Why draw() returned false.
    std::string error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
