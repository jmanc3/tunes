#include "drawing/cached_shadow.h"
#include <algorithm>
#include <cmath>

namespace drawing {
namespace {
constexpr double diffuse_scale = 2.5;
int gaussian_support(double sigma) {
    // Keep the complete tail of the wider component as well as the contact blur.
    return static_cast<int>(std::ceil(4 * diffuse_scale * sigma));
}

void gaussian_blur(Image &image, double sigma) {
    const int support = gaussian_support(sigma);
    std::vector<double> kernel(2 * support + 1, 1);
    double total = 0;
    for (int i = -support; i <= support; ++i) {
        // A two-scale diffusion kernel: mostly broad ambient softness, with a
        // little contact definition. Normalize each Gaussian by its width so
        // the wider component contributes 75% of the energy, not 75% of its peak.
        const double distance = sigma > 0 ? i / sigma : 0;
        const double weight = .25 * std::exp(-.5 * distance * distance) +
            .75 / diffuse_scale * std::exp(-.5 * (distance / diffuse_scale) * (distance / diffuse_scale));
        kernel[i + support] = weight;
        total += weight;
    }
    for (auto &weight : kernel) weight /= total;

    std::vector<double> integral(kernel.size() + 1);
    for (size_t i = 0; i < kernel.size(); ++i)
        integral[i + 1] = integral[i] + kernel[i];
    auto cumulative = [&](int index) {
        return integral[std::clamp(index, 0, static_cast<int>(kernel.size()))];
    };
    // Cairo rectangles have long constant-alpha runs. Integrate the kernel
    // over each run instead of sampling every tap for every output pixel.
    // This is the same convolution, including the two-scale soft falloff.
    struct Run { int begin, end; double value; };
    std::vector<Run> runs;
    auto blur_line = [&](int length, auto read, auto write) {
        runs.clear();
        for (int begin = 0; begin < length;) {
            const double value = read(begin);
            int end = begin + 1;
            while (end < length && read(end) == value) ++end;
            if (value != 0) runs.push_back({begin, end, value});
            begin = end;
        }
        for (int x = 0; x < length; ++x) {
            double alpha = 0;
            for (const auto &run : runs)
                alpha += run.value * (cumulative(x - run.begin + support + 1) -
                                      cumulative(x - run.end + support + 1));
            write(x, alpha);
        }
    };

    // Two separable passes, retaining fractional alpha until the final write.
    // Samples outside the image are transparent, rather than clamped edges.
    std::vector<double> horizontal(image.argb.size());
    for (int y = 0; y < image.height; ++y) {
        const size_t row = size_t(y) * image.width;
        blur_line(image.width, [&](int x) { return double(image.argb[row + x] >> 24); },
                  [&](int x, double alpha) { horizontal[row + x] = alpha; });
    }
    for (int x = 0; x < image.width; ++x)
        blur_line(image.height, [&](int y) { return horizontal[size_t(y) * image.width + x]; },
                  [&](int y, double alpha) {
                      image.argb[size_t(y) * image.width + x] =
                          uint32_t(std::lround(std::clamp(alpha, 0.0, 255.0))) << 24;
                  });
}
}
void CachedShadow::draw(Context &context, Rect bounds, double radius, const ShadowStyle &style, double dpi, double alpha) {
    if (alpha <= 0 || style.opacity <= 0 || bounds.width <= 0 || bounds.height <= 0) return;
    const std::array key{bounds.width, bounds.height, radius, style.blur, style.offset_y, dpi};
    if (!image_ || key != key_) {
        const double sigma = std::max(0.0, style.blur * dpi) / 2;
        const double blur = gaussian_support(sigma);
        const double offset = style.offset_y * dpi;
        // Include the complete convolution footprint and a transparent border
        // for filtered sampling, including fractional geometry and offsets.
        constexpr double guard = 2;
        const double left = blur + guard;
        const double top = std::ceil(std::max(0.0, blur - offset)) + guard;
        const double bottom = std::ceil(std::max(0.0, blur + offset)) + guard;
        auto image = std::make_unique<Image>();
        image->width = std::ceil(bounds.width + 2 * left);
        image->height = std::ceil(bounds.height + top + bottom);
        image->argb.resize(size_t(image->width) * image->height);
        auto cairo = create_cairo_context(reinterpret_cast<unsigned char *>(image->argb.data()),
                                          image->width, image->height, image->width * 4);
        cairo->set_color(RGBA(0, 0, 0, 1));
        cairo->rounded_rectangle({left, top + offset, bounds.width, bounds.height}, radius);
        cairo->fill();
        cairo->flush();
        // Destroy the Cairo surface before modifying its backing pixels.
        cairo.reset();
        gaussian_blur(*image, sigma);
        cairo = create_cairo_context(reinterpret_cast<unsigned char *>(image->argb.data()),
                                     image->width, image->height, image->width * 4);
        // Remove the casting shape at its original position, leaving only the
        // outside shadow, including when the blurred silhouette is offset.
        cairo->set_operator(Composite::Clear);
        cairo->rounded_rectangle({left, top, bounds.width, bounds.height}, radius);
        cairo->fill();
        cairo->flush();
        // Publish a new image identity only when size/style changes. GL uploads
        // it on the draw below; subsequent cards/frames reuse that one texture.
        image_ = std::move(image);
        key_ = key;
        left_ = left;
        top_ = top;
    }
    context.save();
    context.translate(bounds.x - left_, bounds.y - top_);
    context.draw_image(*image_, std::clamp(style.opacity, 0.0, 1.0) * std::clamp(alpha, 0.0, 1.0));
    context.restore();
}
}
