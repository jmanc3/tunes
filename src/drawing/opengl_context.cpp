#include "drawing/opengl_context.h"
#include "gl_shaders.h"
#include "gl_text.h"
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace drawing {
namespace {
constexpr double pi = std::numbers::pi;
struct Point {
    double x, y;
};
struct Transform {
    double sx = 1, sy = 1, tx = 0, ty = 0;
    Point apply(Point p) const { return {p.x * sx + tx, p.y * sy + ty}; }
    Point inverse(Point p) const { return {(p.x - tx) / sx, (p.y - ty) / sy}; }
};
Rect intersect(Rect a, Rect b) {
    double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::max(0.0, std::min(a.x + a.width, b.x + b.width) - x),
            std::max(0.0, std::min(a.y + a.height, b.y + b.height) - y)};
}
bool empty(Rect r) { return r.width <= 0 || r.height <= 0; }
Rect integral(Rect r) {
    if (empty(r))
        return {r.x, r.y, 0, 0};
    return {std::floor(r.x), std::floor(r.y), std::ceil(r.x + r.width) - std::floor(r.x),
            std::ceil(r.y + r.height) - std::floor(r.y)};
}
struct Texture {
    GLuint id = 0;
    int width = 0, height = 0;
    Texture() { glGenTextures(1, &id); }
    ~Texture() { glDeleteTextures(1, &id); }
    Texture(const Texture &) = delete;
    Texture &operator=(const Texture &) = delete;
    void allocate(int w, int h, bool mask, const void *data = nullptr) {
        width = w;
        height = h;
        glBindTexture(GL_TEXTURE_2D, id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, mask ? GL_R8 : GL_RGBA8, w, h, 0, mask ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE,
                     data);
    }
};
struct Target {
    Rect bounds;
    Texture color;
    GLuint draw = 0, resolve = 0, multisample = 0, stencil = 0;
    int samples;
    bool sampleable;
    Target(Rect b, int count, bool sampled) : bounds(b), samples(count), sampleable(sampled) {
        glGenFramebuffers(1, &draw);
        glGenFramebuffers(1, &resolve);
        glGenRenderbuffers(1, &multisample);
        glGenRenderbuffers(1, &stencil);
    }
    void allocate() {
        int w = bounds.width, h = bounds.height;
        if (sampleable) {
            color.allocate(w, h, false);
            glBindFramebuffer(GL_FRAMEBUFFER, resolve);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color.id, 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                throw std::runtime_error("OpenGL texture framebuffer is incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, draw);
        glBindRenderbuffer(GL_RENDERBUFFER, multisample);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, multisample);
        glBindRenderbuffer(GL_RENDERBUFFER, stencil);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH24_STENCIL8, w, h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            throw std::runtime_error("OpenGL multisample framebuffer is incomplete");
    }
    ~Target() {
        glDeleteFramebuffers(1, &draw);
        glDeleteFramebuffers(1, &resolve);
        glDeleteRenderbuffers(1, &multisample);
        glDeleteRenderbuffers(1, &stencil);
    }
    size_t bytes() const { return size_t(bounds.width) * size_t(bounds.height) * ((sampleable ? 4 : 0) + 8 * samples); }
    void resolved() {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve);
        glBlitFramebuffer(0, 0, bounds.width, bounds.height, 0, 0, bounds.width, bounds.height, GL_COLOR_BUFFER_BIT,
                          GL_NEAREST);
    }
};
struct State {
    Transform transform;
    RGBA color{0, 0, 0, 1};
    Composite composite = Composite::Over;
    LineCap cap = LineCap::Butt;
    FillRule rule = FillRule::Winding;
    double line_width = 2;
    Rect clip;
    std::shared_ptr<Target> clip_mask, source;
};
struct Path {
    std::vector<Point> points;
    bool closed = false;
};
struct Vertex {
    float x, y, u, v, r, g, b, a, opacity;
};
struct BatchKey {
    GLuint texture = 0;
    int kind = 0;
    bool mipmap = false;
    Composite composite = Composite::Over;
    Rect clip{};
    Target *mask = nullptr;
    bool operator==(const BatchKey &b) const {
        return texture == b.texture && kind == b.kind && mipmap == b.mipmap && composite == b.composite &&
               mask == b.mask && clip.x == b.clip.x && clip.y == b.clip.y && clip.width == b.clip.width &&
               clip.height == b.clip.height;
    }
};
GLuint shader(GLenum kind, const char *code) {
    GLuint id = glCreateShader(kind);
    glShaderSource(id, 1, &code, nullptr);
    glCompileShader(id);
    GLint ok;
    glGetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(id, sizeof log, nullptr, log);
        glDeleteShader(id);
        throw std::runtime_error(std::string("OpenGL shader compilation failed: ") + log);
    }
    return id;
}
struct Pipeline {
    GLuint program = 0, vao = 0, vbo = 0;
    GLuint samplers[2]{};
    GLint target, source_kind, has_clip, clip_rect, clip_target, erase;
    Pipeline() {
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenSamplers(2, samplers);
        for (int i = 0; i < 2; ++i) {
            glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, i ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
            glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    ~Pipeline() {
        glDeleteSamplers(2, samplers);
        glDeleteProgram(program);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
    }
    void initialize() {
        auto vs = shader(GL_VERTEX_SHADER, gl_shaders::vertex);
        GLuint fs;
        try {
            fs = shader(GL_FRAGMENT_SHADER, gl_shaders::fragment);
        } catch (...) {
            glDeleteShader(vs);
            throw;
        }
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[4096];
            glGetProgramInfoLog(program, sizeof log, nullptr, log);
            throw std::runtime_error(std::string("OpenGL shader linking failed: ") + log);
        }
        glUseProgram(program);
        target = glGetUniformLocation(program, "target");
        source_kind = glGetUniformLocation(program, "sourceKind");
        has_clip = glGetUniformLocation(program, "hasClip");
        clip_rect = glGetUniformLocation(program, "clipRect");
        clip_target = glGetUniformLocation(program, "clipTarget");
        erase = glGetUniformLocation(program, "erasePass");
        glUniform1i(glGetUniformLocation(program, "sourceTexture"), 0);
        glUniform1i(glGetUniformLocation(program, "clipTexture"), 1);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        const int sizes[] = {2, 2, 4, 1};
        size_t offset = 0;
        for (int i = 0; i < 4; ++i) {
            glEnableVertexAttribArray(i);
            glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void *>(offset));
            offset += sizes[i] * sizeof(float);
        }
    }
};
using TextKey = std::tuple<std::string, std::string, double, bool, double, double, TextAlign>;
struct TextEntry {
    TextRaster raster;
    std::shared_ptr<Texture> texture;
    uint64_t used = 0;
};
struct ImageEntry {
    std::weak_ptr<const char> lifetime;
    std::shared_ptr<Texture> texture;
    uint64_t used = 0;
};

