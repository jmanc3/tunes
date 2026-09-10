#include "client/egl_window.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <map>
#include <stdexcept>
#include <string>
#include <wayland-egl.h>

namespace {
void require_egl(bool ok, const char *operation) {
    if (!ok)
        throw std::runtime_error(std::string(operation) + " failed (EGL error " + std::to_string(eglGetError()) + ")");
}
struct Display {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config{};
    explicit Display(wl_display *native) {
        display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, native, nullptr);
        require_egl(display != EGL_NO_DISPLAY, "eglGetPlatformDisplay");
        require_egl(eglInitialize(display, nullptr, nullptr), "eglInitialize");
        try {
            require_egl(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
            const EGLint attributes[] = {EGL_SURFACE_TYPE,
                                         EGL_WINDOW_BIT,
                                         EGL_RENDERABLE_TYPE,
                                         EGL_OPENGL_ES3_BIT,
                                         EGL_RED_SIZE,
                                         8,
                                         EGL_GREEN_SIZE,
                                         8,
                                         EGL_BLUE_SIZE,
                                         8,
                                         EGL_ALPHA_SIZE,
                                         8,
                                         EGL_SAMPLE_BUFFERS,
                                         0,
                                         EGL_SAMPLES,
                                         0,
                                         EGL_NONE};
            EGLint count = 0;
            require_egl(eglChooseConfig(display, attributes, &config, 1, &count) && count, "eglChooseConfig");
        } catch (...) {
            eglTerminate(display);
            display = EGL_NO_DISPLAY;
            throw;
        }
    }
    ~Display() {
        if (display != EGL_NO_DISPLAY)
            eglTerminate(display);
    }
};
std::shared_ptr<Display> display_for(wl_display *native) {
    // Windowing operations, including destruction, run on the UI thread.
    static std::map<wl_display *, std::weak_ptr<Display>> displays;
    auto &weak = displays[native];
    auto display = weak.lock();
    if (!display) {
        display = std::make_shared<Display>(native);
        weak = display;
    }
    return display;
}
} // namespace
struct EGLWindow::Impl {
    std::shared_ptr<Display> display;
    wl_egl_window *window = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::unique_ptr<drawing::OpenGLContext> drawing;
    void current() { require_egl(eglMakeCurrent(display->display, surface, surface, context), "eglMakeCurrent"); }
    ~Impl() {
        if (!display)
            return;
        if (context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE)
            eglMakeCurrent(display->display, surface, surface, context);
        drawing.reset();
        eglMakeCurrent(display->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
            eglDestroySurface(display->display, surface);
        if (context != EGL_NO_CONTEXT)
            eglDestroyContext(display->display, context);
        if (window)
            wl_egl_window_destroy(window);
    }
};
EGLWindow::EGLWindow(wl_display *display, wl_surface *surface, int width, int height)
    : impl_(std::make_unique<Impl>()) {
    auto &i = *impl_;
    i.display = display_for(display);
    i.window = wl_egl_window_create(surface, width, height);
    if (!i.window)
        throw std::runtime_error("wl_egl_window_create failed");
    i.surface = eglCreatePlatformWindowSurface(i.display->display, i.display->config, i.window, nullptr);
    require_egl(i.surface != EGL_NO_SURFACE, "eglCreatePlatformWindowSurface");
    const EGLint attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    i.context = eglCreateContext(i.display->display, i.display->config, EGL_NO_CONTEXT, attributes);
    require_egl(i.context != EGL_NO_CONTEXT, "eglCreateContext");
    i.current();
    // UI invalidation/frame callbacks drive repainting; avoid blocking the event
    // loop in swap on an obscured window or while a popup waits for configure.
    require_egl(eglSwapInterval(i.display->display, 0), "eglSwapInterval");
    i.drawing = drawing::create_opengl_context(width, height);
}
EGLWindow::~EGLWindow() = default;
void EGLWindow::resize(int width, int height) {
    // Resize before making this surface current: acquiring its back buffer first
    // can lock the old dimensions until the next swap.
    wl_egl_window_resize(impl_->window, width, height, 0, 0);
    impl_->current();
    impl_->drawing->resize(width, height);
}
void EGLWindow::begin_frame() {
    impl_->current();
    impl_->drawing->begin_frame();
}
void EGLWindow::present() {
    impl_->drawing->flush();
    // EGL attaches, damages, and commits the wl_surface itself.
    require_egl(eglSwapBuffers(impl_->display->display, impl_->surface), "eglSwapBuffers");
}
drawing::Context *EGLWindow::context() { return impl_->drawing.get(); }
