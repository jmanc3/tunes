#include "drawing/context.h"
#include "drawing/cached_shadow.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

int main() {
    using namespace drawing;
    constexpr int width = 128, height = 64;
    std::vector<uint32_t> pixels(width * height);
    auto context = create_cairo_context(reinterpret_cast<unsigned char *>(pixels.data()), width, height, width * 4);
    auto &c = *context;
    auto clear = [&] {
        c.save(); c.set_operator(Composite::Clear); c.paint_source(); c.restore();
    };
    // Clip and transform state must be restored for subsequent widgets.
    c.save();
    c.translate(4, 4);
    c.rectangle(0, 0, 8, 8); c.clip();
    c.set_color(RGBA(1, 0, 0, 1)); c.paint_source();
    c.restore();
    c.set_color(RGBA(0, 1, 0, 1)); c.rectangle(20, 0, 4, 4); c.fill();
    c.flush();
    check(pixels[4 * width + 4] == 0xffff0000 && pixels[0] == 0 && pixels[20] == 0xff00ff00,
          "Clipping/transform restoration failed");

    // Artwork crossfades use additive premultiplied layers, not source-over.
    clear();
    Image red{1, 1, {0xffff0000}}, blue{1, 1, {0xff0000ff}};
    c.save(); c.scale(8, 8);
    c.push_group();
    c.draw_image(red, .5);
    c.set_operator(Composite::Add); c.draw_image(blue, .5);
    c.pop_group_to_source(); c.set_operator(Composite::Over); c.paint_source();
    c.restore(); c.flush();
    const auto pixel = pixels[3 * width + 3];
    check((pixel >> 24) >= 254 && ((pixel >> 16) & 255) == 128 && (pixel & 255) == 128,
          "Scaled image crossfade lost opacity or color");
    check(pixels[9 * width + 9] == 0, "Image drawing escaped its bounds");

    // Even-odd paths produce holes, and fill_preserve retains paths for clipping.
    clear();
    c.save(); c.set_fill_rule(FillRule::EvenOdd);
    c.rectangle(0, 0, 12, 12); c.rectangle(4, 4, 4, 4);
    c.set_color(RGBA(1, 1, 1, 1)); c.fill_preserve(); c.clip();
    c.set_color(RGBA(0, 1, 0, 1)); c.paint_source(); c.restore(); c.flush();
    check(pixels[0] == 0xff00ff00 && pixels[5 * width + 5] == 0, "Path fill/clip semantics changed");

    clear();
    c.set_color(RGBA(1, 1, 1, 1));
    c.rounded_rectangle({8, 8, 24, 24}, 8); c.fill(); c.flush();
    check(pixels[8 * width + 8] == 0 && pixels[20 * width + 20] == 0xffffffff,
          "Rounded rectangle geometry changed");
    clear();
    c.shadow({16, 16, 16, 16}, 2, {.5, 4, 2}, 1); c.flush();
    check(pixels[24 * width + 24] != 0 && pixels[0] == 0, "Shadow drawing failed");

    clear();
    CachedShadow shadow;
    shadow.draw(c, {32, 20, 32, 24}, 0, {1, 8, 0}, 1); c.flush();
    auto alpha = [&](int x, int y) { return pixels[y * width + x] >> 24; };
    check(alpha(48, 32) == 0 && alpha(32, 32) == 0 && alpha(63, 32) == 0,
          "Cached shadow must cut out the casting rectangle");
    check(alpha(31, 32) > alpha(28, 32) && alpha(28, 32) > alpha(24, 32) && alpha(24, 32) > 0,
          "Gaussian shadow must fall off smoothly outside the rectangle");
    check(alpha(16, 32) > 0 && alpha(31, 32) - alpha(30, 32) <= 16,
          "Diffuse shadow must have a broad tail and a gentle near-edge slope");
    check(alpha(28, 32) == alpha(67, 32) && alpha(48, 16) == alpha(48, 47),
          "Unshifted Gaussian shadow must be symmetric");
    check(std::all_of(pixels.begin(), pixels.end(), [](auto p) { return (p & 0xffffff) == 0; }),
          "Shadow pixels must be black");
    clear();
    shadow.draw(c, {32, 20, 32, 24}, 8, {1, 8, 0}, 1); c.flush();
    check(alpha(32, 20) > 0 && alpha(48, 32) == 0,
          "Rounded shadow cutout must preserve the outside corners");
    clear();
    shadow.draw(c, {32, 20, 32, 24}, 0, {1, 8, 4}, 1); c.flush();
    check(alpha(48, 45) > alpha(48, 18) && alpha(48, 43) == 0,
          "Offset shadow must remain outside the original shape");
    clear();
    shadow.draw(c, {32, 20, 32, 24}, 0, {1, 0, 0}, 1); c.flush();
    check(std::all_of(pixels.begin(), pixels.end(), [](auto p) { return p == 0; }),
          "Zero-blur unshifted shadow must disappear after the cutout");

    // Test the exterior tails separately from the intentional shape cutout.
    // Fractional bounds/DPI and offsets exercise all four texture margins.
    for (double offset : {-18.5, 0.0, 18.5}) {
        constexpr int extent = 512;
        std::vector<uint32_t> tail_pixels(extent * extent);
        auto tail_context = create_cairo_context(reinterpret_cast<unsigned char *>(tail_pixels.data()),
                                                 extent, extent, extent * 4);
        shadow.draw(*tail_context, {224.25, 224.5, 63.5, 63.25}, 9, {1, 18, offset}, 1.5);
        tail_context->flush();
        for (bool vertical : {false, true}) {
            int first = -1, last = -1;
            auto sample = [&](int i) {
                return (vertical ? tail_pixels[i * extent + 256] : tail_pixels[256 * extent + i]) >> 24;
            };
            for (int i = 0; i < extent; ++i)
                if (sample(i)) {
                    if (first < 0) first = i;
                    last = i;
                }
            check(first > 0 && last < extent - 1, "Shadow tail must fit inside the destination");
            check(sample(first) == 1 && sample(last) == 1,
                  "Shadow must fade to the lowest alpha before its exterior edge");
        }
    }

    clear();
    TextStyle style;
    style.font = "Sans"; style.size = 12; style.color = RGBA(1, 1, 1, 1);
    auto metrics = c.text(0, 0, "Hello", style, false);
    c.flush();
    check(metrics.width > 0 && metrics.height > 0, "Text measurement failed");
    check(std::all_of(pixels.begin(), pixels.end(), [](auto p) { return p == 0; }), "Measurement drew pixels");
    auto rendered = c.text(0, 0, "Hello", style, true);
    c.flush();
    check(rendered.width == metrics.width && rendered.height == metrics.height, "Text layout and rendering disagree");
    check(std::any_of(pixels.begin(), pixels.end(), [](auto p) { return p != 0; }), "Text was not rendered");
    style.width = 24; style.height = 16;
    c.text(0, 0, "A much longer string", style, false);
    style.width = style.height = -1;
    auto reset = c.text(0, 0, "Hello", style, false);
    check(reset.width == metrics.width && reset.height == metrics.height, "Cached layout retained old constraints");
    context.reset();
    context = create_cairo_context(reinterpret_cast<unsigned char *>(pixels.data()), width, height, width * 4);
    check(context->text(0, 0, "Hello", style, false).width == metrics.width, "Recreated context changed text metrics");
}