class GLContext final : public OpenGLContext {
    Pipeline pipeline_;
    std::shared_ptr<Target> root_, target_;
    std::vector<std::shared_ptr<Target>> pool_;
    State state_;
    std::vector<State> saved_;
    struct Group {
        State state;
        std::shared_ptr<Target> parent;
        size_t depth;
    };
    std::vector<Group> groups_;
    std::vector<Path> paths_;
    bool new_path_ = true;
    std::vector<Vertex> vertices_;
    BatchKey batch_;
    std::shared_ptr<Target> batch_mask_, batch_source_;
    std::shared_ptr<Texture> batch_texture_;
    std::unordered_map<const Image *, ImageEntry> images_;
    std::map<TextKey, TextEntry> texts_;
    int samples_ = 4, max_texture_ = 0;
    uint64_t frame_ = 0;
    OpenGLStats stats_;

    void bind_target() {
        glBindFramebuffer(GL_FRAMEBUFFER, target_->draw);
        glViewport(0, 0, target_->bounds.width, target_->bounds.height);
        const auto b = target_->bounds;
        glUniform4f(pipeline_.target, b.x, b.y, b.width, b.height);
    }
    std::shared_ptr<Target> target(Rect bounds, bool sampled = true, bool exact_size = false) {
        bounds = integral(bounds);
        // Empty clips still need a legal framebuffer; scissoring suppresses draws.
        bounds.width = std::max(1.0, bounds.width);
        bounds.height = std::max(1.0, bounds.height);
        if (sampled && !exact_size) {
            // Stable size buckets allow small animation/clip changes to reuse FBOs.
            bounds.width = std::min(double(max_texture_), std::ceil(bounds.width / 32) * 32);
            bounds.height = std::min(double(max_texture_), std::ceil(bounds.height / 32) * 32);
        }
        if (bounds.width > max_texture_ || bounds.height > max_texture_)
            throw std::runtime_error("Drawing target exceeds GL_MAX_TEXTURE_SIZE");
        for (auto &entry : pool_) {
            if (entry.use_count() == 1 && entry->sampleable == sampled && entry->bounds.width == bounds.width &&
                entry->bounds.height == bounds.height) {
                entry->bounds = bounds;
                return entry;
            }
        }
        auto result = std::make_shared<Target>(bounds, samples_, sampled);
        result->allocate();
        pool_.push_back(result);
        ++stats_.target_allocations;
        return result;
    }
    void clear_target() {
        bind_target();
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilMask(255);
        glClearColor(0, 0, 0, 0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    }
    void uniforms(const BatchKey &key) {
        bind_target();
        auto clip = intersect(key.clip, target_->bounds);
        auto scissor = integral(clip);
        glEnable(GL_SCISSOR_TEST);
        glScissor(scissor.x - target_->bounds.x,
                  target_->bounds.height - (scissor.y - target_->bounds.y + scissor.height), scissor.width,
                  scissor.height);
        glUniform4f(pipeline_.clip_rect, clip.x, clip.y, clip.width, clip.height);
        glUniform1i(pipeline_.source_kind, key.kind);
        glUniform1i(pipeline_.has_clip, key.mask != nullptr);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, key.texture);
        glBindSampler(0, pipeline_.samplers[key.mipmap ? 1 : 0]);
        glBindSampler(1, pipeline_.samplers[0]);
        if (key.mask) {
            const auto b = key.mask->bounds;
            glUniform4f(pipeline_.clip_target, b.x, b.y, b.width, b.height);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, key.mask->color.id);
            glActiveTexture(GL_TEXTURE0);
        }
    }
    void upload(const std::vector<Vertex> &vertices) {
        glBindVertexArray(pipeline_.vao);
        glBindBuffer(GL_ARRAY_BUFFER, pipeline_.vbo);
        // Orphan the streaming buffer; never wait on an in-flight batch.
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
    }
    void draw_batch() {
        if (vertices_.empty())
            return;
        uniforms(batch_);
        upload(vertices_);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        const bool erase = batch_.composite == Composite::Clear || batch_.composite == Composite::Source;
        glUniform1i(pipeline_.erase, erase);
        if (erase) {
            glBlendFunc(GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
            glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
            ++stats_.draw_calls;
        }
        if (batch_.composite != Composite::Clear) {
            glUniform1i(pipeline_.erase, 0);
            glBlendFunc(GL_ONE, batch_.composite == Composite::Over ? GL_ONE_MINUS_SRC_ALPHA : GL_ONE);
            glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
            ++stats_.draw_calls;
        }
        vertices_.clear();
        batch_mask_.reset();
        batch_source_.reset();
        batch_texture_.reset();
    }
    void start_batch(GLuint texture, int kind, bool mipmap = false) {
        BatchKey key{texture, kind, mipmap, state_.composite, state_.clip, state_.clip_mask.get()};
        if (!vertices_.empty() && (!(key == batch_) || key.composite == Composite::Source || vertices_.size() >= 4092))
            draw_batch();
        batch_ = key;
        batch_mask_ = state_.clip_mask;
    }
    static Vertex vertex(Point p, double u, double v, RGBA c, double alpha) {
        double a = std::clamp(c.a, 0.0, 1.0) * alpha;
        return {float(p.x),     float(p.y),     float(u), float(v),    float(c.r * a),
                float(c.g * a), float(c.b * a), float(a), float(alpha)};
    }
    void source_geometry(const std::vector<Point> &points, double alpha = 1) {
        if (empty(state_.clip) || points.empty())
            return;
        const auto source = state_.source;
        start_batch(source ? source->color.id : 0, source ? 3 : 0);
        batch_source_ = source;
        for (auto p : points) {
            double u = 0, v = 0;
            if (source) {
                u = (p.x - source->bounds.x) / source->bounds.width;
                v = 1 - (p.y - source->bounds.y) / source->bounds.height;
            }
            vertices_.push_back(vertex(p, u, v, source ? RGBA(1, 1, 1, 1) : state_.color, alpha));
        }
    }
    static std::vector<Point> quad(Rect r) {
        return {{r.x, r.y},
                {r.x + r.width, r.y},
                {r.x + r.width, r.y + r.height},
                {r.x, r.y},
                {r.x + r.width, r.y + r.height},
                {r.x, r.y + r.height}};
    }
    void textured_quad(const std::shared_ptr<Texture> &texture, int kind, Rect r, RGBA color, double alpha,
                       bool mipmap = false) {
        if (empty(state_.clip) || empty(r))
            return;
        start_batch(texture->id, kind, mipmap);
        batch_texture_ = texture;
        const auto points = quad(r);
        const double uv[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
        for (int i = 0; i < 6; ++i)
            vertices_.push_back(vertex(state_.transform.apply(points[i]), uv[i][0], uv[i][1], color, alpha));
    }
    Rect path_bounds() const {
        double x = std::numeric_limits<double>::infinity(), y = x, right = -x, bottom = -x;
        for (const auto &path : paths_)
            for (auto p : path.points) {
                x = std::min(x, p.x);
                y = std::min(y, p.y);
                right = std::max(right, p.x);
                bottom = std::max(bottom, p.y);
            }
        return std::isfinite(x) ? Rect{x, y, right - x, bottom - y} : Rect{0, 0, 0, 0};
    }
    void consume_path() {
        paths_.clear();
        new_path_ = true;
    }
    std::vector<Point> fans() const {
        std::vector<Point> result;
        for (const auto &path : paths_)
            for (size_t i = 2; i < path.points.size(); ++i)
                result.insert(result.end(), {path.points[0], path.points[i - 1], path.points[i]});
        return result;
    }
    bool convex() const {
        const Path *shape = nullptr;
        for (const auto &path : paths_)
            if (path.points.size() >= 3) {
                if (shape)
                    return false;
                shape = &path;
            }
        if (!shape)
            return false;
        const auto &p = shape->points;
        double sign = 0, turn = 0;
        for (size_t i = 0; i < p.size(); ++i) {
            auto a = p[i], b = p[(i + 1) % p.size()], c = p[(i + 2) % p.size()];
            double cross = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
            if (std::abs(cross) < 1e-8)
                continue;
            if (sign * cross < 0)
                return false;
            sign = cross;
            turn += std::atan2(cross, (b.x - a.x) * (c.x - b.x) + (b.y - a.y) * (c.y - b.y));
        }
        return std::abs(turn) <= 2 * pi + 1e-5;
    }
    void stencil_geometry(const std::vector<Point> &geometry, Rect bounds, bool stroke) {
        if (geometry.empty() || empty(intersect(bounds, state_.clip)))
            return;
        draw_batch();
        auto region = intersect(integral(bounds), state_.clip);
        BatchKey key{0, 0, false, Composite::Over, region, nullptr};
        uniforms(key);
        glEnable(GL_STENCIL_TEST);
        glStencilMask(255);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
        glDisable(GL_BLEND);
        glStencilFunc(GL_ALWAYS, 1, 255);
        if (stroke)
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        else if (state_.rule == FillRule::EvenOdd)
            glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
        else {
            glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
            glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
        }
        std::vector<Vertex> vertices;
        vertices.reserve(geometry.size());
        for (auto p : geometry)
            vertices.push_back(vertex(p, 0, 0, RGBA(1, 1, 1, 1), 1));
        upload(vertices);
        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
        ++stats_.draw_calls;
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glStencilFunc(GL_NOTEQUAL, 0, state_.rule == FillRule::EvenOdd && !stroke ? 1 : 255);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0);
        source_geometry(quad(integral(bounds)));
        draw_batch();
        glDisable(GL_STENCIL_TEST);
        glStencilMask(255);
    }
    std::shared_ptr<Texture> image_texture(const Image &image) {
        auto it = images_.find(&image);
        if (it != images_.end() && it->second.lifetime.lock() == image.cache_lifetime) {
            it->second.used = frame_;
            return it->second.texture;
        }
        if (image.width > max_texture_ || image.height > max_texture_)
            throw std::runtime_error("Artwork exceeds GL_MAX_TEXTURE_SIZE");
        draw_batch();
        auto texture = std::make_shared<Texture>();
        std::vector<unsigned char> rgba(image.argb.size() * 4);
        for (size_t i = 0; i < image.argb.size(); ++i) {
            auto pixel = image.argb[i];
            rgba[i * 4] = pixel >> 16;
            rgba[i * 4 + 1] = pixel >> 8;
            rgba[i * 4 + 2] = pixel;
            rgba[i * 4 + 3] = pixel >> 24;
        }
        texture->allocate(image.width, image.height, false, rgba.data());
        // Trilinear minification prevents shimmering when large artwork is reduced.
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        images_[&image] = {image.cache_lifetime, texture, frame_};
        ++stats_.image_uploads;
        return texture;
    }
    void trim_caches() {
        size_t bytes = 0;
        for (auto it = images_.begin(); it != images_.end();) {
            if (it->second.lifetime.expired())
                it = images_.erase(it);
            else {
                bytes += size_t(it->second.texture->width) * it->second.texture->height * 16 / 3;
                ++it;
            }
        }
        while (bytes > 128 * 1024 * 1024 && !images_.empty()) {
            auto old = std::min_element(images_.begin(), images_.end(),
                                        [](auto &a, auto &b) { return a.second.used < b.second.used; });
            bytes -= size_t(old->second.texture->width) * old->second.texture->height * 16 / 3;
            images_.erase(old);
        }
        stats_.image_bytes = bytes;
        bytes = 0;
        for (auto &[key, entry] : texts_)
            if (entry.texture)
                bytes += size_t(entry.texture->width) * entry.texture->height;
        while ((bytes > 16 * 1024 * 1024 || texts_.size() > 2048) && !texts_.empty()) {
            auto old = std::min_element(texts_.begin(), texts_.end(),
                                        [](auto &a, auto &b) { return a.second.used < b.second.used; });
            if (old->second.texture)
                bytes -= size_t(old->second.texture->width) * old->second.texture->height;
            texts_.erase(old);
        }
        stats_.text_bytes = bytes;
        size_t idle = 0;
        std::erase_if(pool_, [&](const auto &t) {
            if (t.use_count() != 1)
                return false;
            idle += t->bytes();
            return idle > std::max(size_t(64 * 1024 * 1024), root_->bytes() * 2);
        });
    }

  public:
    GLContext(int width, int height) {
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_);
        GLint maximum;
        glGetIntegerv(GL_MAX_SAMPLES, &maximum);
        samples_ = std::min(4, maximum);
        pipeline_.initialize();
        vertices_.reserve(4096);
        resize(width, height);
        begin_frame();
    }
    ~GLContext() override = default;
    OpenGLStats stats() const override {
        auto result = stats_;
        result.image_bytes = result.text_bytes = 0;
        for (const auto &[key, entry] : images_)
            result.image_bytes += size_t(entry.texture->width) * entry.texture->height * 16 / 3;
        for (const auto &[key, entry] : texts_)
            if (entry.texture)
                result.text_bytes += size_t(entry.texture->width) * entry.texture->height;
        return result;
    }
    void resize(int width, int height) override {
        if (width <= 0 || height <= 0)
            throw std::invalid_argument("Invalid OpenGL target size");
        draw_batch();
        saved_.clear();
        groups_.clear();
        state_ = {};
        root_.reset();
        target_.reset();
        pool_.clear();
        root_ = target({0, 0, double(width), double(height)}, false);
        target_ = root_;
        state_.clip = root_->bounds;
        consume_path();
    }
    void begin_frame() override {
        draw_batch();
        if (!groups_.empty() || !saved_.empty())
            throw std::logic_error("Unbalanced drawing state at frame boundary");
        ++frame_;
        trim_caches();
        state_ = {};
        state_.clip = root_->bounds;
        target_ = root_;
        consume_path();
        glUseProgram(pipeline_.program);
        glBindVertexArray(pipeline_.vao);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DITHER);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        clear_target();
    }
    void save() override { saved_.push_back(state_); }
    void restore() override {
        if (saved_.empty() || (!groups_.empty() && saved_.size() <= groups_.back().depth))
            throw std::logic_error("Unbalanced drawing restore");
        // A queued batch retains references to any textures the restored state drops.
        state_ = std::move(saved_.back());
        saved_.pop_back();
    }
    void translate(double x, double y) override {
        state_.transform.tx += x * state_.transform.sx;
        state_.transform.ty += y * state_.transform.sy;
    }
    void scale(double x, double y) override {
        state_.transform.sx *= x;
        state_.transform.sy *= y;
    }
    void new_sub_path() override { new_path_ = true; }
    void move_to(double x, double y) override {
        paths_.push_back({{state_.transform.apply({x, y})}, false});
        new_path_ = false;
    }
    void line_to(double x, double y) override {
        if (new_path_ || paths_.empty())
            move_to(x, y);
        else
            paths_.back().points.push_back(state_.transform.apply({x, y}));
    }
    void arc(double x, double y, double radius, double start, double end) override {
        if (radius < 0)
            throw std::invalid_argument("Negative arc radius");
        while (end < start)
            end += 2 * pi;
        end = std::min(end, start + 2 * pi);
        const double device_radius = radius * std::max(std::abs(state_.transform.sx), std::abs(state_.transform.sy));
        const double step = device_radius > .2 ? 2 * std::acos(std::clamp(1 - .2 / device_radius, -1.0, 1.0)) : pi / 2;
        const int count = std::clamp(int(std::ceil((end - start) / std::max(.001, step))), 1, 8192);
        for (int i = 0; i <= count; ++i) {
            double a = start + (end - start) * i / count;
            line_to(x + radius * std::cos(a), y + radius * std::sin(a));
        }
    }
    void close_path() override {
        if (!paths_.empty() && !paths_.back().points.empty()) {
            paths_.back().closed = true;
            // Cairo retains the starting point for a following line/arc.
            auto point = paths_.back().points.front();
            paths_.push_back({{point}, false});
            new_path_ = false;
        }
    }
    void rectangle(double x, double y, double width, double height) override {
        move_to(x, y);
        line_to(x + width, y);
        line_to(x + width, y + height);
        line_to(x, y + height);
        close_path();
    }
    void fill_preserve() override {
        auto geometry = fans();
        if (convex())
            source_geometry(geometry);
        else
            stencil_geometry(geometry, path_bounds(), false);
    }
    void fill() override {
        fill_preserve();
        consume_path();
    }
    void clip() override {
        auto bounds = intersect(state_.clip, path_bounds());
        // Axis-aligned rectangles are the common UI clip and need no mask allocation.
        const Path *rectangle_path = nullptr;
        for (const auto &p : paths_)
            if (p.points.size() >= 3) {
                if (rectangle_path) {
                    rectangle_path = nullptr;
                    break;
                }
                rectangle_path = &p;
            }
        bool rect = rectangle_path && rectangle_path->points.size() == 4;
        if (rect)
            for (size_t i = 0; i < 4; ++i) {
                auto a = rectangle_path->points[i], b = rectangle_path->points[(i + 1) % 4];
                if (a.x != b.x && a.y != b.y)
                    rect = false;
            }
        if (rect || empty(bounds)) {
            if (rect) {
                // Text and close_path leave move-only subpaths. They have no
                // fill area and must not enlarge the rectangular clip.
                const auto &points = rectangle_path->points;
                double left = points[0].x, right = left, top = points[0].y, bottom = top;
                for (const auto &p : points) {
                    left = std::min(left, p.x); right = std::max(right, p.x);
                    top = std::min(top, p.y); bottom = std::max(bottom, p.y);
                }
                bounds = intersect(state_.clip, {left, top, right - left, bottom - top});
            }
            state_.clip = bounds;
            consume_path();
            return;
        }
        draw_batch();
        auto parent = target_;
        auto saved = state_;
        target_ = target(bounds);
        clear_target();
        state_.clip = bounds;
        state_.source.reset();
        state_.color = RGBA(1, 1, 1, 1);
        state_.composite = Composite::Over;
        fill();
        draw_batch();
        target_->resolved();
        auto mask = target_;
        target_ = parent;
        state_ = saved;
        state_.clip = bounds;
        state_.clip_mask = mask;
        bind_target();
    }
    void stroke() override {
        if (state_.line_width <= 0 || state_.transform.sx == 0 || state_.transform.sy == 0) {
            consume_path();
            return;
        }
        std::vector<Point> mesh;
        const double half = state_.line_width / 2;
        auto triangle = [&](Point a, Point b, Point c) {
            mesh.insert(mesh.end(), {state_.transform.apply(a), state_.transform.apply(b), state_.transform.apply(c)});
        };
        auto disk = [&](Point center) {
            double radius = half * std::max(std::abs(state_.transform.sx), std::abs(state_.transform.sy));
            int n = std::clamp(int(std::ceil(pi / std::acos(std::clamp(1 - .15 / std::max(.15, radius), -1.0, 1.0)))),
                               8, 256);
            for (int i = 0; i < n; ++i)
                triangle(center,
                         {center.x + half * std::cos(2 * pi * i / n), center.y + half * std::sin(2 * pi * i / n)},
                         {center.x + half * std::cos(2 * pi * (i + 1) / n),
                          center.y + half * std::sin(2 * pi * (i + 1) / n)});
        };
        for (const auto &path : paths_) {
            std::vector<Point> points;
            for (auto p : path.points) {
                p = state_.transform.inverse(p);
                if (points.empty() || std::hypot(p.x - points.back().x, p.y - points.back().y) > 1e-8)
                    points.push_back(p);
            }
            if (points.size() == 1 && path.points.size() > 1 && state_.cap == LineCap::Round)
                disk(points.front());
            if (points.size() < 2)
                continue;
            if (path.closed &&
                std::hypot(points.front().x - points.back().x, points.front().y - points.back().y) < 1e-8)
                points.pop_back();
            if (points.size() < 2)
                continue;
            size_t segments = points.size() - (path.closed ? 0 : 1);
            std::vector<Point> normals;
            for (size_t i = 0; i < segments; ++i) {
                auto a = points[i], b = points[(i + 1) % points.size()];
                double length = std::hypot(b.x - a.x, b.y - a.y);
                Point direction{(b.x - a.x) / length, (b.y - a.y) / length};
                Point n{-direction.y * half, direction.x * half};
                normals.push_back(n);
                if (!path.closed && state_.cap == LineCap::Square) {
                    if (i == 0) {
                        a.x -= direction.x * half;
                        a.y -= direction.y * half;
                    }
                    if (i + 1 == segments) {
                        b.x += direction.x * half;
                        b.y += direction.y * half;
                    }
                }
                Point p{a.x + n.x, a.y + n.y}, q{b.x + n.x, b.y + n.y}, r{b.x - n.x, b.y - n.y},
                    s{a.x - n.x, a.y - n.y};
                triangle(p, q, r);
                triangle(p, r, s);
            }
            // Default Cairo miter joins (limit 10), with bevel fallback.
            size_t first = path.closed ? 0 : 1, last = path.closed ? points.size() : points.size() - 1;
            for (size_t i = first; i < last; ++i) {
                auto center = points[i], a = normals[(i + segments - 1) % segments], b = normals[i % segments];
                double denom = half * half + a.x * b.x + a.y * b.y;
                for (double side : {-1.0, 1.0}) {
                    Point p{center.x + side * a.x, center.y + side * a.y},
                        q{center.x + side * b.x, center.y + side * b.y};
                    Point m = center;
                    if (std::abs(denom) > 1e-10) {
                        m.x += side * (a.x + b.x) * half * half / denom;
                        m.y += side * (a.y + b.y) * half * half / denom;
                    }
                    if (std::hypot(m.x - center.x, m.y - center.y) > half * 10)
                        m = center;
                    triangle(p, m, q);
                    triangle(p, center, q);
                }
            }
            if (!path.closed && state_.cap == LineCap::Round) {
                disk(points.front());
                disk(points.back());
            }
        }
        Rect bounds = path_bounds();
        double pad = state_.line_width * 5 * std::max(std::abs(state_.transform.sx), std::abs(state_.transform.sy));
        bounds = {bounds.x - pad, bounds.y - pad, bounds.width + 2 * pad, bounds.height + 2 * pad};
        stencil_geometry(mesh, bounds, true);
        consume_path();
    }
    void set_color(RGBA color) override {
        state_.color = color;
        state_.source.reset();
    }
    void set_line_width(double width) override { state_.line_width = width; }
    void set_line_cap(LineCap cap) override { state_.cap = cap; }
    void set_fill_rule(FillRule rule) override { state_.rule = rule; }
    void set_operator(Composite op) override { state_.composite = op; }
    void push_group() override {
        draw_batch();
        groups_.push_back({state_, target_, saved_.size()});
        target_ = target(intersect(state_.clip, target_->bounds));
        clear_target();
    }
    void push_group(Rect bounds) override {
        draw_batch();
        groups_.push_back({state_, target_, saved_.size()});
        auto a = state_.transform.apply({bounds.x, bounds.y});
        auto b = state_.transform.apply({bounds.x + bounds.width, bounds.y + bounds.height});
        state_.clip =
            intersect(state_.clip, {std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)});
        target_ = target(intersect(state_.clip, target_->bounds));
        clear_target();
        consume_path();
    }
    void pop_group_to_source() override {
        if (groups_.empty() || saved_.size() != groups_.back().depth)
            throw std::logic_error("Unbalanced drawing group");
        draw_batch();
        target_->resolved();
        auto source = target_;
        auto group = std::move(groups_.back());
        groups_.pop_back();
        state_ = std::move(group.state);
        target_ = std::move(group.parent);
        state_.source = source;
        bind_target();
    }
    void paint_source(double alpha) override {
        auto bounds = intersect(state_.clip, target_->bounds);
        if (state_.source && (state_.composite == Composite::Over || state_.composite == Composite::Add))
            bounds = intersect(bounds, state_.source->bounds);
        source_geometry(quad(bounds), std::clamp(alpha, 0.0, 1.0));
    }
    void draw_image(const Image &image, double alpha, ImageFilter filter) override {
        auto a = state_.transform.apply({0, 0});
        auto b = state_.transform.apply({double(image.width), double(image.height)});
        Rect bounds{std::min(a.x, b.x), std::min(a.y, b.y), std::abs(b.x - a.x), std::abs(b.y - a.y)};
        if (image.width > 0 && image.height > 0 && image.argb.size() >= size_t(image.width) * image.height &&
            !empty(intersect(bounds, state_.clip))) {
            auto texture = image_texture(image);
            textured_quad(texture, 1, {0, 0, double(image.width), double(image.height)}, RGBA(1, 1, 1, 1),
                          std::clamp(alpha, 0.0, 1.0), filter == ImageFilter::Good);
        }
        consume_path();
    }
    TextMetrics text(double x, double y, const std::string &value, const TextStyle &style, bool draw) override {
        TextKey key{value, style.font, style.size, style.bold, style.width, style.height, style.align};
        auto [it, inserted] = texts_.try_emplace(key);
        auto &entry = it->second;
        entry.used = frame_;
        if (inserted)
            entry.raster = rasterize_gl_text(value, style, draw);
        if (draw && !entry.texture && entry.raster.width && entry.raster.height) {
            if (entry.raster.coverage.empty())
                entry.raster = rasterize_gl_text(value, style, true);
            if (entry.raster.width > max_texture_ || entry.raster.height > max_texture_)
                throw std::runtime_error("Text exceeds GL_MAX_TEXTURE_SIZE");
            draw_batch();
            entry.texture = std::make_shared<Texture>();
            entry.texture->allocate(entry.raster.width, entry.raster.height, true, entry.raster.coverage.data());
            entry.raster.coverage.clear();
            entry.raster.coverage.shrink_to_fit();
            ++stats_.text_uploads;
        }
        set_color(style.color);
        if (draw && entry.texture) {
            auto glyphs = [&](double dx, double dy, RGBA color) {
                textured_quad(
                    entry.texture, 2,
                    {dx + entry.raster.x, dy + entry.raster.y, double(entry.raster.width), double(entry.raster.height)},
                    color, 1);
            };
            if (style.shadow.opacity > 0) {
                double opacity = std::clamp(style.shadow.opacity, 0.0, .999);
                for (int row = -2; row <= 2; ++row)
                    for (int col = -2; col <= 2; ++col) {
                        double weight = (3 - std::abs(row)) * (3 - std::abs(col)) / 81.0;
                        glyphs(x + col * std::max(0.0, style.shadow.blur) * style.dpi / 2,
                               y + (style.shadow.offset_y + row * std::max(0.0, style.shadow.blur) / 2) * style.dpi,
                               RGBA(0, 0, 0, 1 - std::pow(1 - opacity, weight)));
                    }
            }
            glyphs(std::round(x), std::round(y), style.color);
            move_to(std::round(x), std::round(y));
        }
        return entry.raster.metrics;
    }
    void gaussian_blur(double sigma, double amount) override {
        if (sigma <= 0 || amount <= 0) return;
        if (!groups_.empty()) throw std::logic_error("Cannot blur inside a drawing group");
        draw_batch();
        auto saved = state_;
        auto original = target(root_->bounds, true, true);
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, root_->draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, original->resolve);
        glBlitFramebuffer(0, 0, root_->bounds.width, root_->bounds.height,
                         0, 0, root_->bounds.width, root_->bounds.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        state_ = State{};
        state_.clip = root_->bounds;
        auto source = original;
        glUseProgram(pipeline_.program);
        glUniform1f(glGetUniformLocation(pipeline_.program, "blurSigma"), std::min(sigma, 20.0));
        for (int kind : {4, 5}) {
            target_ = target(root_->bounds, true, true);
            clear_target();
            start_batch(source->color.id, kind);
            batch_source_ = source;
            for (auto p : quad(root_->bounds))
                vertices_.push_back(vertex(p, p.x / root_->bounds.width, 1 - p.y / root_->bounds.height,
                                           RGBA(1, 1, 1, 1), 1));
            draw_batch();
            target_->resolved();
            source = target_;
        }
        target_ = root_;
        clear_target();
        state_.composite = Composite::Add;
        state_.source = original;
        paint_source(1 - std::clamp(amount, 0.0, 1.0));
        state_.source = source;
        paint_source(amount);
        draw_batch();
        state_ = std::move(saved);
        bind_target();
    }
    void flush() override {
        if (!groups_.empty())
            throw std::logic_error("Cannot present inside a drawing group");
        draw_batch();
        glDisable(GL_SCISSOR_TEST);
        // Resolve straight into EGL's single-sample back buffer: one GPU blit.
        glBindFramebuffer(GL_READ_FRAMEBUFFER, root_->draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, root_->bounds.width, root_->bounds.height, 0, 0, root_->bounds.width,
                          root_->bounds.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFlush();
    }
};
} // namespace
std::unique_ptr<OpenGLContext> create_opengl_context(int width, int height) {
    GLint major = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    if (major < 3)
        throw std::runtime_error("OpenGL ES 3 context is required");
    return std::make_unique<GLContext>(width, height);
}
} // namespace drawing
