#pragma once
#include "drawing/context.h"
#include <array>

namespace drawing {
// One Cairo raster shared by all instances of a shape. Position and the current
// transform are deliberately excluded from the key (scroll/pulse reuse it).
class CachedShadow {
public:
    // Style opacity and fade alpha are applied at draw time to the full-opacity raster.
    void draw(Context &context, Rect bounds, double radius, const ShadowStyle &style, double dpi, double alpha = 1);

private:
    std::unique_ptr<Image> image_;
    std::array<double, 6> key_{};
    double left_ = 0, top_ = 0;
};
}
