#include "drawing/context.h"
#include <cairo.h>
#include <pango/pangocairo.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace drawing {
namespace {
class CairoContext final : public Context {
    cairo_t *cr_;
    std::map<std::tuple<std::string, double, bool>, PangoLayout *> fonts_;
public:
    explicit CairoContext(cairo_t *cr) : cr_(cr) {}
    ~CairoContext() override {
        for (auto &[key, layout] : fonts_) g_object_unref(layout);
        cairo_destroy(cr_);
    }
    void save() override { cairo_save(cr_); }
    void restore() override { cairo_restore(cr_); }
    void translate(double x, double y) override { cairo_translate(cr_, x, y); }
    void scale(double x, double y) override { cairo_scale(cr_, x, y); }
    void new_sub_path() override { cairo_new_sub_path(cr_); }
    void move_to(double x, double y) override { cairo_move_to(cr_, x, y); }
    void line_to(double x, double y) override { cairo_line_to(cr_, x, y); }
    void arc(double x, double y, double radius, double start, double end) override {
        cairo_arc(cr_, x, y, radius, start, end);
    }
    void close_path() override { cairo_close_path(cr_); }
    void rectangle(double x, double y, double width, double height) override {
        cairo_rectangle(cr_, x, y, width, height);
    }
    void fill() override { cairo_fill(cr_); }
    void fill_preserve() override { cairo_fill_preserve(cr_); }
    void clip() override { cairo_clip(cr_); }
    void stroke() override { cairo_stroke(cr_); }
    void set_color(RGBA c) override { cairo_set_source_rgba(cr_, c.r, c.g, c.b, c.a); }
    void set_line_width(double width) override { cairo_set_line_width(cr_, width); }
    void set_line_cap(LineCap cap) override {
        switch (cap) {
        case LineCap::Butt: cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT); break;
        case LineCap::Round: cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND); break;
        case LineCap::Square: cairo_set_line_cap(cr_, CAIRO_LINE_CAP_SQUARE); break;
        }
    }
    void set_fill_rule(FillRule rule) override {
        cairo_set_fill_rule(cr_, rule == FillRule::EvenOdd ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
    }
    void set_operator(Composite op) override {
        switch (op) {
        case Composite::Over: cairo_set_operator(cr_, CAIRO_OPERATOR_OVER); break;
        case Composite::Add: cairo_set_operator(cr_, CAIRO_OPERATOR_ADD); break;
        case Composite::Source: cairo_set_operator(cr_, CAIRO_OPERATOR_SOURCE); break;
        case Composite::Clear: cairo_set_operator(cr_, CAIRO_OPERATOR_CLEAR); break;
        }
    }
    void push_group() override { cairo_push_group(cr_); }
    void pop_group_to_source() override { cairo_pop_group_to_source(cr_); }
    void paint_source(double alpha) override { cairo_paint_with_alpha(cr_, alpha); }
    void flush() override { cairo_surface_flush(cairo_get_target(cr_)); }
    void draw_image(const Image &image, double alpha, ImageFilter filter) override {
        if (image.width <= 0 || image.height <= 0 ||
            image.argb.size() < static_cast<size_t>(image.width) * image.height) return;
        // No pixel copy; the temporary source is released before returning.
        auto surface = cairo_image_surface_create_for_data(
            reinterpret_cast<unsigned char *>(const_cast<uint32_t *>(image.argb.data())),
            CAIRO_FORMAT_ARGB32, image.width, image.height, image.width * 4);
        save();
        cairo_set_source_surface(cr_, surface, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr_), filter == ImageFilter::Bilinear ? CAIRO_FILTER_BILINEAR : CAIRO_FILTER_GOOD);
        cairo_pattern_set_extend(cairo_get_source(cr_), CAIRO_EXTEND_PAD);
        rectangle(0, 0, image.width, image.height);
        clip();
        paint_source(alpha);
        restore();
        cairo_surface_destroy(surface);
    }
    TextMetrics text(double x, double y, const std::string &value, const TextStyle &style, bool draw) override {
        auto key = std::make_tuple(style.font, style.size, style.bold);
        auto &layout = fonts_[key];
        if (!layout) {
            layout = pango_cairo_create_layout(cr_);
            auto desc = pango_font_description_new();
            pango_font_description_set_family(desc, style.font.c_str());
            pango_font_description_set_size(desc, style.size * PANGO_SCALE);
            pango_font_description_set_weight(desc, style.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
            pango_layout_set_font_description(layout, desc);
            pango_font_description_free(desc);
        }
        pango_cairo_update_layout(cr_, layout);
        pango_layout_set_text(layout, value.data(), value.size());
        PangoAlignment alignment = PANGO_ALIGN_LEFT;
        if (style.align == TextAlign::Center) alignment = PANGO_ALIGN_CENTER;
        if (style.align == TextAlign::Right) alignment = PANGO_ALIGN_RIGHT;
        pango_layout_set_alignment(layout, alignment);
        pango_layout_set_width(layout, style.width < 0 ? -1 : style.width * PANGO_SCALE);
        pango_layout_set_height(layout, style.width < 0 || style.height < 0 ? -1 : style.height * PANGO_SCALE);
        pango_layout_set_wrap(layout, style.width < 0 ? PANGO_WRAP_NONE : PANGO_WRAP_WORD_CHAR);
        pango_layout_set_ellipsize(layout, style.width >= 0 && style.height >= 0 ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_NONE);
        PangoRectangle ink, logical;
        pango_layout_get_pixel_extents(layout, &ink, &logical);
        set_color(style.color);
        if (draw) {
            if (style.shadow.opacity > 0) {
                save();
                const auto &shadow = style.shadow;
                const double opacity = std::clamp(shadow.opacity, 0.0, .999);
                for (int row = -2; row <= 2; ++row)
                    for (int col = -2; col <= 2; ++col) {
                        const double weight = (3 - std::abs(row)) * (3 - std::abs(col)) / 81.0;
                        set_color(RGBA(0, 0, 0, 1 - std::pow(1 - opacity, weight)));
                        move_to(x + col * std::max(0.0, shadow.blur) * style.dpi / 2,
                                y + (shadow.offset_y + row * std::max(0.0, shadow.blur) / 2) * style.dpi);
                        pango_cairo_show_layout(cr_, layout);
                    }
                restore();
            }
            move_to(std::round(x), std::round(y));
            pango_cairo_show_layout(cr_, layout);
        }
        return {double(ink.width), double(ink.height), double(logical.width), double(logical.height)};
    }
};
}
std::unique_ptr<Context> create_cairo_context(unsigned char *data, int width, int height, int stride) {
    if (!data || width <= 0 || height <= 0 || stride < cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width))
        throw std::invalid_argument("Invalid drawing buffer");
    auto surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, width, height, stride);
    auto cr = cairo_create(surface);
    cairo_surface_destroy(surface);
    const auto status = cairo_status(cr);
    if (status != CAIRO_STATUS_SUCCESS) {
        cairo_destroy(cr);
        throw std::runtime_error(cairo_status_to_string(status));
    }
    try { return std::make_unique<CairoContext>(cr); }
    catch (...) { cairo_destroy(cr); throw; }
}
}
