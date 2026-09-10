#include "gl_text.h"
#include <cairo.h>
#include <cstring>
#include <memory>
#include <pango/pangocairo.h>
#include <stdexcept>

namespace drawing {
namespace {
using Surface = std::unique_ptr<cairo_surface_t, decltype(&cairo_surface_destroy)>;
using Cairo = std::unique_ptr<cairo_t, decltype(&cairo_destroy)>;
using Layout = std::unique_ptr<PangoLayout, decltype(&g_object_unref)>;
using Font = std::unique_ptr<PangoFontDescription, decltype(&pango_font_description_free)>;
} // namespace
TextRaster rasterize_gl_text(const std::string &text, const TextStyle &style, bool rasterize) {
    Surface surface(cairo_image_surface_create(CAIRO_FORMAT_A8, 1, 1), cairo_surface_destroy);
    Cairo cr(cairo_create(surface.get()), cairo_destroy);
    Layout layout(pango_cairo_create_layout(cr.get()), g_object_unref);
    Font desc(pango_font_description_new(), pango_font_description_free);
    pango_font_description_set_family(desc.get(), style.font.c_str());
    pango_font_description_set_size(desc.get(), style.size * PANGO_SCALE);
    pango_font_description_set_weight(desc.get(), style.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout.get(), desc.get());
    pango_layout_set_text(layout.get(), text.data(), text.size());
    pango_layout_set_alignment(layout.get(), style.align == TextAlign::Left     ? PANGO_ALIGN_LEFT
                                             : style.align == TextAlign::Center ? PANGO_ALIGN_CENTER
                                                                                : PANGO_ALIGN_RIGHT);
    pango_layout_set_width(layout.get(), style.width < 0 ? -1 : style.width * PANGO_SCALE);
    pango_layout_set_height(layout.get(), style.width < 0 || style.height < 0 ? -1 : style.height * PANGO_SCALE);
    pango_layout_set_wrap(layout.get(), style.width < 0 ? PANGO_WRAP_NONE : PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout.get(),
                               style.width >= 0 && style.height >= 0 ? PANGO_ELLIPSIZE_MIDDLE : PANGO_ELLIPSIZE_NONE);
    PangoRectangle ink, logical;
    pango_layout_get_pixel_extents(layout.get(), &ink, &logical);
    TextRaster result;
    result.metrics = {double(ink.width), double(ink.height), double(logical.width), double(logical.height)};
    // Retain bearings (including negative glyph overhang) and a filtering gutter.
    result.x = ink.x - 1;
    result.y = ink.y - 1;
    result.width = ink.width ? ink.width + 2 : 0;
    result.height = ink.height ? ink.height + 2 : 0;
    if (rasterize && result.width && result.height) {
        surface.reset(cairo_image_surface_create(CAIRO_FORMAT_A8, result.width, result.height));
        Cairo mask(cairo_create(surface.get()), cairo_destroy);
        cairo_set_source_rgba(mask.get(), 1, 1, 1, 1);
        cairo_move_to(mask.get(), -result.x, -result.y);
        pango_cairo_show_layout(mask.get(), layout.get());
        cairo_surface_flush(surface.get());
        const auto status = cairo_status(mask.get());
        if (status != CAIRO_STATUS_SUCCESS)
            throw std::runtime_error(cairo_status_to_string(status));
        result.coverage.resize(size_t(result.width) * result.height);
        const auto data = cairo_image_surface_get_data(surface.get());
        const int stride = cairo_image_surface_get_stride(surface.get());
        for (int y = 0; y < result.height; ++y)
            std::memcpy(result.coverage.data() + size_t(y) * result.width, data + y * stride, result.width);
    }
    return result;
}
} // namespace drawing
