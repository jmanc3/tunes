#pragma once
#include "drawing/context.h"

namespace drawing {
struct TextRaster {
    TextMetrics metrics{};
    int x = 0, y = 0, width = 0, height = 0;
    std::vector<unsigned char> coverage;
};
// Pango shapes once per cache miss. Rasterization is optional for measurements.
TextRaster rasterize_gl_text(const std::string &text, const TextStyle &style, bool rasterize);
} // namespace drawing
