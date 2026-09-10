#pragma once
#include "drawing/color.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace drawing {
// Native-endian 0xAARRGGBB words, with premultiplied alpha. Immutable while drawn.
struct Image {
    int width = 0, height = 0;
    std::vector<uint32_t> argb;
    // Lifetime witness for backend caches; pixel data is immutable after publication.
    std::shared_ptr<const char> cache_lifetime = std::make_shared<const char>(0);
};
struct Rect { double x, y, width, height; };
enum class Composite { Over, Add, Source, Clear };
enum class LineCap { Butt, Round, Square };
enum class FillRule { Winding, EvenOdd };
enum class ImageFilter { Bilinear, Good };
enum class TextAlign { Left, Center, Right };
struct ShadowStyle {
    double opacity = 0, blur = 0, offset_y = 0;
};
struct TextStyle {
    std::string font;
    double size = 12; // Font size in points, matching the existing UI typography.
    bool bold = false;
    TextAlign align = TextAlign::Left;
    double width = -1, height = -1; // Pixels; -1 means unconstrained.
    RGBA color;
    ShadowStyle shadow;
    double dpi = 1;
};
struct TextMetrics { double ink_width, ink_height, width, height; };

// Stateful immediate-mode drawing in device pixels. save/restore preserve drawing
// state (including clipping and transforms), but not the current path. fill,
// stroke and clip consume the path; fill_preserve retains it. Groups isolate
// drawing into a transparent layer; popping makes that layer the paint source.
class Context {
public:
    virtual ~Context() = default;
    // Reusable geometry/effects implemented using the primitives below.
    void rounded_rectangle(Rect bounds, double radius);
    void shadow(Rect bounds, double radius, const ShadowStyle &style, double dpi);
    virtual void save() = 0;
    virtual void restore() = 0;
    virtual void translate(double x, double y) = 0;
    virtual void scale(double x, double y) = 0;
    virtual void new_sub_path() = 0;
    virtual void move_to(double x, double y) = 0;
    virtual void line_to(double x, double y) = 0;
    virtual void arc(double x, double y, double radius, double start, double end) = 0;
    virtual void close_path() = 0;
    virtual void rectangle(double x, double y, double width, double height) = 0;
    virtual void fill() = 0;
    virtual void fill_preserve() = 0;
    virtual void clip() = 0;
    virtual void stroke() = 0;
    virtual void set_color(RGBA color) = 0;
    virtual void set_line_width(double width) = 0;
    virtual void set_line_cap(LineCap cap) = 0;
    virtual void set_fill_rule(FillRule rule) = 0;
    virtual void set_operator(Composite op) = 0;
    virtual void push_group() = 0;
    // Constrain an opacity group in user coordinates, allowing a small GPU target.
    virtual void push_group(Rect bounds) {
        push_group();
        rectangle(bounds.x, bounds.y, bounds.width, bounds.height);
        clip();
    }
    virtual void pop_group_to_source() = 0;
    virtual void paint_source(double alpha = 1) = 0;
    // Draw at the origin in image pixels under the current transform. Clips to
    // image bounds and preserves drawing state; consumes the current path.
    virtual void draw_image(const Image &image, double alpha = 1, ImageFilter filter = ImageFilter::Bilinear) = 0;
    virtual TextMetrics text(double x, double y, const std::string &text, const TextStyle &style, bool draw) = 0;
    virtual void flush() = 0;
};

// The caller owns the ARGB32 buffer and keeps it alive until context destruction.
std::unique_ptr<Context> create_cairo_context(unsigned char *data, int width, int height, int stride);
}
