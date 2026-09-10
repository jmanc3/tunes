#include "drawing/opengl_context.h"
#include "drawing/cached_shadow.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>

using namespace drawing;
static void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
struct EGLFixture {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool initialize() {
        display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr))
            return false;
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                            EGL_PBUFFER_BIT,
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
                                            EGL_NONE};
        EGLConfig config;
        EGLint count;
        if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || !count)
            return false;
        const EGLint surface_attributes[] = {EGL_WIDTH, 256, EGL_HEIGHT, 192, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, surface_attributes);
        const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        return context != EGL_NO_CONTEXT && surface != EGL_NO_SURFACE &&
               eglMakeCurrent(display, surface, surface, context);
    }
    ~EGLFixture() {
        if (display != EGL_NO_DISPLAY) {
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (context != EGL_NO_CONTEXT)
                eglDestroyContext(display, context);
            if (surface != EGL_NO_SURFACE)
                eglDestroySurface(display, surface);
            eglTerminate(display);
        }
    }
};
constexpr int width = 256, height = 192;
static std::vector<uint32_t> read_pixels(int w = width, int h = height) {
    std::vector<unsigned char> rgba(w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    std::vector<uint32_t> result(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto p = rgba.data() + ((h - 1 - y) * w + x) * 4;
            result[y * w + x] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
    return result;
}
static void compare(OpenGLContext &gl, const char *name, const std::function<void(Context &)> &scene,
                    double tolerance = 2.5) {
    std::vector<uint32_t> expected(width * height);
    auto cairo = create_cairo_context(reinterpret_cast<unsigned char *>(expected.data()), width, height, width * 4);
    scene(*cairo);
    cairo->flush();
    gl.begin_frame();
    scene(gl);
    gl.flush();
    auto actual = read_pixels();
    check(glGetError() == GL_NO_ERROR, std::string(name) + ": GL error");
    double error = 0;
    int differing = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        int largest = 0;
        for (int shift : {0, 8, 16, 24}) {
            int delta = std::abs(int((actual[i] >> shift) & 255) - int((expected[i] >> shift) & 255));
            largest = std::max(largest, delta);
            error += delta;
        }
        if (largest > 40)
            ++differing;
    }
    error /= actual.size() * 4;
    std::cout << name << ": mean channel error " << error << ", pixels >40 " << differing << '\n';
    check(error < tolerance, std::string(name) + ": Cairo/OpenGL pixel mismatch");
    check(differing < int(actual.size() / 30), std::string(name) + ": too many substantially different pixels");
}
int main() try {
    EGLFixture egl;
    if (!egl.initialize()) {
        std::cerr << "No surfaceless EGL ES3 driver available\n";
        return 77;
    }
    std::cout << "GL renderer: " << glGetString(GL_RENDERER) << '\n';
    auto gl = create_opengl_context(width, height);
    for (double amount : {1.0, 0.5, 0.0}) {
        compare(*gl, "fullscreen Gaussian blur", [=](Context &c) {
            c.set_color(RGBA(0, 0, 0, 1));
            c.paint_source();
            c.set_color(RGBA(1, 1, 1, 1));
            c.rectangle(width / 2, 0, width / 2, height);
            c.fill();
            c.gaussian_blur(6, amount);
        }, 0.6);
        auto pixels = read_pixels();
        check(pixels.front() == 0xff000000 && pixels.back() == 0xffffffff,
              "Blur must preserve opaque frame edges");
        int near_edge = pixels[(height / 2) * width + width / 2 - 1] & 255;
        check(amount > 0 ? (near_edge > 20 && near_edge < 128) : near_edge == 0,
              "Blur must soften an edge and fade to the sharp original");
    }
    compare(*gl, "scrolled queue below text header", [](Context &c) {
        c.save();
        c.rounded_rectangle({20, 10, 210, 160}, 12); c.clip();
        TextStyle style;
        style.font = "Sans"; style.color = RGBA(1, 1, 1, 1);
        c.text(36, 30, "Queue", style, true);
        c.rectangle(20, 70, 210, 100); c.clip();
        c.save();
        c.rectangle(28, 40, 194, 64); c.clip();
        c.push_group();
        c.set_color(RGBA(1, 0, 0, 1));
        c.rectangle(28, 40, 194, 64); c.fill();
        c.pop_group_to_source(); c.paint_source();
        c.restore();
        c.restore();
    }, .5);
    compare(*gl, "paths, holes and clips", [](Context &c) {
        c.set_color(RGBA(.2, .4, .8, 1));
        c.rounded_rectangle({8, 8, 110, 80}, 12);
        c.fill();
        c.save();
        c.rounded_rectangle({20, 20, 90, 60}, 10);
        c.clip();
        c.translate(15, 10);
        c.scale(1.2, .8);
        c.set_color(RGBA(1, .2, .1, .6));
        c.rectangle(0, 0, 65, 100);
        c.fill();
        c.restore();
        c.set_fill_rule(FillRule::EvenOdd);
        c.set_color(RGBA(.7, .9, .2, .8));
        c.rectangle(140, 10, 85, 80);
        c.rectangle(160, 30, 40, 40);
        c.fill();
        c.set_fill_rule(FillRule::Winding);
        c.move_to(12, 112);
        c.line_to(90, 140);
        c.line_to(30, 160);
        c.line_to(40, 110);
        c.close_path();
        c.fill();
    });
    compare(*gl, "strokes and reflected transforms", [](Context &c) {
        c.set_color(RGBA(.4, .8, .3, .6));
        c.set_line_width(9);
        c.set_line_cap(LineCap::Round);
        c.move_to(20, 25);
        c.line_to(95, 25);
        c.line_to(70, 70);
        c.stroke();
        c.save();
        c.translate(220, 100);
        c.scale(-1.5, .8);
        c.set_line_width(4);
        c.arc(0, 0, 36, -2.6, 2.6);
        c.stroke();
        c.restore();
        c.set_line_cap(LineCap::Square);
        c.move_to(20, 130);
        c.line_to(100, 165);
        c.stroke();
    });
    Image red{2, 2, {0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000}};
    Image blue{2, 2, {0xff0000ff, 0xff0000ff, 0xff0000ff, 0xff0000ff}};
    auto groups = [&](Context &c) {
        c.save();
        c.rectangle(10, 10, 160, 140);
        c.clip();
        c.push_group();
        c.save();
        c.translate(10, 10);
        c.scale(50, 50);
        c.draw_image(red, .5);
        c.restore();
        c.set_operator(Composite::Add);
        c.save();
        c.translate(10, 10);
        c.scale(50, 50);
        c.draw_image(blue, .5);
        c.restore();
        c.pop_group_to_source();
        c.paint_source();
        c.push_group();
        c.push_group();
        c.set_color(RGBA(0, 1, 0, .75));
        c.rectangle(40, 40, 100, 100);
        c.fill();
        c.pop_group_to_source();
        c.paint_source(.7);
        c.pop_group_to_source();
        c.paint_source(.6);
        c.restore();
    };
    compare(*gl, "nested additive groups", groups, .5);
    compare(
        *gl, "bounded cover groups",
        [&](Context &c) {
            c.set_color(RGBA(.1, .2, .3, 1));
            c.paint_source();
            c.push_group({10, 10, 40, 40});
            c.set_color(RGBA(1, 0, 0, 1));
            c.paint_source();
            c.pop_group_to_source();
            c.paint_source(.5);
            c.push_group({80, 20, 30, 30});
            c.push_group({85, 25, 20, 20});
            c.set_color(RGBA(0, 1, 0, .5));
            c.paint_source();
            c.pop_group_to_source();
            c.paint_source();
            c.pop_group_to_source();
            c.paint_source();
        },
        .5);
    compare(*gl, "self-intersecting winding", [](Context &c) {
        c.set_color(RGBA(.8, .4, .1, .7));
        for (int i = 0; i < 5; ++i) {
            double a = 6.283185307179586 * (i * 2 % 5) / 5;
            if (i == 0)
                c.move_to(70 + 50 * std::cos(a), 70 + 50 * std::sin(a));
            else
                c.line_to(70 + 50 * std::cos(a), 70 + 50 * std::sin(a));
        }
        c.close_path();
        c.fill();
    });
    auto uploads = gl->stats().image_uploads;
    compare(*gl, "cached images", groups, .5);
    check(gl->stats().image_uploads == uploads, "Artwork was uploaded again");
    compare(
        *gl, "source and clear operators",
        [](Context &c) {
            c.set_color(RGBA(.2, .4, .7, 1));
            c.paint_source();
            c.set_operator(Composite::Source);
            c.set_color(RGBA(1, 0, 0, .5));
            c.rectangle(20, 20, 80, 80);
            c.fill();
            c.set_color(RGBA(0, 1, 0, .5));
            c.rectangle(50, 50, 80, 80);
            c.fill();
            c.save();
            c.rectangle(70, 70, 80, 80);
            c.clip();
            c.set_operator(Composite::Clear);
            c.paint_source(.5);
            c.restore();
        },
        .5);
    auto text = [](Context &c) {
        TextStyle style;
        style.font = "Sans";
        style.size = 15;
        style.color = RGBA(.2, .8, .9, .8);
        style.width = 180;
        style.height = 40;
        style.align = TextAlign::Center;
        c.text(10, 10, "Music — café ♫", style, true);
        style.shadow = {.4, 2, 1};
        c.text(10, 70, "Text with shadow", style, true);
        style.bold = true;
        style.width = 70;
        style.height = 25;
        c.text(10, 130, "Ellipsized long title", style, true);
    };
    compare(*gl, "text and text shadows", text, 1);
    auto text_uploads = gl->stats().text_uploads;
    compare(*gl, "cached text", text, 1);
    check(gl->stats().text_uploads == text_uploads, "Text was rasterized/uploaded again");
    compare(*gl, "rounded shadows", [](Context &c) {
        c.shadow({30, 30, 100, 80}, 12, {.4, 8, 3}, 1);
        c.set_color(RGBA(1, 1, 1, 1));
        c.rounded_rectangle({30, 30, 100, 80}, 12);
        c.fill();
    });
    CachedShadow artwork_shadow;
    ShadowStyle artwork_style{.18, 22, 0};
    auto shadow_scene = [&](Context &c) {
        for (double x : {30., 120.}) {
            Rect bounds{x, 40, 64, 64};
            if (&c == gl.get()) artwork_shadow.draw(c, bounds, 0, artwork_style, 1);
            else { CachedShadow reference; reference.draw(c, bounds, 0, artwork_style, 1); }
        }
    };
    auto shadow_uploads = gl->stats().image_uploads;
    auto shadow_draws = gl->stats().draw_calls;
    compare(*gl, "Cairo-rasterized artwork shadows", shadow_scene, .1);
    check(gl->stats().image_uploads == shadow_uploads + 1, "Identical artwork shadows uploaded separate textures");
    check(gl->stats().draw_calls == shadow_draws + 1, "Artwork shadows were rebuilt from individual GPU shapes");
    compare(*gl, "shared shadow next frame", shadow_scene, .1);
    check(gl->stats().image_uploads == shadow_uploads + 1, "Artwork shadow texture was uploaded again");
    for (double opacity : {.6, 0.0, 1.0, .18}) {
        artwork_style.opacity = opacity;
        compare(*gl, "shadow opacity reuses raster", shadow_scene, .1);
        check(gl->stats().image_uploads == shadow_uploads + 1,
              "Changing shadow opacity must not rebuild or reupload the texture");
    }
    artwork_style = {.3, 12, -9};
    compare(*gl, "shadow style invalidation", shadow_scene, .1);
    check(gl->stats().image_uploads == shadow_uploads + 2, "Changed shadow style reused stale pixels");
    compare(*gl, "shadow DPI and size invalidation", [&](Context &c) {
        Rect bounds{40, 45, 100, 72};
        if (&c == gl.get()) artwork_shadow.draw(c, bounds, 3, artwork_style, 1.5);
        else { CachedShadow reference; reference.draw(c, bounds, 3, artwork_style, 1.5); }
    }, .1);
    check(gl->stats().image_uploads == shadow_uploads + 3, "Changed shadow geometry reused stale pixels");

    CachedShadow fading_shadow;
    const auto fade_uploads = gl->stats().image_uploads;
    for (double alpha : {0.0, .25, .75, 1.0, 1.0}) {
        compare(*gl, "initial shadow fade", [&](Context &c) {
            fading_shadow.draw(c, {40, 45, 100, 72}, 3, artwork_style, 1, alpha);
        }, .1);
        check(gl->stats().image_uploads == fade_uploads + (alpha > 0 ? 1 : 0),
              "Shadow fade must skip zero opacity and reuse one texture throughout");
        if (alpha == 0) {
            const auto pixels = read_pixels();
            check(std::all_of(pixels.begin(), pixels.end(), [](auto p) { return p == 0; }),
                  "Initial hidden shadow must not draw pixels");
        }
    }

    gl->begin_frame();
    auto before = gl->stats().draw_calls;
    for (int i = 0; i < 100; ++i) {
        gl->set_color(RGBA(.2, .5, .8, 1));
        gl->rectangle((i % 10) * 8, (i / 10) * 8, 6, 6);
        gl->fill();
    }
    gl->flush();
    check(gl->stats().draw_calls - before <= 2, "Ordinary rectangles were not batched");
    // Reusing an Image address must not resurrect a stale texture even if a
    // copy keeps the old lifetime witness alive.
    gl->begin_frame();
    Image reused{1, 1, {0xffff0000}}, witness = reused;
    gl->draw_image(reused);
    gl->flush();
    reused = Image{1, 1, {0xff00ff00}};
    gl->begin_frame();
    gl->draw_image(reused);
    gl->flush();
    check(read_pixels()[0] == 0xff00ff00, "Artwork cache reused a dead object's texture");
    uploads = gl->stats().image_uploads;
    // Size changes keep immutable resource caches but replace render targets.
    gl->resize(128, 96);
    gl->begin_frame();
    gl->set_color(RGBA(0, 1, 0, 1));
    gl->paint_source();
    gl->flush();
    auto resized = read_pixels(128, 96);
    check(resized.front() == 0xff00ff00 && resized.back() == 0xff00ff00, "Resize rendered into stale bounds");
    gl->resize(127, 95);
    gl->begin_frame();
    gl->set_color(RGBA(0, 1, 0, 1));
    gl->paint_source();
    gl->gaussian_blur(6, 1);
    gl->flush();
    resized = read_pixels(127, 95);
    check(std::all_of(resized.begin(), resized.end(), [](uint32_t p) { return p == 0xff00ff00; }),
          "Blur must preserve constant colors at non-aligned window sizes");
    gl->resize(width, height);
    compare(*gl, "after resize", groups, .5);
    check(gl->stats().image_uploads == uploads, "Resize discarded artwork texture cache");
    check(glGetError() == GL_NO_ERROR, "GL error after resize");
    gl.reset();
    check(glGetError() == GL_NO_ERROR, "GL error during resource cleanup");
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
