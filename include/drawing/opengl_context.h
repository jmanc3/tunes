#pragma once
#include "drawing/context.h"
#include <cstddef>

namespace drawing {
struct OpenGLStats {
    uint64_t draw_calls = 0, image_uploads = 0, text_uploads = 0, target_allocations = 0;
    size_t image_bytes = 0, text_bytes = 0;
};
// Requires a current OpenGL ES 3 context throughout use AND destruction.
// Owns its GL objects, but not the EGL context or its presentation surface.
class OpenGLContext : public Context {
  public:
    virtual void resize(int width, int height) = 0;
    // Call after making the EGL context current, before each complete repaint.
    virtual void begin_frame() = 0;
    virtual OpenGLStats stats() const = 0;
};
// flush() resolves the antialiased frame to framebuffer 0. Presentation is the
// caller's responsibility. No CPU readback is performed by the renderer.
std::unique_ptr<OpenGLContext> create_opengl_context(int width, int height);
} // namespace drawing
