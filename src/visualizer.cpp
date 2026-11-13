#include "visualizer.h"

#include "drawing/context.h"
#include "player.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <projectM-4/projectM.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <utility>
#include <vector>

#ifndef TUNES_VISUALIZER_INSTALL_DIR
#define TUNES_VISUALIZER_INSTALL_DIR "/usr/local/share/tunes"
#endif
#ifndef TUNES_VISUALIZER_SOURCE_DIR
#define TUNES_VISUALIZER_SOURCE_DIR ""
#endif

namespace fs = std::filesystem;

namespace {

// projectM's cost scales with pixels; past roughly a megapixel the extra
// resolution is not visible once scaled up, so large windows render smaller.
constexpr double max_render_pixels = 1280.0 * 800.0;

std::string data_home() {
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return xdg;
    if (const char *home = std::getenv("HOME"); home && *home) return std::string(home) + "/.local/share";
    return {};
}

// Candidate directories in priority order: explicit override, the user's
// data dir, the install prefix, then the build's downloaded packs.
std::vector<fs::path> candidate_dirs(const char *env, const char *leaf, const char *source_leaf) {
    std::vector<fs::path> dirs;
    if (const char *value = std::getenv(env); value && *value) dirs.emplace_back(value);
    if (auto home = data_home(); !home.empty()) dirs.push_back(fs::path(home) / "tunes" / leaf);
    dirs.push_back(fs::path(TUNES_VISUALIZER_INSTALL_DIR) / leaf);
    if (*TUNES_VISUALIZER_SOURCE_DIR) dirs.push_back(fs::path(TUNES_VISUALIZER_SOURCE_DIR) / source_leaf);
    return dirs;
}

std::vector<std::string> scan_presets() {
    std::vector<std::string> presets;
    for (const auto &dir : candidate_dirs("TUNES_VISUALIZER_PRESETS", "presets", "presets")) {
        std::error_code error;
        if (!fs::is_directory(dir, error)) continue;
        for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, error);
             !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
            if (it->path().filename().string().starts_with('.')) {
                if (it->is_directory(error)) it.disable_recursion_pending();
                continue;
            }
            auto ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".milk" && it->is_regular_file(error)) presets.push_back(it->path().string());
        }
        // The first directory with presets wins; mixing packs would duplicate them.
        if (!presets.empty()) break;
    }
    std::sort(presets.begin(), presets.end());
    return presets;
}

std::vector<std::string> texture_dirs() {
    std::vector<std::string> dirs;
    for (const auto &dir : candidate_dirs("TUNES_VISUALIZER_TEXTURES", "textures", "textures/textures")) {
        std::error_code error;
        if (fs::is_directory(dir, error)) dirs.push_back(dir.string());
    }
    return dirs;
}

} // namespace

struct Visualizer::Impl {
    Player *player = nullptr;
    bool open = false;

    projectm_handle pm = nullptr;
    bool failed = false;
    std::string error;
    GLuint fbo = 0, texture = 0;
    int render_w = 0, render_h = 0;

    bool scanned = false;
    std::vector<std::string> presets;
    std::vector<std::size_t> history;
    std::size_t history_pos = 0;
    std::string current_name;
    bool switch_requested = false;
    int failed_loads = 0;
    std::mt19937 rng{std::random_device{}()};
    std::vector<float> samples = std::vector<float>(1 << 14);

    void load(std::size_t index, bool smooth) {
        if (!pm || index >= presets.size()) return;
        current_name = fs::path(presets[index]).stem().string();
        projectm_load_preset_file(pm, presets[index].c_str(), smooth);
    }

    void go_random(bool smooth) {
        if (presets.empty()) return;
        std::size_t index = std::uniform_int_distribution<std::size_t>(0, presets.size() - 1)(rng);
        if (presets.size() > 1 && !history.empty() && index == history[history_pos])
            index = (index + 1) % presets.size();
        if (!history.empty()) history.resize(history_pos + 1);
        history.push_back(index);
        history_pos = history.size() - 1;
        load(index, smooth);
    }

    void go_next(bool smooth) {
        if (history_pos + 1 < history.size()) {
            load(history[++history_pos], smooth);
        } else {
            go_random(smooth);
        }
    }

    void go_previous() {
        if (history_pos > 0) load(history[--history_pos], true);
    }

