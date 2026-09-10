#include "drawing/context.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace drawing {
void Context::rounded_rectangle(Rect b, double radius) {
    constexpr double pi = std::numbers::pi;
    const double r = std::clamp(radius, 0.0, std::max(0.0, std::min(b.width, b.height) / 2));
    new_sub_path();
    arc(b.x + b.width - r, b.y + r, r, -pi / 2, 0);
    arc(b.x + b.width - r, b.y + b.height - r, r, 0, pi / 2);
    arc(b.x + r, b.y + b.height - r, r, pi / 2, pi);
    arc(b.x + r, b.y + r, r, pi, 3 * pi / 2);
    close_path();
}

void Context::shadow(Rect b, double radius, const ShadowStyle &style, double dpi) {
    if (style.opacity <= 0 || b.width <= 0 || b.height <= 0) return;
    save();
    const double blur = std::max(0.0, style.blur) * dpi;
    const int layers = std::max(1, static_cast<int>(std::ceil(blur * 2)));
    const double opacity = std::clamp(style.opacity, 0.0, .999);
    for (int i = layers; i >= 1; --i) {
        const double spread = blur * i / layers;
        const double weight = 2.0 * (layers - i + 1) / (layers * (layers + 1.0));
        set_color(RGBA(0, 0, 0, 1 - std::pow(1 - opacity, weight)));
        rounded_rectangle({b.x - spread, b.y + style.offset_y * dpi - spread,
                           b.width + 2 * spread, b.height + 2 * spread}, radius + spread);
        fill();
    }
    restore();
}
}
