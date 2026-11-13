// Offscreen check that projectM renders through the OpenGL drawing context and
// that the renderer's GL state is restored afterwards.
#include "drawing/opengl_context.h"
#include "visualizer.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <vector>

constexpr int width = 320, height = 160;

static bool make_context() {
    auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint config_attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                        EGL_NONE};
    EGLConfig config;
    EGLint count;
    if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count) return false;
    const EGLint surface_attributes[] = {EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE};
    auto surface = eglCreatePbufferSurface(display, config, surface_attributes);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    return context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE && eglMakeCurrent(display, surface, surface, context);
}

static std::vector<unsigned char> pixels() {
    std::vector<unsigned char> rgba(width * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return rgba; // Bottom-up rows.
}

static const unsigned char *at(const std::vector<unsigned char> &p, int x, int y) {
    return p.data() + ((height - 1 - y) * width + x) * 4;
}

static int fail(const char *message) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main() {
    if (!make_context()) {
        std::fprintf(stderr, "SKIP: no surfaceless EGL/GLES3\n");
        return 77;
    }
    auto gl = drawing::create_opengl_context(width, height);
    Visualizer visualizer;
    visualizer.open(nullptr);
    std::vector<unsigned char> frame;
    for (int i = 0; i < 90; ++i) {
        gl->begin_frame();
        // Drawing queued before the visualizer must land beneath nothing it does.
        gl->rectangle(width / 2, 0, width / 4, height);
        gl->set_color(RGBA(0, 1, 0, 1));
        gl->fill();
        if (!visualizer.draw(gl.get(), 0, 0, width / 2, height))
            return fail(visualizer.error().c_str());
        // Drawing after it exercises the restored GL state.
        gl->rectangle(width * 3 / 4, 0, width / 4, height);
        gl->set_color(RGBA(1, 0, 0, 1));
        gl->fill();
        gl->flush();
        frame = pixels();
    }
    std::printf("preset: '%s' (%zu available)\n", visualizer.preset_name().c_str(), visualizer.preset_count());
    long lit = 0;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width / 2; ++x) {
            auto p = at(frame, x, y);
            lit += p[0] + p[1] + p[2] > 30;
        }
    std::printf("visualizer pixels lit: %ld / %d\n", lit, width / 2 * height);
    if (lit < width / 2 * height / 50) return fail("visualizer area is blank");
    auto green = at(frame, width * 5 / 8, height / 2), red = at(frame, width * 7 / 8, height / 2);
    std::printf("before: %d %d %d %d  after: %d %d %d %d\n", green[0], green[1], green[2], green[3], red[0], red[1],
                red[2], red[3]);
    if (!(green[1] > 250 && green[0] < 5 && green[2] < 5)) return fail("drawing before the visualizer was disturbed");
    if (!(red[0] > 250 && red[1] < 5 && red[2] < 5)) return fail("drawing after the visualizer is wrong");

    visualizer.close();
    gl->begin_frame();
    gl->rectangle(0, 0, width, height);
    gl->set_color(RGBA(0, 0, 1, 1));
    gl->fill();
    visualizer.collect(gl.get());
    gl->flush();
    frame = pixels();
    auto blue = at(frame, width / 4, height / 2);
    if (!(blue[2] > 250 && blue[0] < 5 && blue[1] < 5)) return fail("frame after release is wrong");
    if (glGetError() != GL_NO_ERROR) return fail("GL error");
    std::puts("PASS");
    return 0;
}