    bool create(int w, int h) {
        pm = projectm_create();
        if (!pm) {
            failed = true;
            error = "projectM could not start (OpenGL ES 3 shaders unavailable)";
            std::cerr << "tunes: " << error << '\n';
            return false;
        }
        projectm_set_window_size(pm, w, h);
        projectm_set_fps(pm, 60);
        projectm_set_aspect_correction(pm, true);
        projectm_set_preset_duration(pm, 30);
        projectm_set_soft_cut_duration(pm, 3);
        projectm_set_hard_cut_enabled(pm, false);
        projectm_set_beat_sensitivity(pm, 1);
        auto dirs = texture_dirs();
        std::vector<const char *> paths;
        for (auto &dir : dirs) paths.push_back(dir.c_str());
        if (!paths.empty()) projectm_set_texture_search_paths(pm, paths.data(), paths.size());
        projectm_set_preset_switch_requested_event_callback(pm, [](bool, void *data) {
            static_cast<Impl *>(data)->switch_requested = true;
        }, this);
        projectm_set_preset_switch_failed_event_callback(pm, [](const char *file, const char *message, void *data) {
            auto self = static_cast<Impl *>(data);
            std::cerr << "tunes: visualizer preset failed: " << (file ? file : "?") << ": "
                      << (message ? message : "") << '\n';
            // Try another, but give up after a run of failures (e.g. no
            // usable presets) and stay on the built-in idle preset.
            if (++self->failed_loads < 8) self->switch_requested = true;
        }, this);
        go_random(false);
        return true;
    }

    void ensure_target(int w, int h) {
        if (fbo && w == render_w && h == render_h) return;
        if (!texture) glGenTextures(1, &texture);
        if (!fbo) glGenFramebuffers(1, &fbo);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        render_w = w;
        render_h = h;
        if (pm) projectm_set_window_size(pm, w, h);
    }

    void feed_audio() {
        if (!player || !pm) return;
        const std::size_t max_frames = projectm_pcm_get_max_samples();
        for (;;) {
            const auto count = player->read_visualizer_samples(samples.data(), samples.size());
            for (std::size_t offset = 0; offset < count;) {
                const auto frames = std::min(max_frames, (count - offset) / 2);
                projectm_pcm_add_float(pm, samples.data() + offset, frames, PROJECTM_STEREO);
                offset += frames * 2;
            }
            if (count < samples.size()) break;
        }
    }

    void render(int w, int h) {
        if (!pm && (failed || !create(w, h))) return;
        ensure_target(w, h);
        feed_audio();
        projectm_opengl_render_frame_fbo(pm, fbo);
        // Presets leave arbitrary alpha; the composite expects opaque pixels.
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, w, h);
        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        if (std::exchange(switch_requested, false)) go_next(true);
    }

    void release() {
        if (pm) projectm_destroy(pm);
        pm = nullptr;
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (texture) glDeleteTextures(1, &texture);
        fbo = texture = 0;
        render_w = render_h = 0;
        history.clear();
        history_pos = 0;
        current_name.clear();
        switch_requested = false;
        failed_loads = 0;
    }
};

Visualizer::Visualizer() : impl_(std::make_unique<Impl>()) {}

Visualizer::~Visualizer() {
    // GL objects can only be freed with a current context; at exit the
    // window may already be gone, in which case the driver reclaims them.
    if (impl_->player) impl_->player->set_visualizer_tap(false);
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) impl_->release();
}

void Visualizer::open(Player *player) {
    impl_->player = player;
    impl_->open = true;
    if (!impl_->scanned) {
        impl_->presets = scan_presets();
        impl_->scanned = true;
    }
    if (player) {
        player->set_visualizer_tap(false); // Drop stale samples.
        player->set_visualizer_tap(true);
    }
}

void Visualizer::close() {
    impl_->open = false;
    if (impl_->player) impl_->player->set_visualizer_tap(false);
}

bool Visualizer::is_open() const noexcept { return impl_->open; }

bool Visualizer::draw(drawing::Context *cr, double x, double y, double width, double height) {
    if (!impl_->open || width < 1 || height < 1) return false;
    const double reduce = std::min(1.0, std::sqrt(max_render_pixels / (width * height)));
    const int w = std::max(1, int(std::lround(width * reduce)));
    const int h = std::max(1, int(std::lround(height * reduce)));
    if (!cr->native_gl([&] { impl_->render(w, h); })) {
        impl_->error = "The visualizer needs the OpenGL renderer";
        return false;
    }
    if (!impl_->pm) return false;
    cr->save();
    cr->translate(x, y);
    cr->scale(width / w, height / h);
    cr->draw_native_texture(impl_->texture, w, h, true, 1);
    cr->restore();
    return true;
}

void Visualizer::collect(drawing::Context *cr) {
    if (impl_->open || (!impl_->pm && !impl_->fbo)) return;
    cr->native_gl([&] { impl_->release(); });
}

void Visualizer::next_preset() { impl_->go_next(true); }
void Visualizer::previous_preset() { impl_->go_previous(); }
void Visualizer::random_preset() { impl_->go_random(true); }

std::string Visualizer::preset_name() const { return impl_->current_name; }
std::size_t Visualizer::preset_count() const { return impl_->presets.size(); }
std::string Visualizer::error() const { return impl_->error; }
