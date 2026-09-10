#include "library_scrollbar.h"
#include <cmath>
#include <stdexcept>

static void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    LibraryScrollMetrics top(400, 1200, 400, 0, 32);
    check(top.thumb_height == 100 && top.thumb_top == 0, "Thumb must show viewport proportion at top");
    LibraryScrollMetrics bottom(400, 1200, 400, -1200, 32);
    check(bottom.thumb_top + bottom.thumb_height == 400, "Thumb must reach the bottom");
    check(top.offset_at(200, 50) == -600, "Track click must jump directly to the middle");
    LibraryScrollMetrics middle(400, 1200, 400, -600, 32);
    check(middle.offset_at(middle.thumb_top + 17, 17) == -600, "Grabbing thumb must not jump");
    check(middle.offset_at(middle.thumb_top + 47, 17) == -720, "Dragging must follow the grab point");
    check(top.offset_at(-100, 50) == 0 && top.offset_at(900, 50) == -1200,
          "Dragging outside the track must clamp to endpoints");
    LibraryScrollMetrics huge(400, 100000, 400, -50000, 32);
    check(huge.thumb_height == 32, "Large libraries need a usable minimum thumb");
    LibraryScrollMetrics short_track(10, 100, 10, -100, 32);
    check(short_track.thumb_height == 10 && std::isfinite(short_track.offset_at(5, 5)),
          "Tiny windows must not divide by zero");
    LibraryScrollMetrics fits(400, 0, 400, 0, 32);
    check(fits.thumb_height == 400 && fits.offset_at(300, 0) == 0, "Fitting content must not scroll");
    LibraryScrollMetrics scaled(800, 2400, 800, -1200, 64);
    check(scaled.thumb_top == 2 * middle.thumb_top && scaled.offset_at(400, 100) == -1200,
          "DPI scaling must preserve relative position");
}
