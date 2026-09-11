#include "theme.h"

#include "client/raw_windowing.h"
#include "container.h"
#include "player.h"
#include "playback_queue.h"
#include "popup_input.h"
#include "library_scrollbar.h"
#include "client/windowing.h"
#include "utility.h"
#include "audio_data.h"
#include "audio_conversion.h"
#include "album_art.h"
#include "playlist_art.h"
#include "drawing/cached_shadow.h"
#include "session_state.h"
#include "ThreadPool.h"
#include "pipewire_settings.h"

#include <chrono>
#include <charconv>
#include <cctype>
#include <algorithm>
#include <tuple>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <filesystem>
#include <magic.h>
#include <cmath>
#include <gdk/gdk.h>
#include <unordered_map>
#include <optional>
#include <random>
#include <map>
#include <set>
#include <utility>


static float artwork_fade_duration_ms = 100.0f;

using drawing::ShadowStyle;

// Independently tunable library artwork, library text, and popup shadows.
static ShadowStyle library_art_shadow{.6, 2, 0};
static ShadowStyle library_text_shadow{0, 1.5, 1};
static ShadowStyle popup_shadow{.6, 18, 4};
static double popup_corner_radius = 12; // Logical pixels.
static bool first_scale_event_happened = false;

static std::string mylar_font = "SF Pro";
static Player *player = nullptr;
static PlaybackQueue playback_queue;

static long get_current_time_in_ms() {
    using namespace std::chrono;
    milliseconds currentTime = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    return currentTime.count();
}

bool is_audio(const std::filesystem::path& path) {
    magic_t magic = magic_open(MAGIC_MIME_TYPE);
    if (!magic)
        return false;

    if (magic_load(magic, nullptr) != 0) {
        magic_close(magic);
        return false;
    }

    const char* mime = magic_file(magic, path.c_str());
    bool audio = mime && std::string_view(mime).starts_with("audio/");

    magic_close(magic);
    return audio;
}

static void sort_tracks(std::vector<Option> &playable) {
    auto parse_number = [](const std::string &text, int fallback) {
        int number = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
        if (error != std::errc{} || number <= 0 ||
            (end != text.data() + text.size() && *end != '/'))
            return fallback;
        return number;
    };
    for (auto &option : playable) {
        option.album_all_lower = option.album;
        std::transform(option.album_all_lower.begin(), option.album_all_lower.end(),
                       option.album_all_lower.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        option.disc_num = parse_number(option.disc, 1);
        option.track_num = parse_number(option.track, std::numeric_limits<int>::max());
    }
    std::sort(playable.begin(), playable.end(), [](const Option &a, const Option &b) {
        return std::tie(a.album_all_lower, a.disc_num, a.track_num, a.name, a.full)
             < std::tie(b.album_all_lower, b.disc_num, b.track_num, b.name, b.full);
    });

}

struct StartupState {
    std::filesystem::path state_file = session_state_path();
    SessionState session = load_session(state_file);
    std::optional<SessionState> last_saved;
    std::string music_root;
    std::vector<std::string> queue;
    bool explicit_files = false;
    bool defer_queue_until_scan = false;
    std::vector<std::filesystem::path> inputs;
    ThreadPool writer{1};
};

static void configure_startup(StartupState &startup, int argc, char **argv) {
    namespace fs = std::filesystem;
    startup.music_root = normalize_music_directory(startup.session.music_root);
    bool have_folder = false;
    auto &inputs = startup.inputs;
    for (int i = 1; i < argc; ++i) {
        const auto path = fs::canonical(argv[i]);
        std::error_code error;
        if (fs::equivalent(path, fs::path(g_get_home_dir()), error))
            throw std::runtime_error("Refusing to open your home directory as a music library: " +
                                     path.string() + ". Choose a specific music folder, such as ~/Music.");
        if (!fs::is_directory(path)) {
            if (!fs::is_regular_file(path) || !is_audio(path))
                throw std::runtime_error("Expected an audio file or music folder: " + path.string());
            startup.explicit_files = true;
        }
        inputs.push_back(path);
    }
    for (const auto &path : inputs) {
        if (fs::is_directory(path)) {
            if (!have_folder) {
                startup.music_root = path.string();
                have_folder = true;
            }
        } else {
            startup.queue.push_back(path.string());
        }
    }
    startup.defer_queue_until_scan = startup.explicit_files && have_folder;
    // Preserve the previous global session before switching to another library.
    if (!startup.session.music_root.empty()) {
        const auto previous_path = library_session_path(startup.session.music_root);
        if (!fs::exists(previous_path) && !save_session(previous_path, startup.session))
            std::cerr << "Could not migrate the previous library session.\n";
    }
    startup.session = load_library_session(startup.music_root, startup.session);
    startup.state_file = library_session_path(startup.music_root);
}

static void initialize_playback(StartupState &startup) {
    player->set_volume(startup.session.volume);
    if (!player->set_sample_rate(startup.session.sample_rate))
        std::cerr << player->last_error() << '\n';
    if (startup.defer_queue_until_scan)
        return;
    // Opening a library restores paused playback; explicit files request playback.
    if (startup.explicit_files) {
        const bool loaded = player->restore_session(std::move(startup.queue), 0, 0);
        if (!player->queue().empty() && (!loaded || !player->start()))
            std::cerr << "Could not start playback: " << player->last_error() << '\n';
    } else {
        if (!player->restore_session(startup.session.queue, startup.session.current_index, startup.session.seconds))
            std::cerr << "Could not restore playback: " << player->last_error() << '\n';
    }
}

struct ArtRefresh {
    RawApp *app = nullptr;
    RawWindow *window = nullptr;
    std::weak_ptr<AlbumArtCache> cache;
    int timer = -1;
    bool animating = false;
};

// Workers publish only immutable images. Window access and redraws stay on the
// event thread, with one coalesced refresh while artwork jobs are outstanding.
static void poll_artwork(const std::shared_ptr<ArtRefresh> &refresh) {
    if (refresh->timer >= 0 || !refresh->app)
        return;
    refresh->timer = windowing::timer(refresh->app, 16, [refresh](void *) {
        refresh->timer = -1;
        auto cache = refresh->cache.lock();
        if (!cache || !windowing::has_window(refresh->window))
            return;
        // Read pending first: the last worker publishes its change before
        // decrementing the job count, so its final redraw cannot be lost.
        const bool pending = cache->pending();
        const bool animating = std::exchange(refresh->animating, false);
        if (cache->take_changed() || animating)
            windowing::redraw(refresh->window);
        if (pending || refresh->animating)
            poll_artwork(refresh);
    }, nullptr);
}

struct TrackDisplay {
    std::string title;
    std::string artist;
    AlbumArtCache::Handle art;
    std::string album;
    std::string length;
};

struct LibraryScanResult {
    std::vector<AlbumOption> albums;
    std::vector<std::string> launch_queue;
    bool launch = false;
};

struct ClosingAlbum {
    Container *card = nullptr;
    double initial_height = 0; // Logical pixels; contents retain their final layout.
    double initial_gap = 0;
    std::chrono::steady_clock::time_point start;
    Bounds bounds;
    double visible_height = 0;
    double occupied_height = 0;
    std::size_t row = 0;
};

struct QueueRemoval {
    PlaybackQueue::Entry entry;
    std::size_t position;
    std::chrono::steady_clock::time_point start;
};

struct PlaylistNameEdit {
    std::string id;
    std::string text;
    std::size_t caret = 0;
    std::size_t anchor = 0;
    double scroll = 0;

    std::size_t previous(std::size_t pos) const {
        if (pos) --pos;
        while (pos && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80) --pos;
        return pos;
    }
    std::size_t next(std::size_t pos) const {
        if (pos < text.size()) ++pos;
        while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xc0) == 0x80) ++pos;
        return pos;
    }
    void erase_selection() {
        const auto begin = std::min(caret, anchor), end = std::max(caret, anchor);
        text.erase(begin, end - begin);
        caret = anchor = begin;
    }
};

struct PlaylistTrackDrag {
    std::string playlist_id;
    std::string path;
    std::size_t slot = 0;
    double grab_y = 0;
    bool active = false;
    bool can_drop = false;
    bool timer_pending = false;
    std::chrono::steady_clock::time_point last_update;
};

struct PlaylistTrackTarget {
    std::string playlist_id;
    std::string path;
};

struct RootData : UserData {
    Container *queue_overlay = nullptr;
    Container *context_overlay = nullptr;
    Bounds context_bounds;
    Bounds queue_bounds;
    double context_x = 0, context_y = 0;
    std::vector<std::string> context_paths;
    std::string context_playlist_id;
    std::string context_source_playlist_id;
    bool context_whole_playlist = false;
    bool playlist_submenu = false;
    Bounds playlist_menu_bounds;
    Bounds playlist_list_bounds;
    Bounds playlist_scroll_thumb;
    double playlist_scroll = 0;
    double playlist_scroll_max = 0;
    bool playlist_scroll_dragging = false;
    double playlist_scroll_grab = 0;
    PlaylistNameEdit playlist_edit;
    PlaylistTrackDrag playlist_track_drag;
    std::optional<PlaylistTrackTarget> playlist_remove_press;
    playlist_art::Chooser playlist_art_chooser;
    std::string playlist_art_chooser_id;
    bool playlist_art_chooser_poll_pending = false;
    std::future<playlist_art::ImportResult> playlist_art_job;
    std::string playlist_art_job_id;
    std::string playlist_art_previous_file;
    bool playlist_art_removing = false;
    std::string playlist_art_error_id;
    std::string playlist_art_error;
    std::uint64_t playlist_drag_generation = 0;
    std::vector<std::pair<std::uint64_t, Bounds>> queue_rows;
    std::vector<QueueRemoval> queue_removals;
    std::uint64_t queue_drag = 0;
    double queue_dx = 0, queue_dy = 0;
    double queue_scroll = 0, queue_scroll_max = 0;
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::shared_ptr<AlbumArtCache> artwork;
    AlbumArtPrefetch artwork_prefetch;
    int artwork_prefetch_pixels = 0;
    drawing::CachedShadow library_shadow;
    std::optional<std::chrono::steady_clock::time_point> initial_shadow_fade;
    double library_shadow_alpha = 0;
    drawing::CachedShadow playback_bar_shadow;
    drawing::CachedShadow album_panel_shadow;
    drawing::CachedShadow menu_shadow;
    drawing::CachedShadow context_shadow;
    drawing::CachedShadow playlist_menu_shadow;
    drawing::CachedShadow settings_shadow;
    Container *library_scrollbar = nullptr;
    double library_scroll_max = 0;
    double scrollbar_grab = 0;
    bool scrollbar_dragging = false;
    std::chrono::steady_clock::time_point scrollbar_activity{};
    std::shared_ptr<ArtRefresh> artwork_refresh;
    std::size_t album_first = 0;
    std::size_t album_end = 0;
    std::unordered_map<std::string, TrackDisplay> tracks;
    Container *playback_bar = nullptr;
    Container *library = nullptr;
    Container *album_panel = nullptr;
    Container *expanded_album = nullptr;
    std::optional<std::chrono::steady_clock::time_point> album_scroll_start;
    double album_scroll_from = 0;
    std::optional<std::chrono::steady_clock::time_point> album_reveal_start;
    double album_reveal = 1;
    Container *outgoing_album = nullptr;
    double album_transition_from_height = 0;
    double album_transition_from_gap = 0;
    double album_visible_gap = 0;
    double album_visible_height = 0;
    std::size_t album_columns = 1;
    std::vector<ClosingAlbum> closing_albums;
    Container *last_album_clicked = nullptr;
    std::chrono::steady_clock::time_point last_album_click_time;
    double last_album_click_x = 0;
    double last_album_click_y = 0;
    Container *album_double_click_target = nullptr;
    bool album_double_click_handled = false;
    Container *artwork_preview = nullptr;
    Container *settings_menu = nullptr;
    Bounds settings_bounds;
    std::string settings_error;
    std::vector<unsigned> output_rates;
    std::future<PipeWireSettingsResult> pipewire_action;
    std::string pipewire_status;
    bool pipewire_error = false;
    bool pipewire_config_exists = false;
    bool settings_information = false;
    AlbumArtCache::Handle current_art;
    std::optional<std::chrono::steady_clock::time_point> current_art_fade;
    bool current_art_startup_fade = true;
    AlbumArtCache::Handle preview_art;
    std::string preview_playlist_id;
    Bounds preview_bounds;
    StartupState *startup = nullptr;
    double dpi = 1;
    double initial_scroll_offset = 0;
    bool scroll_restored = false;
    bool scroll_restored_at_preferred_scale = false;
    bool first_frame_shown = false;
    bool playback_initialized = false;
    std::chrono::steady_clock::time_point artwork_frame_time;
    std::chrono::steady_clock::time_point last_session_save;
    std::future<LibraryScanResult> scan;
    std::future<std::vector<Option>> playlist_metadata;
    bool scan_failed = false;
    ThreadPool scanner{1};
    ThreadPool playlist_art_writer{1};
};

static RootData *root_data_for(Container *c) {
    while (c->parent)
        c = c->parent;
    return static_cast<RootData *>(c->user_data);
}

static Bounds draw_text(drawing::Context *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, double h, RGBA color, bool bold, int align = 0,
                        const ShadowStyle *shadow = nullptr, double dpi = 1) {
    // float alpha_mix = 1.0;
    // if (draw) {
    //     draw = first_scale_event_happened;
    //     static bool first_event = true;
    //     static long start_time = 0;
    //     if (draw) {
    //         if (first_event) {
    //             first_event = false;
    //             start_time = get_current_time_in_ms();
    //         }
    //         long delta = get_current_time_in_ms() - start_time;
    //         alpha_mix = ((float) (delta)) / 300.0f;
    //         if (alpha_mix > 1) {
    //             alpha_mix = 1.0;
    //         }
    //     }
    // }
    // color.a = alpha_mix;
    drawing::TextStyle style;
    style.font = font;
    style.size = size;
    style.bold = bold;
    style.align = static_cast<drawing::TextAlign>(align);
    style.width = wrap;
    style.height = h;
    style.color = color;
    if (shadow) style.shadow = *shadow;
    style.dpi = dpi;
    const auto metrics = cr->text(x, y, text, style, draw);
    return Bounds(metrics.ink_width, metrics.ink_height, metrics.width, metrics.height);
}

static void rounded_rectangle(drawing::Context *cr, const Bounds &b, double radius) {
    cr->rounded_rectangle({b.x, b.y, b.w, b.h}, radius);
}


static void paint_button_bg(Container *root, Container *c) {
    auto root_data = (RootData *) root->user_data;
    auto dpi = root_data->window->raw_window->dpi;
    auto cr = root_data->window->raw_window->drawing_context;
    
    if (c->state.mouse_pressing) {
        cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->set_color(theme_colors::shadow);
        cr->fill();
    } else if (c->state.mouse_hovering) {
        cr->rectangle(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cr->set_color(theme_colors::shadow_soft);
        cr->fill();
    }
}

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

struct AlbumData : UserData {
    AlbumOption album;
    std::string playlist_id;
    std::string art_file;
    std::optional<std::chrono::steady_clock::time_point> play_pulse_start;
    std::weak_ptr<const AlbumTexture> palette_source;
    RGBA background_color = theme_colors::album_fallback;
    RGBA secondary_color = theme_colors::album_secondary;
    RGBA accent_color = theme_colors::album_secondary;
    std::string name;
    std::string artist;
    AlbumArtCache::Handle art;
    std::optional<std::chrono::steady_clock::time_point> detail_fade;
};

// Grid cards and the playback cover use the same preview and crossfade path.
static void paint_artwork(RootData *rd, drawing::Context *cr, const AlbumArtCache::Handle &handle,
                          const std::shared_ptr<const AlbumTexture> &art,
                          const std::optional<std::chrono::steady_clock::time_point> &fade,
                          double x, double y, double size, bool fade_preview = true) {
    const auto preview = rd->artwork->preview(handle);
    const bool detailed = preview && art != preview;
    double blend = fade_preview ? 0 : 1;
    if (fade_preview && detailed && fade) {
        blend = artwork_fade_duration_ms <= 0 ? 1.0 : std::clamp(
            std::chrono::duration<double, std::milli>(rd->artwork_frame_time - *fade).count() /
            artwork_fade_duration_ms, 0.0, 1.0);
        if (blend < 1) {
            rd->artwork_refresh->animating = true;
            poll_artwork(rd->artwork_refresh);
        }
    }
    auto paint_art = [&](const auto &image, double alpha) {
        cr->save();
        const double scale = std::min(size / image->width, size / image->height);
        cr->translate(x + (size - image->width * scale) / 2,
                        y + (size - image->height * scale) / 2);
        // Stretch the tiny preview to the same display size as the detail.
        cr->scale(scale, scale);
        cr->draw_image(*image, alpha);
        cr->restore();
    };
    if (detailed && blend < 1) {
        // Bound the transient target to this cover, rather than the whole grid.
        cr->push_group({x, y, size, size});
        paint_art(preview, 1 - blend);
        cr->set_operator(drawing::Composite::Add);
        paint_art(art, blend);
        cr->pop_group_to_source();
        cr->set_operator(drawing::Composite::Over);
        cr->paint_source();
    } else {
        paint_art(art, 1);
    }

}

static void playback_changed(Container *root);
static void checkpoint_session(Container *root, bool force = false);
static void finish_playlist_name_edit(Container *root, bool commit);
static void add_context_to_playlist(Container *root, const std::string &id);
static void remove_context_from_playlist(Container *root);
static void remove_playlist_track(Container *root, std::string id, std::string path);
static void delete_context_playlist(Container *root);
static void finish_playlist_track_drag(Container *root);
static void playlist_art_action(Container *root, const std::string &id);
static AlbumArtCache::Handle playlist_art_handle(RootData *rd, const PlaylistState &playlist);

static AlbumArtCache::Handle playback_art(RootData *rd, const std::string &path,
                                          const std::string &playlist_id, int pixels) {
    const auto track = rd->tracks.find(path);
    // Playlist playback checks this track alone, even if its library album
    // shares a cover from another track in the collection.
    auto art = playlist_id.empty() && track != rd->tracks.end()
        ? track->second.art : rd->artwork->create({path});
    rd->artwork->request(art, pixels);
    if (!playlist_id.empty() && rd->startup && rd->artwork->preview_ready(art) && !rd->artwork->image(art)) {
        const auto &playlists = rd->startup->session.playlists;
        const auto playlist = std::find_if(playlists.begin(), playlists.end(),
            [&](const auto &entry) { return entry.id == playlist_id; });
        if (playlist != playlists.end()) {
            art = playlist_art_handle(rd, *playlist);
            rd->artwork->request(art, pixels);
        }
    }
    if (rd->artwork->pending()) poll_artwork(rd->artwork_refresh);
    return art;
}

static void cancel_playlist_track_drag(RootData *rd) {
    rd->playlist_track_drag = {};
    rd->playlist_remove_press.reset();
    ++rd->playlist_drag_generation;
}

static void observe_queue() {
    playback_queue.observe(player->queue(), player->current_index(), [](const auto &queued, const auto &current) {
        return queued == current || preferred_audio_path(queued) == current;
    });
}

static void commit_queue(Container *root) {
    player->queue() = playback_queue.paths();
    player->queue_changed();
    playback_changed(root);
}

static std::vector<std::string> album_paths(const AlbumOption &album) {
    std::vector<std::string> paths;
    for (const auto &song : album.songs) paths.push_back(song.full);
    return paths;
}

static Bounds playlist_trigger_bounds(RootData *rd) {
    return Bounds(rd->context_bounds.x, rd->context_bounds.y + 122 * rd->dpi,
                  rd->context_bounds.w, 40 * rd->dpi).intersection(rd->context_bounds);
}

static void layout_playlist_submenu(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    const auto viewport = root->real_bounds;
    const auto parent = rd->context_bounds;
    const double d = rd->dpi;
    const std::size_t count = rd->startup ? rd->startup->session.playlists.size() : 0;
    const double right_space = std::max(0.0, viewport.right() - parent.right());
    const double left_space = std::max(0.0, parent.x - viewport.x);
    const bool right = right_space >= 264 * d || right_space >= left_space;
    const double width = std::min(264 * d, right ? right_space : left_space);
    const double height = std::min((44 + 40 * std::min<std::size_t>(7, count)) * d, viewport.h);
    const double y = std::clamp(playlist_trigger_bounds(rd).y, viewport.y, viewport.bottom() - height);
    rd->playlist_menu_bounds = Bounds(right ? parent.right() : parent.x - width, y, width, height);
    rd->playlist_list_bounds = Bounds(rd->playlist_menu_bounds.x, y + std::min(42 * d, height),
                                     width, std::max(0.0, height - 44 * d));
    const auto list = rd->playlist_list_bounds;
    rd->playlist_scroll_max = std::max(0.0, count * 40 * d - list.h);
    rd->playlist_scroll = std::clamp(rd->playlist_scroll, 0.0, rd->playlist_scroll_max);
    rd->playlist_scroll_thumb = {};
    if (rd->playlist_scroll_max > 0 && list.h > 0) {
        const double thumb = std::min(list.h, std::max(24 * d, list.h * list.h / (count * 40 * d)));
        rd->playlist_scroll_thumb = Bounds(list.right() - 9 * d,
            list.y + (list.h - thumb) * rd->playlist_scroll / rd->playlist_scroll_max, 5 * d, thumb);
    }
}

static void paint_playlist_submenu(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->playlist_submenu || !rd->startup)
        return;
    auto cr = rd->window->raw_window->drawing_context;
    const auto b = rd->playlist_menu_bounds;
    const double d = rd->dpi;
    cr->save();
    rd->playlist_menu_shadow.draw(*cr, {b.x, b.y, b.w, b.h}, popup_corner_radius * d, popup_shadow, d);
    cr->set_color(theme().surface);
    rounded_rectangle(cr, b, popup_corner_radius * d);
    cr->fill_preserve();
    cr->clip();
    auto row = [&](const Bounds &bounds, const std::string &label, bool bold, double padding) {
        if (bounds_contains(bounds, root->mouse_current_x, root->mouse_current_y)) {
            cr->set_color(theme().button);
            set_rect(cr, bounds); cr->fill();
        }
        draw_text(cr, bounds.x + 16 * d, bounds.y + 10 * d, label, 12 * d, true,
                  mylar_font, std::max(0.0, bounds.w - padding * d), 24 * d, theme().text, bold);
    };
    row(Bounds(b.x, b.y + 2 * d, b.w, 40 * d), "+ New playlist", true, 32);
    cr->set_color(theme().divider);
    cr->rectangle(b.x + 12 * d, b.y + 41 * d, std::max(0.0, b.w - 24 * d), d); cr->fill();
    set_rect(cr, rd->playlist_list_bounds); cr->clip();
    const auto &playlists = rd->startup->session.playlists;
    const auto first = static_cast<std::size_t>(rd->playlist_scroll / (40 * d));
    for (auto i = first; i < playlists.size(); ++i) {
        const Bounds bounds(b.x, rd->playlist_list_bounds.y + i * 40 * d - rd->playlist_scroll, b.w, 40 * d);
        if (bounds.y >= rd->playlist_list_bounds.bottom()) break;
        row(bounds, playlists[i].name, false, rd->playlist_scroll_max > 0 ? 40 : 32);
    }
    if (rd->playlist_scroll_max > 0) {
        cr->set_color(with_alpha(theme().scrollbar, rd->playlist_scroll_dragging ? .8 : .45));
        rounded_rectangle(cr, rd->playlist_scroll_thumb, 2.5 * d); cr->fill();
    }
    cr->restore();
}

static void drag_playlist_scrollbar(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    const double travel = rd->playlist_list_bounds.h - rd->playlist_scroll_thumb.h;
    if (travel > 0) {
        rd->playlist_scroll = std::clamp((root->mouse_current_y - rd->playlist_list_bounds.y -
            rd->playlist_scroll_grab) / travel, 0.0, 1.0) * rd->playlist_scroll_max;
        layout_playlist_submenu(root);
    }
    windowing::redraw(rd->window->raw_window);
}

static void open_queue_context(Container *root, std::vector<std::string> paths, std::string playlist_id = {}, bool whole_playlist = false) {
    auto rd = static_cast<RootData *>(root->user_data);
    finish_playlist_name_edit(root, true);
    cancel_playlist_track_drag(rd);
    rd->context_paths = std::move(paths);
    rd->context_playlist_id = std::move(playlist_id);
    rd->context_source_playlist_id = rd->context_playlist_id;
    rd->context_whole_playlist = whole_playlist && !rd->context_playlist_id.empty();
    rd->playlist_submenu = false;
    rd->playlist_scroll = 0;
    rd->playlist_scroll_dragging = false;
    rd->context_x = root->mouse_current_x;
    rd->context_y = root->mouse_current_y;
    rd->last_album_clicked = nullptr;
    rd->album_double_click_target = nullptr;
    rd->context_overlay->exists = true;
    layout(root, root, root->real_bounds);
    windowing::redraw(rd->window->raw_window);
}

static void fill_queue_overlay(Container *root, Container *overlay, bool context = false) {
    auto rd = static_cast<RootData *>(root->user_data);
    (context ? rd->context_overlay : rd->queue_overlay) = overlay;
    overlay->name = context ? "queue-context" : "queue-flyout";
    overlay->type = ::fullycustom;
    overlay->z_index = context ? 91 : 90;
    overlay->exists = false;
    overlay->when_drag_end_is_click = false;
    overlay->minimum_x_distance_to_move_before_drag_begins = 6;
    overlay->minimum_y_distance_to_move_before_drag_begins = 6;
    overlay->pre_layout = [context](Container *root, Container *, const Bounds &b) {
        auto rd = static_cast<RootData *>(root->user_data);
        const auto d = rd->dpi;
        const double w = std::min((context ? 224 : 420) * d, context ? b.w / 2 : b.w);
        const double h = std::min((context ? (rd->context_playlist_id.empty() ? 164 : 204) : 520) * d,
                                  std::max(0.0, b.h - (context ? 0 : 104 * d)));
        (context ? rd->context_bounds : rd->queue_bounds) = Bounds(std::clamp(context ? rd->context_x : b.right() - w - 12 * d,
                                           b.x, b.right() - w),
            std::clamp(context ? rd->context_y : b.bottom() - 104 * d - h, b.y, b.bottom() - h), w, h);
        if (context) layout_playlist_submenu(root);
    };
    if (context) {
        overlay->when_mouse_enters_container = overlay->when_mouse_motion = [](Container *root, Container *) {
            auto rd = static_cast<RootData *>(root->user_data);
            const double x = root->mouse_current_x, y = root->mouse_current_y;
            const bool open = bounds_contains(playlist_trigger_bounds(rd), x, y) ||
                (rd->playlist_submenu && bounds_contains(rd->playlist_menu_bounds, x, y));
            if (open != rd->playlist_submenu && !rd->playlist_scroll_dragging) {
                rd->playlist_submenu = open;
                layout_playlist_submenu(root);
            }
            windowing::redraw(rd->window->raw_window);
        };
        overlay->when_mouse_leaves_container = [](Container *root, Container *) {
            auto rd = static_cast<RootData *>(root->user_data);
            if (!rd->playlist_scroll_dragging) rd->playlist_submenu = false;
            windowing::redraw(rd->window->raw_window);
        };
    }
    overlay->when_paint = [context](Container *root, Container *) {
        auto rd = static_cast<RootData *>(root->user_data);
        observe_queue();
        auto cr = rd->window->raw_window->drawing_context;
        const auto b = context ? rd->context_bounds : rd->queue_bounds;
        const auto d = rd->dpi;
        auto text = [&](double x, double y, const std::string &s, int size, bool bold, double width) {
            draw_text(cr, x, y, s, (size) * d, true, mylar_font, std::max(0.0, width), 24 * d,
                      theme().text, bold);
        };
        cr->save();
        (context ? rd->context_shadow : rd->menu_shadow).draw(*cr, {b.x, b.y, b.w, b.h}, popup_corner_radius * d, popup_shadow, d);
        cr->set_color(theme().surface);
        rounded_rectangle(cr, b, popup_corner_radius * d);
        cr->fill_preserve();
        cr->clip();
        if (context) {
            const char *labels[] = {"Play Next", "Play After All Next", "Add to Queue", "Add to playlist",
                                   rd->context_whole_playlist ? "Delete playlist" : "Remove from playlist"};
            const int count = rd->context_playlist_id.empty() ? 4 : 5;
            for (int i = 0; i < count; ++i) {
                Bounds row(b.x, b.y + (2 + i * 40) * d, b.w, 40 * d);
                if (bounds_contains(row, root->mouse_current_x, root->mouse_current_y) || (i == 3 && rd->playlist_submenu)) {
                    cr->set_color(theme().button);
                    cr->rectangle(row.x, row.y, row.w, row.h); cr->fill();
                }
                if (i == 4) {
                    draw_text(cr, row.x + 16 * d, row.y + 10 * d, labels[i], 12 * d, true,
                              mylar_font, std::max(0.0, row.w - 32 * d), 24 * d, theme().danger_text, false);
                } else {
                    text(row.x + 16 * d, row.y + 10 * d, labels[i], 12, false, row.w - (i == 3 ? 48 : 32) * d);
                }
                if (i == 3) text(row.right() - 25 * d, row.y + 9 * d, "›", 14, false, 18 * d);
            }
            cr->restore();
            paint_playlist_submenu(root);
            return;
        }
        text(b.x + 16 * d, b.y + 12 * d, "Queue", 16, true, b.w - 150 * d);
        text(b.right() - 114 * d, b.y + 15 * d, "Clear all", 11, true, 74 * d);
        text(b.right() - 30 * d, b.y + 10 * d, "×", 20, false, 26 * d);
        text(b.x + 16 * d, b.y + 42 * d, "Drag ↕ to reorder · Swipe ↔ to remove", 10, false, b.w - 32 * d);
        const double top = b.y + 70 * d;
        cr->rectangle(b.x, top, b.w, std::max(0.0, b.bottom() - top)); cr->clip();
        auto visible = std::vector<PlaybackQueue::Entry>();
        if (auto current = playback_queue.current()) visible.push_back(*current);
        const auto &items = playback_queue.entries();
        visible.insert(visible.end(), items.begin() + playback_queue.upcoming_begin(), items.end());
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(rd->queue_removals, [&](const auto &r) { return now - r.start >= std::chrono::milliseconds(220); });
        for (const auto &r : rd->queue_removals)
            visible.insert(visible.begin() + std::min(r.position, visible.size()), r.entry);
        rd->queue_rows.clear();
        double y = top - rd->queue_scroll;
        int previous_group = -1;
        for (std::size_t i = 0; i < visible.size(); ++i) {
            const auto &e = visible[i];
            const bool current = playback_queue.current() && playback_queue.current()->id == e.id;
            const int group = current ? 0 : e.category == PlaybackQueue::Category::Next ? 1 : 2;
            if (group != previous_group) {
                text(b.x + 16 * d, y + 4 * d, group == 0 ? "Now Playing" : group == 1 ? "Play Next" : "Up Next · Queue", 11, true, b.w - 32 * d);
                y += 28 * d;
                previous_group = group;
            }
            auto removed = std::find_if(rd->queue_removals.begin(), rd->queue_removals.end(),
                                       [&](const auto &r) { return r.entry.id == e.id; });
            const double progress = removed == rd->queue_removals.end() ? 0 : std::clamp(
                std::chrono::duration<double, std::milli>(now - removed->start).count() / 220.0, 0.0, 1.0);
            const double ease = progress * progress * (3 - 2 * progress);
            const double height = 64 * d * (1 - ease);
            Bounds row(b.x + 8 * d, y, b.w - 16 * d, height);
            if (!current && removed == rd->queue_removals.end()) rd->queue_rows.push_back({e.id, row});
            const double swipe = rd->queue_drag == e.id && std::abs(rd->queue_dx) > std::abs(rd->queue_dy) ? rd->queue_dx : 0;
            if (row.bottom() > top && row.y < b.bottom()) {
                cr->save();
                cr->rectangle(row.x, row.y, row.w, row.h); cr->clip();
                cr->push_group();
                cr->translate(swipe + ease * b.w, 0);
                cr->set_color(theme().selection);
                cr->rectangle(row.x, row.y + 2 * d, row.w, 60 * d); cr->fill();
                const auto handle = playback_art(rd, e.path, e.playlist_id, 48 * d);
                if (const auto art = rd->artwork->image(handle))
                    paint_artwork(rd, cr, handle, art, {}, row.x + 6 * d, row.y + 8 * d, 48 * d, false);
                const auto it = rd->tracks.find(e.path);
                if (it != rd->tracks.end()) {
                    const auto &track = it->second;
                    text(row.x + 64 * d, row.y + 10 * d, track.title, 12, true, row.w - 102 * d);
                    text(row.x + 64 * d, row.y + 30 * d, track.album.empty() ? "Unknown album" : track.album, 10, false, row.w - 102 * d);
                } else {
                    text(row.x + 64 * d, row.y + 10 * d, std::filesystem::path(e.path).stem().string(), 12, true, row.w - 102 * d);
                    text(row.x + 64 * d, row.y + 30 * d, "Unknown album", 10, false, row.w - 102 * d);
                }
                if (!current) text(row.right() - 30 * d, row.y + 18 * d, "×", 18, false, 24 * d);
                cr->pop_group_to_source(); cr->paint_source(1 - ease);
                cr->restore();
                if (rd->queue_drag && std::abs(rd->queue_dy) > std::abs(rd->queue_dx) &&
                    bounds_contains(row, root->mouse_current_x, root->mouse_current_y)) {
                    cr->set_color(theme().accent);
                    cr->rectangle(row.x, row.y, row.w, 3 * d); cr->fill();
                }
            }
            y += height;
        }
        rd->queue_scroll_max = std::max(0.0, y + rd->queue_scroll - b.bottom());
        const auto old_scroll = rd->queue_scroll;
        rd->queue_scroll = std::clamp(rd->queue_scroll, 0.0, rd->queue_scroll_max);
        if (rd->queue_scroll_max > 0) {
            const double viewport = b.bottom() - top;
            const double thumb = viewport * viewport / (viewport + rd->queue_scroll_max);
            cr->set_color(theme_colors::drop_indicator);
            cr->rectangle(b.right() - 5 * d,
                top + (viewport - thumb) * rd->queue_scroll / rd->queue_scroll_max, 3 * d, thumb);
            cr->fill();
        }
        if (old_scroll != rd->queue_scroll) rd->artwork_refresh->animating = true;
        poll_artwork(rd->artwork_refresh);
        if (visible.empty()) text(b.x + 16 * d, top + 20 * d, "Your queue is empty", 12, false, b.w - 32 * d);
        if (!rd->queue_removals.empty()) {
            rd->artwork_refresh->animating = true;
            poll_artwork(rd->artwork_refresh);
        }
        cr->restore();
    };
    auto remove = [context](Container *root, std::uint64_t id) {
        auto rd = static_cast<RootData *>(root->user_data);
        observe_queue();
        const auto &entries = playback_queue.entries();
        auto it = std::find_if(entries.begin() + playback_queue.upcoming_begin(), entries.end(),
                               [id](const auto &e) { return e.id == id; });
        if (it == entries.end()) return;
        auto position = static_cast<std::size_t>(it - entries.begin()) - playback_queue.upcoming_begin() + (playback_queue.current() ? 1 : 0);
        rd->queue_removals.push_back({*it, position, std::chrono::steady_clock::now()});
        playback_queue.remove(id);
        commit_queue(root);
    };
    overlay->when_mouse_down = [context](Container *root, Container *c) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (context) {
            rd->playlist_scroll_dragging = false;
            const double x = root->mouse_current_x, y = root->mouse_current_y;
            if (c->state.mouse_button_pressed == BTN_LEFT && rd->playlist_submenu && rd->playlist_scroll_max > 0 &&
                bounds_contains(rd->playlist_list_bounds, x, y) && x >= rd->playlist_list_bounds.right() - 14 * rd->dpi) {
                rd->playlist_scroll_dragging = true;
                rd->playlist_scroll_grab = bounds_contains(rd->playlist_scroll_thumb, x, y)
                    ? y - rd->playlist_scroll_thumb.y : rd->playlist_scroll_thumb.h / 2;
                drag_playlist_scrollbar(root);
            }
            return;
        }
        rd->queue_drag = 0;
        rd->queue_dx = rd->queue_dy = 0;
        if (context || c->state.mouse_button_pressed != BTN_LEFT ||
            root->mouse_current_y < rd->queue_bounds.y + 70 * rd->dpi) return;
        for (const auto &[id, b] : rd->queue_rows)
            if (bounds_contains(b, root->mouse_current_x, root->mouse_current_y)) rd->queue_drag = id;
    };
    overlay->when_drag = [context](Container *root, Container *) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (context) {
            if (rd->playlist_scroll_dragging) drag_playlist_scrollbar(root);
            return;
        }
        rd->queue_dx = root->mouse_current_x - root->mouse_initial_x;
        rd->queue_dy = root->mouse_current_y - root->mouse_initial_y;
        if (rd->queue_drag && std::abs(rd->queue_dy) > std::abs(rd->queue_dx)) {
            if (root->mouse_current_y < rd->queue_bounds.y + 100 * rd->dpi) rd->queue_scroll -= 12 * rd->dpi;
            if (root->mouse_current_y > rd->queue_bounds.bottom() - 30 * rd->dpi) rd->queue_scroll += 12 * rd->dpi;
            rd->queue_scroll = std::clamp(rd->queue_scroll, 0.0, rd->queue_scroll_max);
        }
        windowing::redraw(rd->window->raw_window);
    };
    overlay->when_drag_start = overlay->when_drag;
    overlay->when_drag_end = [remove, context](Container *root, Container *) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (context) {
            rd->playlist_scroll_dragging = false;
            windowing::redraw(rd->window->raw_window);
            return;
        }
        if (rd->queue_drag) {
            if (std::abs(rd->queue_dx) > 70 * rd->dpi && std::abs(rd->queue_dx) > std::abs(rd->queue_dy))
                remove(root, rd->queue_drag);
            else if (std::abs(rd->queue_dy) > std::abs(rd->queue_dx)) {
                observe_queue();
                for (const auto &[id, b] : rd->queue_rows)
                    if (bounds_contains(b, root->mouse_current_x, root->mouse_current_y)) {
                        playback_queue.move(rd->queue_drag, id);
                        commit_queue(root);
                        break;
                    }
            }
        }
        rd->queue_drag = 0;
        rd->queue_dx = rd->queue_dy = 0;
        windowing::redraw(rd->window->raw_window);
    };
    overlay->when_clicked = [remove, context](Container *root, Container *c) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            const double x = root->mouse_current_x, y = root->mouse_current_y;
            // A right click replaces the context popup while preserving the queue.
            if (context && (bounds_contains(rd->context_bounds, x, y) ||
                (rd->playlist_submenu && bounds_contains(rd->playlist_menu_bounds, x, y)))) return;
            rd->context_overlay->exists = false;
            if (rd->queue_overlay->exists && bounds_contains(rd->queue_bounds, x, y)) {
                if (context) {
                    forward_popup_right_click(root, {rd->context_overlay});
                } else if (y >= rd->queue_bounds.y + 70 * rd->dpi) {
                    observe_queue();
                    for (const auto &[id, row] : rd->queue_rows) {
                        if (!bounds_contains(row, x, y)) continue;
                        for (const auto &entry : playback_queue.entries())
                            if (entry.id == id) {
                                open_queue_context(root, {entry.path});
                                rd->context_source_playlist_id = entry.playlist_id;
                                break;
                            }
                        break;
                    }
                }
            } else {
                forward_popup_right_click(root, {rd->context_overlay, rd->queue_overlay});
            }
            windowing::redraw(rd->window->raw_window);
            return;
        }
        if (c->state.mouse_button_pressed != BTN_LEFT) return;
        const auto b = context ? rd->context_bounds : rd->queue_bounds;
        const auto d = rd->dpi;
        const double x = root->mouse_current_x, y = root->mouse_current_y;
        rd->queue_drag = 0;
        rd->playlist_scroll_dragging = false;
        if (context && rd->playlist_submenu && bounds_contains(rd->playlist_menu_bounds, x, y)) {
            if (bounds_contains(Bounds(rd->playlist_menu_bounds.x, rd->playlist_menu_bounds.y + 2 * d,
                                      rd->playlist_menu_bounds.w, 40 * d), x, y)) {
                add_context_to_playlist(root, {});
            } else if (rd->startup && bounds_contains(rd->playlist_list_bounds, x, y) &&
                       (rd->playlist_scroll_max == 0 || x < rd->playlist_list_bounds.right() - 14 * d)) {
                const auto index = static_cast<std::size_t>((y - rd->playlist_list_bounds.y + rd->playlist_scroll) / (40 * d));
                if (index < rd->startup->session.playlists.size()) {
                    const auto id = rd->startup->session.playlists[index].id;
                    add_context_to_playlist(root, id);
                }
            }
            windowing::redraw(rd->window->raw_window);
            return;
        }
        if (!bounds_contains(b, x, y)) {
            rd->context_overlay->exists = false;
            rd->queue_overlay->exists = false;
        }
        else if (context) {
            if (bounds_contains(playlist_trigger_bounds(rd), x, y)) {
                rd->playlist_submenu = true;
                layout_playlist_submenu(root);
                windowing::redraw(rd->window->raw_window);
                return;
            }
            const int action = static_cast<int>((y - b.y - 2 * d) / (40 * d));
            if (action == 4 && !rd->context_playlist_id.empty()) {
                if (rd->context_whole_playlist) delete_context_playlist(root);
                else remove_context_from_playlist(root);
                return;
            }
            if (action >= 0 && action < 3) {
                observe_queue();
                playback_queue.add(rd->context_paths, static_cast<PlaybackQueue::Action>(action), rd->context_source_playlist_id);
                commit_queue(root);
            }
            c->exists = false;
        } else if (y < b.y + 40 * d && x > b.right() - 40 * d) {
            c->exists = false;
            rd->context_overlay->exists = false;
        }
        else if (y < b.y + 40 * d && x > b.right() - 120 * d) {
            observe_queue();
            const auto begin = playback_queue.upcoming_begin();
            for (auto i = begin; i < playback_queue.entries().size(); ++i)
                rd->queue_removals.push_back({playback_queue.entries()[i],
                    i - begin + (playback_queue.current() ? 1 : 0), std::chrono::steady_clock::now()});
            playback_queue.clear();
            commit_queue(root);
        } else if (y >= b.y + 70 * d) {
            for (const auto &[id, row] : rd->queue_rows)
                if (x > row.right() - 38 * d && bounds_contains(row, x, y)) { remove(root, id); break; }
        }
        windowing::redraw(rd->window->raw_window);
    };
    overlay->when_fine_scrolled = [context](Container *root, Container *, double, double y, bool) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (!context) rd->queue_scroll = std::clamp(rd->queue_scroll - y, 0.0, rd->queue_scroll_max);
        else if (rd->playlist_submenu && bounds_contains(rd->playlist_list_bounds, root->mouse_current_x, root->mouse_current_y)) {
            rd->playlist_scroll = std::clamp(rd->playlist_scroll - y, 0.0, rd->playlist_scroll_max);
            layout_playlist_submenu(root);
        }
        windowing::redraw(rd->window->raw_window);
    };
}

static void play_album(const AlbumData &source, std::size_t index) {
    const auto &album = source.album;
    if (index >= album.songs.size()) return;
    auto &queue = player->queue();
    queue.clear();
    for (const auto &song : album.songs)
        queue.push_back(song.full);
    playback_queue.reset(queue, index, source.playlist_id);
    if (!player->play_queued_item(index))
        std::cerr << "Album playback failed: " << player->last_error() << '\n';
}

struct AlbumTrackLayout {
    double art_size;
    double text_width;
    double column_width;
    double column_gap;
    std::size_t rows;
    bool single_column = false;

    Bounds track_bounds(const Bounds &panel, double dpi, std::size_t index) const {
        return Bounds(panel.x + 56 * dpi + (index / rows) * (column_width + column_gap),
                      panel.y + (94 + (index % rows) * 32) * dpi, column_width, 32 * dpi);
    }

    Bounds hit_bounds(const Bounds &panel, double dpi, std::size_t index) const {
        auto bounds = track_bounds(panel, dpi, index);
        if (single_column) {
            bounds.x -= 24 * dpi;
            bounds.w += 24 * dpi;
        }
        return bounds;
    }

    Bounds remove_bounds(const Bounds &panel, double dpi, std::size_t index) const {
        if (!single_column) return {};
        const auto row = track_bounds(panel, dpi, index);
        return Bounds(row.right() - 36 * dpi, row.y + 4 * dpi, 24 * dpi, 24 * dpi).intersection(row);
    }

    Bounds duration_bounds(const Bounds &panel, double dpi, std::size_t index) const {
        const auto row = track_bounds(panel, dpi, index);
        // Keep 12 logical pixels of outer padding on both sides of each row.
        return Bounds(row.right() - (single_column ? 94 : 58) * dpi, row.y + 7 * dpi, 46 * dpi, 22 * dpi);
    }
};

static AlbumTrackLayout album_track_layout(double width, double dpi, std::size_t count, bool single_column = false) {
    const double art_size = std::min(360 * dpi, width * .4);
    const double text_width = std::max(0.0, width - art_size - 88 * dpi);
    const double gap = 24 * dpi;
    // Playlists always remain a linear list; only albums may flow into columns.
    const auto allowed = static_cast<std::size_t>(std::max(1.0,
        std::floor((text_width + gap) / (312 * dpi + gap))));
    const auto columns = !single_column && count > 7 ? std::min(allowed, (count + 6) / 7) : 1;
    const auto rows = std::max<std::size_t>(1, (count + columns - 1) / columns);
    return {art_size, text_width, std::max(0.0, (text_width - (columns - 1) * gap) / columns),
            gap, rows, single_column};
}

static void schedule_playlist_track_drag(Container *root);

static void update_playlist_track_drag(Container *root, bool scroll = true) {
    auto rd = static_cast<RootData *>(root->user_data);
    auto &drag = rd->playlist_track_drag;
    if (!drag.active) return;
    if (!rd->expanded_album || !rd->album_panel || !rd->library->interactable ||
        rd->context_overlay->exists || rd->queue_overlay->exists) {
        cancel_playlist_track_drag(rd);
        return;
    }
    const auto album = static_cast<AlbumData *>(rd->expanded_album->user_data);
    if (album->playlist_id != drag.playlist_id ||
        std::none_of(album->album.songs.begin(), album->album.songs.end(), [&](const auto &song) { return song.full == drag.path; })) {
        cancel_playlist_track_drag(rd);
        return;
    }
    const double dpi = rd->dpi;
    const auto viewport = rd->library->real_bounds;
    const auto tracks = album_track_layout(rd->album_panel->real_bounds.w, dpi, album->album.songs.size(), true);
    auto first = tracks.hit_bounds(rd->album_panel->real_bounds, dpi, 0);
    const double x = root->mouse_current_x, y = root->mouse_current_y;
    const bool in_column = x >= first.x && x < first.right();
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::clamp(std::chrono::duration<double>(now - drag.last_update).count(), 0.0, .05);
    drag.last_update = now;
    const double edge = std::min(48 * dpi, viewport.h / 3);
    if (scroll && in_column && edge > 0) {
        double delta = 0;
        if (y < viewport.y + edge) {
            delta = 600 * dpi * elapsed * std::clamp((viewport.y + edge - y) / edge, 0.0, 1.0);
            delta = std::min(delta, std::max(0.0, viewport.y + edge - first.y));
        } else if (y > viewport.bottom() - edge) {
            const double bottom = first.y + album->album.songs.size() * 32 * dpi;
            delta = -std::min(600 * dpi * elapsed * std::clamp((y - viewport.bottom() + edge) / edge, 0.0, 1.0),
                              std::max(0.0, bottom - viewport.bottom() + edge));
        }
        const double offset = std::clamp(rd->library->scroll_v_real + delta, -rd->library_scroll_max, 0.0);
        if (offset != rd->library->scroll_v_real) {
            rd->album_scroll_start.reset();
            rd->library->scroll_v_real = offset;
            rd->scrollbar_activity = now;
            layout(root, root, root->real_bounds);
            first = tracks.hit_bounds(rd->album_panel->real_bounds, dpi, 0);
        }
    }
    const double bottom = first.y + album->album.songs.size() * 32 * dpi;
    const double drop_margin = std::max(16 * dpi, edge);
    drag.can_drop = in_column && y >= viewport.y && y < viewport.bottom() &&
                    y >= first.y - drop_margin && y <= bottom + drop_margin;
    drag.slot = static_cast<std::size_t>(std::clamp(std::floor((y - first.y) / (32 * dpi) + .5),
                                                  0.0, static_cast<double>(album->album.songs.size())));
    windowing::redraw(rd->window->raw_window);
    if (scroll && root->left_mouse_down) schedule_playlist_track_drag(root);
}

static void schedule_playlist_track_drag(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->app || !rd->playlist_track_drag.active || rd->playlist_track_drag.timer_pending) return;
    rd->playlist_track_drag.timer_pending = true;
    const auto generation = rd->playlist_drag_generation;
    const auto window = rd->window->raw_window;
    std::weak_ptr<bool> lifetime = root->lifetime;
    windowing::timer(rd->app, 16, [root, lifetime, window, generation](void *) {
        if (lifetime.expired() || !windowing::has_window(window)) return;
        auto rd = static_cast<RootData *>(root->user_data);
        if (generation != rd->playlist_drag_generation) return;
        rd->playlist_track_drag.timer_pending = false;
        if (rd->playlist_track_drag.active && root->left_mouse_down)
            update_playlist_track_drag(root);
    }, nullptr);
}

static Bounds playlist_title_bounds(const Bounds &panel, double dpi, std::size_t count) {
    return Bounds(panel.x + 56 * dpi, panel.y + 16 * dpi,
                  album_track_layout(panel.w, dpi, count).text_width, 24 * dpi);
}

static double playlist_name_width(RootData *rd, const std::string &text) {
    return draw_text(rd->window->raw_window->drawing_context, 0, 0, text, 16 * rd->dpi,
                     false, mylar_font, -1, -1, theme().text_primary, true).w;
}

static void paint_playlist_name_edit(Container *root, const Bounds &bounds, RGBA foreground) {
    auto rd = static_cast<RootData *>(root->user_data);
    auto cr = rd->window->raw_window->drawing_context;
    auto &edit = rd->playlist_edit;
    const double d = rd->dpi;
    const double caret = playlist_name_width(rd, edit.text.substr(0, edit.caret));
    const double width = std::max(0.0, bounds.w - 8 * d);
    edit.scroll = std::max(0.0, std::clamp(edit.scroll, caret - width, caret));
    cr->save();
    auto field = bounds;
    field.grow(4 * d);
    cr->set_color(theme_colors::edit_background);
    rounded_rectangle(cr, field, 4 * d); cr->fill();
    cr->set_color(foreground);
    cr->rectangle(field.x, field.bottom() - d, field.w, d); cr->fill();
    set_rect(cr, bounds); cr->clip();
    const double x = bounds.x + 2 * d - edit.scroll;
    if (edit.caret != edit.anchor) {
        const auto begin = std::min(edit.caret, edit.anchor), end = std::max(edit.caret, edit.anchor);
        const double left = playlist_name_width(rd, edit.text.substr(0, begin));
        const double right = playlist_name_width(rd, edit.text.substr(0, end));
        cr->set_color(theme_colors::text_selection);
        cr->rectangle(x + left, bounds.y, right - left, bounds.h); cr->fill();
    }
    draw_text(cr, x, bounds.y, edit.text, 16 * d, true, mylar_font, -1, -1, foreground, true);
    cr->set_color(foreground);
    cr->rectangle(x + caret, bounds.y + 2 * d, std::max(1.0, d), bounds.h - 4 * d); cr->fill();
    cr->restore();
}

static void click_playlist_name(Container *root, AlbumData *album, const Bounds &bounds) {
    auto rd = static_cast<RootData *>(root->user_data);
    auto &edit = rd->playlist_edit;
    if (edit.id != album->playlist_id) {
        finish_playlist_name_edit(root, true);
        edit.id = album->playlist_id;
        edit.text = album->name;
        edit.caret = edit.text.size();
        edit.anchor = 0;
    } else {
        const double x = root->mouse_current_x - bounds.x - 2 * rd->dpi + edit.scroll;
        std::size_t pos = 0;
        while (pos < edit.text.size()) {
            const auto next = edit.next(pos);
            const double left = playlist_name_width(rd, edit.text.substr(0, pos));
            const double right = playlist_name_width(rd, edit.text.substr(0, next));
            if (x < (left + right) / 2) break;
            pos = next;
        }
        edit.caret = edit.anchor = pos;
    }
    rd->last_album_clicked = rd->album_double_click_target = nullptr;
    windowing::redraw(rd->window->raw_window);
}

struct AlbumColorLess {
    bool operator()(const RGBA &a, const RGBA &b) const {
        return std::tie(a.r, a.g, a.b, a.a) < std::tie(b.r, b.g, b.b, b.a);
    }
};

static std::map<RGBA, float, AlbumColorLess> mainColorsInImage(const drawing::Image &image) {
    std::map<RGBA, float, AlbumColorLess> palette;
    const int width = image.width, height = image.height;
    if (width <= 0 || height <= 0 || image.argb.empty())
        return palette;
    constexpr int dimension = 10, flexibility = 2;
    constexpr double range = 60.0 / 255.0;
    std::map<RGBA, int, AlbumColorLess> counter;
    for (int y = 0; y < dimension; ++y) {
        const auto row = image.argb.data() + (y * height / dimension) * width;
        for (int x = 0; x < dimension; ++x) {
            const auto pixel = row[x * width / dimension];
            const int red = (pixel >> 16) & 255, green = (pixel >> 8) & 255, blue = pixel & 255;
            // Count the same 5 × 5 × 5 flexible colors without storing the expanded vector.
            for (int r = -flexibility; r <= flexibility; ++r)
                for (int g = -flexibility; g <= flexibility; ++g)
                    for (int b = -flexibility; b <= flexibility; ++b)
                        ++counter[RGBA(std::clamp(red + r, 0, 255) / 255.0,
                                       std::clamp(green + g, 0, 255) / 255.0,
                                       std::clamp(blue + b, 0, 255) / 255.0, 1)];
        }
    }
    std::vector<std::pair<RGBA, int>> ordered(counter.begin(), counter.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    std::vector<std::pair<RGBA, int>> ranges;
    float total = 0;
    for (const auto &[color, count] : ordered) {
        const bool exclude = std::any_of(ranges.begin(), ranges.end(), [&](const auto &existing) {
            return std::abs(color.r - existing.first.r) <= range &&
                   std::abs(color.g - existing.first.g) <= range &&
                   std::abs(color.b - existing.first.b) <= range;
        });
        if (!exclude) {
            ranges.emplace_back(color, count);
            total += count;
        }
    }
    for (const auto &[color, count] : ranges)
        palette[color] = count / total;
    return palette;
}

static void update_album_colors(AlbumData *album, const std::shared_ptr<const AlbumTexture> &art) {
    if (!art || album->palette_source.lock() == art)
        return;
    album->palette_source = art;
    const auto palette = mainColorsInImage(*art);
    if (palette.empty())
        return;
    std::vector<std::pair<RGBA, float>> colors(palette.begin(), palette.end());
    std::stable_sort(colors.begin(), colors.end(), [](const auto &a, const auto &b) {
        return a.second > b.second;
    });
    album->background_color = album->secondary_color = album->accent_color = colors.front().first;
    colors.erase(colors.begin());
    const auto distance = [&](const RGBA &color) {
        const auto &background = album->background_color;
        return std::pow(color.r - background.r, 2) + std::pow(color.g - background.g, 2) +
               std::pow(color.b - background.b, 2);
    };
    std::stable_sort(colors.begin(), colors.end(), [&](const auto &a, const auto &b) {
        return distance(a.first) > distance(b.first);
    });
    if (!colors.empty()) {
        album->accent_color = colors[0].first;
        album->secondary_color = colors.size() > 1 ? colors[1].first : colors[0].first;
    }
}

static void open_artwork_preview(Container *root, const AlbumArtCache::Handle &art, const std::string &playlist_id = {});

static bool consume_album_double_click(Container *root);

static void retain_closing_album(RootData *rd) {
    if (!rd->expanded_album)
        return;
    rd->closing_albums.push_back({rd->expanded_album, rd->album_visible_height / rd->dpi,
        rd->album_visible_gap / rd->dpi, std::chrono::steady_clock::now()});
}

static void close_album(Container *root) {
    finish_playlist_name_edit(root, true);
    auto rd = static_cast<RootData *>(root->user_data);
    cancel_playlist_track_drag(rd);
    // Keep the event target alive until the current event dispatch completes.
    retain_closing_album(rd);
    rd->expanded_album = nullptr;
    rd->outgoing_album = nullptr;
    rd->album_scroll_start.reset();
    rd->album_reveal_start.reset();
    rd->album_panel->exists = false;
    layout(root, root, root->real_bounds);
    windowing::redraw(rd->window->raw_window);
}

static Bounds album_action_bounds(const Bounds &b, double dpi, int action) {
    return Bounds(b.x + (56 + action * 76) * dpi, b.y + 61 * dpi, 70 * dpi, 26 * dpi);
}

static Bounds playlist_art_button_bounds(const Bounds &artwork, double dpi) {
    const double width = std::min(120 * dpi, std::max(0.0, artwork.w - 16 * dpi));
    const double height = std::min(32 * dpi, std::max(0.0, artwork.h - 16 * dpi));
    return Bounds(artwork.x + (artwork.w - width) / 2, artwork.bottom() - height - 8 * dpi, width, height);
}

static void open_album(Container *root, Container *card, bool animate = true) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (rd->expanded_album == card)
        return;
    cancel_playlist_track_drag(rd);
    if (animate) finish_playlist_name_edit(root, true);
    if (!rd->album_panel) {
        rd->album_panel = rd->library->child(FILL_SPACE, FILL_SPACE);
        rd->album_panel->handles_pierced = [](Container *c, int x, int y) {
            const auto rd = root_data_for(c);
            const auto b = c->real_bounds;
            return rd->expanded_album && bounds_contains(Bounds(b.x, b.y, b.w, std::min(b.h, rd->album_visible_height)), x, y);
        };
        rd->album_panel->when_mouse_down = [](Container *root, Container *c) {
            auto rd = static_cast<RootData *>(root->user_data);
            cancel_playlist_track_drag(rd);
            c->when_drag_end_is_click = true;
            c->minimum_x_distance_to_move_before_drag_begins = 6 * rd->dpi;
            c->minimum_y_distance_to_move_before_drag_begins = 6 * rd->dpi;
            if (c->state.mouse_button_pressed != BTN_LEFT || !rd->expanded_album) return;
            const auto album = static_cast<AlbumData *>(rd->expanded_album->user_data);
            if (album->playlist_id.empty()) return;
            const auto tracks = album_track_layout(c->real_bounds.w, rd->dpi, album->album.songs.size(), true);
            for (std::size_t i = 0; i < album->album.songs.size(); ++i) {
                if (!bounds_contains(tracks.hit_bounds(c->real_bounds, rd->dpi, i), root->mouse_current_x, root->mouse_current_y))
                    continue;
                c->when_drag_end_is_click = false;
                if (bounds_contains(tracks.remove_bounds(c->real_bounds, rd->dpi, i), root->mouse_current_x, root->mouse_current_y)) {
                    rd->playlist_remove_press = PlaylistTrackTarget{album->playlist_id, album->album.songs[i].full};
                    break;
                }
                rd->playlist_track_drag.playlist_id = album->playlist_id;
                rd->playlist_track_drag.path = album->album.songs[i].full;
                rd->playlist_track_drag.grab_y = (root->mouse_current_y - tracks.track_bounds(c->real_bounds, rd->dpi, i).y) / rd->dpi;
                c->when_drag_end_is_click = false;
                break;
            }
        };
        rd->album_panel->when_drag_start = rd->album_panel->when_drag = [](Container *root, Container *c) {
            auto rd = static_cast<RootData *>(root->user_data);
            if (rd->playlist_track_drag.playlist_id.empty() || c->state.mouse_button_pressed != BTN_LEFT) return;
            if (!rd->playlist_track_drag.active) {
                rd->playlist_track_drag.active = true;
                rd->playlist_track_drag.last_update = std::chrono::steady_clock::now();
                rd->last_album_clicked = rd->album_double_click_target = nullptr;
                rd->album_scroll_start.reset();
                rd->album_reveal_start.reset();
                rd->album_reveal = 1;
                rd->outgoing_album = nullptr;
                layout(root, root, root->real_bounds);
            }
            update_playlist_track_drag(root);
        };
        rd->album_panel->when_drag_end = [](Container *root, Container *) {
            finish_playlist_track_drag(root);
        };
        rd->album_panel->when_paint = [](Container *root, Container *c) {
            auto rd = static_cast<RootData *>(root->user_data);
            auto cr = rd->window->raw_window->drawing_context;
            auto draw_panel = [&](Container *card, Bounds b, double visible_height, double visible_gap) {
                auto album = static_cast<AlbumData *>(card->user_data);
                const double dpi = rd->dpi;
                // These rows are painted directly rather than child widgets.
                // Respect the panel's event hover state and modal overlays before
                // using pointer coordinates to highlight individual controls.
                const bool hover_enabled = c->state.mouse_hovering && rd->library->interactable &&
                    card == rd->expanded_album && !((rd->queue_overlay && rd->queue_overlay->exists) || (rd->context_overlay && rd->context_overlay->exists));

                const auto tracks = album_track_layout(b.w, dpi, album->album.songs.size(), !album->playlist_id.empty());
                const double art_size = tracks.art_size;
                const double text_width = tracks.text_width;
                update_album_colors(album, rd->artwork->image(album->art));
                const auto background = album->background_color;
                const bool light = .2126 * background.r + .7152 * background.g + .0722 * background.b > .45;
                const auto foreground = light ? theme_colors::art_text_dark : theme_colors::art_text_light;
                const auto secondary = album->secondary_color;
                cr->save();
                set_rect(cr, c->parent->real_bounds);
                cr->clip();
                // Fixed-size casters keep the shadow cached as the panel animates.
                // Clip each one to the outside of its visible edge.
                const double shadow_alpha = std::clamp(visible_height / (16 * dpi), 0.0, 1.0);
                const double panel_bottom = b.y + std::min(b.h, visible_height);
                const auto viewport = c->parent->real_bounds;
                auto draw_edge_shadow = [&](double edge, bool above) {
                    // Inset the caster so only the softer blur reaches the panel edge.
                    const double inset = 8 * dpi;
                    cr->save();
                    set_rect(cr, above
                        ? Bounds(viewport.x, viewport.y, viewport.w, std::max(0.0, edge - viewport.y))
                        : Bounds(viewport.x, edge, viewport.w, std::max(0.0, viewport.bottom() - edge)));
                    cr->clip();
                    rd->album_panel_shadow.draw(*cr,
                        {b.x, above ? edge + inset : edge - inset - 96 * dpi, b.w, 96 * dpi},
                        0, {.14, 32, 0}, dpi, shadow_alpha);
                    cr->restore();
                };
                draw_edge_shadow(b.y, true);
                draw_edge_shadow(panel_bottom, false);
                // Reveal the final layout without moving or scaling its contents.
                set_rect(cr, Bounds(b.x, b.y - 12 * dpi, b.w, 12 * dpi + visible_height));
                cr->clip();
                set_rect(cr, b);
                set_argb(cr, background);
                cr->fill();
                const double pointer_x = card->real_bounds.x + card->real_bounds.w / 2;
                const double pointer_size = 12 * dpi * std::clamp(visible_gap / (16 * dpi), 0.0, 1.0);
                cr->move_to(pointer_x - pointer_size, b.y);
                cr->line_to(pointer_x, b.y - pointer_size);
                cr->line_to(pointer_x + pointer_size, b.y);
                cr->close_path();
                set_argb(cr, background);
                cr->fill();
                draw_text(cr, b.x + 16 * dpi, b.y + 16 * dpi, "×", 22 * dpi, true,
                          mylar_font, 28 * dpi, -1, foreground, false);
                const auto title = playlist_title_bounds(b, dpi, album->album.songs.size());
                if (!album->playlist_id.empty() && rd->playlist_edit.id == album->playlist_id && card == rd->expanded_album) {
                    paint_playlist_name_edit(root, title, foreground);
                } else {
                    draw_text(cr, title.x, title.y, album->name, 16 * dpi, true, mylar_font,
                              text_width, 24 * dpi, foreground, true);
                    if (!album->playlist_id.empty() && hover_enabled && bounds_contains(title, root->mouse_current_x, root->mouse_current_y)) {
                        cr->set_color(foreground);
                        cr->rectangle(title.x, title.bottom(), std::min(title.w, playlist_name_width(rd, album->name)), dpi); cr->fill();
                    }
                }
                std::string artist_year = album->artist;
                const auto year = album->playlist_id.empty() && !album->album.songs.empty() ? album->album.songs.front().year : std::string{};
                if (!year.empty() && year != "0")
                    artist_year += " (" + year + ")";
                draw_text(cr, b.x + 56 * dpi, b.y + 41 * dpi, artist_year,
                          12 * dpi, true, mylar_font, text_width, 20 * dpi,
                          secondary, false);
                const char *actions[] = {"▶ Play", "Shuffle", "+ Queue"};
                for (int i = 0; i < 3; ++i) {
                    const auto button = album_action_bounds(b, dpi, i);
                    if (button.x + button.w > b.x + 56 * dpi + text_width)
                        continue;
                    if (hover_enabled && bounds_contains(button, root->mouse_current_x, root->mouse_current_y)) {
                        rounded_rectangle(cr, button, 6 * dpi);
                        cr->set_color(theme_colors::row_hover);
                        cr->fill();
                    }
                    draw_text(cr, button.x, button.y + 4 * dpi, actions[i], 11 * dpi, true,
                              mylar_font, button.w, 20 * dpi, foreground, true, 1);
                }
                const auto playing_path = player->current_path();
                const auto &drag = rd->playlist_track_drag;
                const bool dragging = drag.active && drag.playlist_id == album->playlist_id && card == rd->expanded_album;
                for (std::size_t i = 0; i < album->album.songs.size(); ++i) {
                    const auto &song = album->album.songs[i];
                    const auto row = tracks.track_bounds(b, dpi, i);
                    const auto duration_bounds = tracks.duration_bounds(b, dpi, i);
                    const auto remove_bounds = tracks.remove_bounds(b, dpi, i);
                    if (row.intersection(c->parent->real_bounds).empty())
                        continue;
                    const bool hovered = !dragging && hover_enabled &&
                        bounds_contains(tracks.hit_bounds(b, dpi, i), root->mouse_current_x, root->mouse_current_y);
                    const bool dragged = dragging && song.full == drag.path;
                    if (hovered) {
                        rounded_rectangle(cr, row, 6 * dpi);
                        cr->set_color(theme_colors::row_track);
                        cr->fill();
                    }
                    const bool playing = song.full == playing_path;
                    const bool bold = playing || hovered;
                    const auto title = song.name.empty() ? std::filesystem::path(song.full).stem().string() : song.name;
                    const auto number = playing ? "♫" : (album->playlist_id.empty() && !song.track.empty() && song.track != "0"
                        ? song.track : std::to_string(i + 1));
                    int duration = 0;
                    const auto [end, error] = std::from_chars(song.length.data(), song.length.data() + song.length.size(), duration);
                    const auto time = error == std::errc{} && end == song.length.data() + song.length.size() && duration >= 0
                        ? seconds_to_mmss(duration) : std::string{};
                    auto row_foreground = foreground, row_secondary = secondary;
                    if (dragged) row_foreground.a = row_secondary.a = .35;
                    if (!album->playlist_id.empty()) {
                        cr->set_color(RGBA(secondary.r, secondary.g, secondary.b, hovered || dragged ? .9 : .45));
                        for (int dot = 0; dot < 6; ++dot)
                            cr->rectangle(row.x - (16 - (dot % 2) * 5) * dpi, row.y + (9 + (dot / 2) * 5) * dpi, 2 * dpi, 2 * dpi);
                        cr->fill();
                    }
                    cr->save();
                    set_rect(cr, row);
                    cr->clip();
                    draw_text(cr, row.x + 12 * dpi, row.y + 7 * dpi, number, 11 * dpi, true, mylar_font,
                              28 * dpi, 22 * dpi, row_secondary, bold, 0);
                    draw_text(cr, row.x + 48 * dpi, row.y + 7 * dpi, title, 12 * dpi, true,
                              mylar_font, std::max(1.0, duration_bounds.x - row.x - 56 * dpi), 22 * dpi,
                              row_foreground, bold);
                    draw_text(cr, duration_bounds.x, duration_bounds.y, time, 11 * dpi, true,
                              mylar_font, duration_bounds.w, duration_bounds.h, row_secondary, bold, 2);
                    if (!album->playlist_id.empty() && !remove_bounds.empty()) {
                        const bool remove_hovered = hovered && bounds_contains(remove_bounds, root->mouse_current_x, root->mouse_current_y);
                        if (remove_hovered) {
                            cr->set_color(theme_colors::remove_hover);
                            rounded_rectangle(cr, remove_bounds, 4 * dpi); cr->fill();
                        }
                        cr->set_color(remove_hovered ? theme_colors::remove_icon : row_secondary);
                        cr->set_line_width(1.5 * dpi);
                        const double x = remove_bounds.x + remove_bounds.w / 2;
                        const double y = remove_bounds.y + remove_bounds.h / 2;
                        const double radius = std::min({4 * dpi, remove_bounds.w / 4, remove_bounds.h / 4});
                        cr->move_to(x - radius, y - radius); cr->line_to(x + radius, y + radius);
                        cr->move_to(x + radius, y - radius); cr->line_to(x - radius, y + radius);
                        cr->stroke();
                    }
                    cr->restore();
                }
                if (!album->playlist_id.empty() && album->album.songs.empty()) {
                    const auto row = tracks.track_bounds(b, dpi, 0);
                    draw_text(cr, row.x, row.y + 7 * dpi, "This playlist is empty", 12 * dpi, true,
                              mylar_font, row.w, 24 * dpi, secondary, false);
                }
                if (dragging && drag.can_drop) {
                    const auto first = tracks.track_bounds(b, dpi, 0);
                    const auto source = std::find_if(album->album.songs.begin(), album->album.songs.end(),
                                                     [&](const auto &song) { return song.full == drag.path; });
                    if (source != album->album.songs.end()) {
                        auto floating = first;
                        floating.y = std::clamp(root->mouse_current_y - drag.grab_y * dpi,
                                               viewport.y, std::max(viewport.y, viewport.bottom() - floating.h));
                        cr->set_color(RGBA(background.r * .85 + foreground.r * .15,
                                           background.g * .85 + foreground.g * .15,
                                           background.b * .85 + foreground.b * .15, .97));
                        rounded_rectangle(cr, floating, 4 * dpi); cr->fill();
                        const auto title = source->name.empty() ? std::filesystem::path(source->full).stem().string() : source->name;
                        draw_text(cr, floating.x + 8 * dpi, floating.y + 7 * dpi, "↕", 12 * dpi, true,
                                  mylar_font, 24 * dpi, 22 * dpi, foreground, false);
                        draw_text(cr, floating.x + 40 * dpi, floating.y + 7 * dpi, title, 12 * dpi, true,
                                  mylar_font, std::max(1.0, floating.w - 48 * dpi), 22 * dpi, foreground, true);
                    }
                    const double y = first.y + drag.slot * 32 * dpi;
                    cr->set_color(theme_colors::playing_indicator);
                    cr->rectangle(first.x - 18 * dpi, y - dpi, first.w + 18 * dpi, 2 * dpi); cr->fill();
                    cr->arc(first.x - 18 * dpi, y, 3 * dpi, 0, 2 * M_PI); cr->fill();
                }
                const auto art = rd->artwork->image(album->art);
                const Bounds artwork(b.x + b.w - art_size - 16 * dpi, b.y + 16 * dpi, art_size, art_size);
                if (art && art_size > 0)
                    paint_artwork(rd, cr, album->art, art, {}, artwork.x, artwork.y, art_size, false);
                if (!album->playlist_id.empty() && card == rd->expanded_album && !dragging && art_size > 0) {
                    const bool choosing = rd->playlist_art_chooser.visible() && rd->playlist_art_chooser_id == album->playlist_id;
                    const bool saving = rd->playlist_art_job.valid() && rd->playlist_art_job_id == album->playlist_id;
                    if ((hover_enabled && bounds_contains(artwork, root->mouse_current_x, root->mouse_current_y)) || choosing || saving) {
                        const auto button = playlist_art_button_bounds(artwork, dpi);
                        const bool over = hover_enabled && bounds_contains(button, root->mouse_current_x, root->mouse_current_y);
                        cr->set_color(with_alpha(theme_colors::black, over ? .85 : .7));
                        rounded_rectangle(cr, button, 6 * dpi); cr->fill();
                        const auto label = choosing ? "Choosing…" : saving ? (rd->playlist_art_removing ? "Removing…" : "Saving…")
                            : album->art_file.empty() ? "Choose art" : "Remove art";
                        draw_text(cr, button.x, button.y + 8 * dpi, label, 11 * dpi, true,
                                  mylar_font, button.w, 20 * dpi, theme().on_accent, true, 1);
                    }
                    if (rd->playlist_art_error_id == album->playlist_id && !rd->playlist_art_error.empty()) {
                        Bounds message(artwork.x + 8 * dpi, artwork.y + 8 * dpi,
                                       std::max(0.0, artwork.w - 16 * dpi), std::min(72 * dpi, std::max(0.0, artwork.h - 56 * dpi)));
                        cr->set_color(theme_colors::art_error);
                        rounded_rectangle(cr, message, 6 * dpi); cr->fill();
                        draw_text(cr, message.x + 8 * dpi, message.y + 6 * dpi, rd->playlist_art_error, 10 * dpi, true,
                                  mylar_font, std::max(0.0, message.w - 16 * dpi), std::max(0.0, message.h - 12 * dpi), theme().on_accent, false);
                    }
                }
                cr->restore();
            };
            for (const auto &closing : rd->closing_albums)
                draw_panel(closing.card, closing.bounds, closing.visible_height,
                           closing.occupied_height - closing.visible_height);
            if (!rd->expanded_album)
                return;
            if (rd->outgoing_album) {
                // Add weighted complete layers so overlapping opaque backgrounds crossfade evenly.
                cr->push_group();
                auto layer = [&](Container *card, double height, double opacity) {
                    cr->push_group();
                    auto bounds = c->real_bounds;
                    bounds.h = height;
                    draw_panel(card, bounds, rd->album_visible_height, rd->album_visible_gap);
                    cr->pop_group_to_source();
                    cr->set_operator(drawing::Composite::Add);
                    cr->paint_source(opacity);
                    cr->set_operator(drawing::Composite::Over);
                };
                layer(rd->outgoing_album, rd->album_transition_from_height * rd->dpi, 1 - rd->album_reveal);
                layer(rd->expanded_album, c->real_bounds.h, rd->album_reveal);
                cr->pop_group_to_source();
                cr->paint_source();
            } else {
                draw_panel(rd->expanded_album, c->real_bounds, rd->album_visible_height, rd->album_visible_gap);
            }
        };
        rd->album_panel->when_clicked = [](Container *root, Container *c) {
            auto data = static_cast<RootData *>(root->user_data);
            const auto remove_press = std::exchange(data->playlist_remove_press, std::nullopt);
            cancel_playlist_track_drag(data);
            if (remove_press && c->state.mouse_button_pressed == BTN_LEFT) {
                data->last_album_clicked = data->album_double_click_target = nullptr;
                if (data->expanded_album) {
                    const auto album = static_cast<AlbumData *>(data->expanded_album->user_data);
                    const auto tracks = album_track_layout(c->real_bounds.w, data->dpi, album->album.songs.size(), true);
                    const auto path = preferred_audio_path(remove_press->path);
                    const auto visible = Bounds(c->real_bounds.x, c->real_bounds.y, c->real_bounds.w,
                                                std::min(c->real_bounds.h, data->album_visible_height)).intersection(data->library->real_bounds);
                    for (std::size_t i = 0; i < album->album.songs.size(); ++i) {
                        if (album->playlist_id == remove_press->playlist_id &&
                            (album->album.songs[i].full == remove_press->path || album->album.songs[i].full == path) &&
                            bounds_contains(tracks.remove_bounds(c->real_bounds, data->dpi, i).intersection(visible),
                                            root->mouse_current_x, root->mouse_current_y)) {
                            remove_playlist_track(root, remove_press->playlist_id, remove_press->path);
                            break;
                        }
                    }
                }
                return;
            }
            if (consume_album_double_click(root))
                return;
            if (c->state.mouse_button_pressed == BTN_RIGHT) {
                auto rd = static_cast<RootData *>(root->user_data);
                if (!rd->expanded_album) return;
                auto album = static_cast<AlbumData *>(rd->expanded_album->user_data);
                const auto tracks = album_track_layout(c->real_bounds.w, rd->dpi, album->album.songs.size(), !album->playlist_id.empty());
                for (std::size_t i = 0; i < album->album.songs.size(); ++i)
                    if (bounds_contains(tracks.hit_bounds(c->real_bounds, rd->dpi, i), root->mouse_current_x, root->mouse_current_y)) {
                        open_queue_context(root, {album->album.songs[i].full}, album->playlist_id);
                        return;
                    }
                open_queue_context(root, album_paths(album->album), album->playlist_id, true);
                return;
            }
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            auto rd = static_cast<RootData *>(root->user_data);
            auto album = static_cast<AlbumData *>(rd->expanded_album->user_data);
            const auto b = c->real_bounds;
            if (root->mouse_current_y >= b.y + std::min(b.h, rd->album_visible_height))
                return;
            const auto tracks = album_track_layout(b.w, rd->dpi, album->album.songs.size(), !album->playlist_id.empty());
            const auto title = playlist_title_bounds(b, rd->dpi, album->album.songs.size());
            if (!album->playlist_id.empty() && bounds_contains(title, root->mouse_current_x, root->mouse_current_y)) {
                click_playlist_name(root, album, title);
                return;
            }
            if (bounds_contains(Bounds(b.x + 8 * rd->dpi, b.y + 8 * rd->dpi, 36 * rd->dpi, 36 * rd->dpi),
                                root->mouse_current_x, root->mouse_current_y)) {
                close_album(root);
                return;
            }
            const Bounds artwork(b.x + b.w - tracks.art_size - 16 * rd->dpi,
                                 b.y + 16 * rd->dpi, tracks.art_size, tracks.art_size);
            if (bounds_contains(artwork, root->mouse_current_x, root->mouse_current_y)) {
                if (!album->playlist_id.empty() && bounds_contains(playlist_art_button_bounds(artwork, rd->dpi),
                                                                  root->mouse_current_x, root->mouse_current_y)) {
                    playlist_art_action(root, album->playlist_id);
                    return;
                }
                open_artwork_preview(root, album->art, album->playlist_id);
                return;
            }
            for (int action = 0; action < 3; ++action) {
                const auto button = album_action_bounds(b, rd->dpi, action);
                if (button.x + button.w > b.x + 56 * rd->dpi + tracks.text_width ||
                    !bounds_contains(button, root->mouse_current_x, root->mouse_current_y))
                    continue;
                if (album->album.songs.empty()) return;
                if (action == 0) {
                    play_album(*album, 0);
                } else if (action == 1) {
                    auto queue = album_paths(album->album);
                    static std::mt19937 random(std::random_device{}());
                    std::shuffle(queue.begin(), queue.end(), random);
                    observe_queue();
                    const auto index = playback_queue.upcoming_begin();
                    playback_queue.add(queue, PlaybackQueue::Action::PlayNext, album->playlist_id);
                    commit_queue(root);
                    if (!player->play_queued_item(index))
                        std::cerr << "Album playback failed: " << player->last_error() << '\n';
                } else {
                    observe_queue();
                    playback_queue.add(album_paths(album->album), PlaybackQueue::Action::Append, album->playlist_id);
                    commit_queue(root);
                }
                return;
            }

            for (std::size_t i = 0; i < album->album.songs.size(); ++i) {
                if (bounds_contains(tracks.hit_bounds(b, rd->dpi, i),
                                    root->mouse_current_x, root->mouse_current_y)) {
                    if (!album->playlist_id.empty() && bounds_contains(tracks.remove_bounds(b, rd->dpi, i),
                                                                      root->mouse_current_x, root->mouse_current_y))
                        return;
                    play_album(*album, i);
                    break;
                }
            }
        };
    }
    if (!animate) {
        rd->expanded_album = card;
        rd->album_reveal = 1;
        rd->album_reveal_start.reset();
        rd->album_scroll_start.reset();
        rd->outgoing_album = nullptr;
        return;
    }
    if (rd->expanded_album != card) {
        const auto &cards = rd->library->children;
        const auto index = std::distance(cards.begin(), std::find(cards.begin(), cards.end(), card));
        const auto previous = std::distance(cards.begin(), std::find(cards.begin(), cards.end(), rd->expanded_album));
        const bool same_row = rd->expanded_album && index / rd->album_columns == previous / rd->album_columns;
        if (!same_row)
            retain_closing_album(rd);
        rd->outgoing_album = same_row ? rd->expanded_album : nullptr;
        rd->album_transition_from_height = same_row ? rd->album_visible_height / rd->dpi : 0;
        rd->album_transition_from_gap = same_row ? rd->album_visible_gap / rd->dpi : 0;
        // If we return to a row mid-close, transition from its current visible height.
        const auto closing = std::find_if(rd->closing_albums.begin(), rd->closing_albums.end(), [&](const auto &entry) {
            const auto closing_index = std::distance(cards.begin(), std::find(cards.begin(), cards.end(), entry.card));
            return closing_index / rd->album_columns == index / rd->album_columns;
        });
        if (!same_row && closing != rd->closing_albums.end()) {
            rd->outgoing_album = closing->card;
            rd->album_transition_from_height = closing->visible_height / rd->dpi;
            rd->album_transition_from_gap = (closing->occupied_height - closing->visible_height) / rd->dpi;
            rd->closing_albums.erase(closing);
        }
        rd->album_reveal_start = std::chrono::steady_clock::now();
        rd->album_reveal = 0;
    }
    rd->expanded_album = card;
    // Interpolate the selected card's screen position, compensating for changing panels above it.
    rd->album_scroll_from = (card->real_bounds.y - rd->library->real_bounds.y) / rd->dpi;
    rd->album_scroll_start = std::chrono::steady_clock::now();
    layout(root, root, root->real_bounds);
    windowing::redraw(rd->window->raw_window);
}

// Restore before layout so scroll clamping includes the panel's full height.
static void restore_expanded_album(Container *root, const std::string &track, const std::string &playlist_id = {}) {
    if (track.empty() && playlist_id.empty()) return;
    auto data = static_cast<RootData *>(root->user_data);
    for (auto card : data->library->children) {
        if (card == data->album_panel) continue;
        const auto album = static_cast<AlbumData *>(card->user_data);
        const auto &songs = album->album.songs;
        if ((!playlist_id.empty() && album->playlist_id == playlist_id) ||
            (playlist_id.empty() && album->playlist_id.empty() &&
             std::any_of(songs.begin(), songs.end(), [&](const auto &song) { return song.full == track; }))) {
            open_album(root, card, false);
            return;
        }
    }
}

static std::string expanded_album_track(RootData *data) {
    if (!data->expanded_album) return {};
    const auto album = static_cast<AlbumData *>(data->expanded_album->user_data);
    if (!album->playlist_id.empty()) return {};
    const auto &songs = album->album.songs;
    return songs.empty() ? std::string() : songs.front().full;
}

static std::string expanded_playlist_id(RootData *data) {
    return data->expanded_album ? static_cast<AlbumData *>(data->expanded_album->user_data)->playlist_id : std::string{};
}

static bool consume_album_double_click(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->album_double_click_target)
        return false;
    if (!rd->album_double_click_handled) {
        rd->album_double_click_handled = true;
        auto card = rd->album_double_click_target;
        auto album = static_cast<AlbumData *>(card->user_data);
        album->play_pulse_start = std::chrono::steady_clock::now();
        play_album(*album, 0);
        open_album(root, card);
        windowing::redraw(rd->window->raw_window);
    }
    return true;
}

static AlbumArtCache::Handle playlist_art_handle(RootData *rd, const PlaylistState &playlist) {
    const auto file = playlist_art::owned_file(rd->startup->state_file, playlist.art_file);
    return file.empty() ? rd->artwork->create_collage(playlist.tracks) : rd->artwork->create_file(file);
}

static Container *add_album(Container *parent, const AlbumOption &option, AlbumArtCache::Handle existing_art = {},
                      std::optional<std::chrono::steady_clock::time_point> detail_fade = {}, const PlaylistState *playlist = nullptr) {
    if (option.songs.empty() && !playlist)
        return nullptr;
    auto data = new AlbumData;
    data->album = option;
    if (playlist) {
        data->playlist_id = playlist->id;
        data->art_file = playlist->art_file;
    }
    data->detail_fade = detail_fade;
    std::vector<std::string> tracks;
    tracks.reserve(option.songs.size());
    for (const auto &song : option.songs)
        tracks.push_back(song.full);
    auto root_data = root_data_for(parent);
    data->art = existing_art ? std::move(existing_art) : playlist
        ? playlist_art_handle(root_data, *playlist) : root_data->artwork->create(std::move(tracks));
    root_data->artwork_prefetch.add(data->art);
    if (playlist) {
        data->name = playlist->name;
        data->artist = "Playlist · " + std::to_string(option.songs.size()) + " tracks";
    } else {
        for (const auto &song : option.songs)
            root_data->tracks[song.full] = {
                song.name.empty() ? std::filesystem::path(song.full).stem().string() : song.name,
                song.artist, data->art, song.album, song.length};
        data->name = option.songs.front().album.empty() ? "Unknown" : option.songs.front().album;
        data->artist = option.songs.front().artist;
        for (const auto &song : option.songs) {
            if (song.artist != data->artist) {
                data->artist = "Various Artists";
                break;
            }
        }
        if (data->artist.empty())
            data->artist = "Unknown Artist";
    }

    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    c->user_data = data;
    c->exists = false;
    c->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<AlbumData *>(c->user_data);
        auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
        auto cr = window->drawing_context;
        const double dpi = window->dpi;
        if (c->real_bounds.intersection(c->parent->real_bounds).empty())
            return;
        const auto art = static_cast<RootData *>(root->user_data)->artwork->image(data->art);
        // AlbumTexture *art = nullptr;

        cr->save();
        set_rect(cr, c->parent->real_bounds);
        cr->clip();
        if (data->play_pulse_start) {
            auto rd = static_cast<RootData *>(root->user_data);
            const double t = std::clamp(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - *data->play_pulse_start).count() / 240.0, 0.0, 1.0);
            const double scale = 1 - .07 * std::pow(std::sin(M_PI * t), 2);
            const double cx = c->real_bounds.x + c->real_bounds.w / 2;
            const double cy = c->real_bounds.y + c->real_bounds.h / 2;
            cr->translate(cx, cy);
            cr->scale(scale, scale);
            cr->translate(-cx, -cy);
            if (t >= 1)
                data->play_pulse_start.reset();
            else {
                rd->artwork_refresh->animating = true;
                poll_artwork(rd->artwork_refresh);
            }
        }
        const double pad = 8 * dpi;
        const double x = c->real_bounds.x + pad;
        const double y = c->real_bounds.y + pad;
        const double size = std::max(0.0, c->real_bounds.w - 2 * pad);
        auto rd = root_data_for(root);
        rd->library_shadow.draw(*cr, {x, y, size, size}, 0, library_art_shadow, dpi, rd->library_shadow_alpha);
        cr->rectangle(x, y, size, size);
        cr->set_color(theme().placeholder);
        cr->fill();
        if (art && size > 0) {
            paint_artwork(static_cast<RootData *>(root->user_data), cr, data->art, art,
                          data->detail_fade, x, y, size);
        } else {
            draw_text(cr, x, y + size / 2 - 12 * dpi, "♫", 24 * dpi, true,
                      mylar_font, size, -1, theme().placeholder_text, false, 1);
        }
        if (c->state.mouse_hovering && size > 0) {
            const double cx = x + size / 2, cy = y + size / 2;
            cr->arc(cx, cy, std::min(26 * dpi, size / 3), 0, 2 * M_PI);
            cr->set_color(theme_colors::art_overlay);
            cr->fill();
            cr->move_to(cx - 6 * dpi, cy - 10 * dpi);
            cr->line_to(cx + 10 * dpi, cy);
            cr->line_to(cx - 6 * dpi, cy + 10 * dpi);
            cr->close_path();
            cr->set_color(theme().on_accent);
            cr->fill();
        }
        draw_text(cr, x, y + size + 8 * dpi, data->name, 12 * dpi, true,
                  mylar_font, size, 20 * dpi, theme().text_primary, true,
                  0, nullptr, dpi);
        draw_text(cr, x, y + size + 30 * dpi, data->artist, 10 * dpi, true,
                  mylar_font, size, 18 * dpi, theme().text_muted, false,
                  0, nullptr, dpi);
        cr->restore();
    };
    c->when_clicked = [](Container *root, Container *c) {
        if (consume_album_double_click(root))
            return;
        if (c->state.mouse_button_pressed == BTN_RIGHT) {
            const auto album = static_cast<AlbumData *>(c->user_data);
            open_queue_context(root, album_paths(album->album), album->playlist_id, true);
            return;
        }
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto rd = root_data_for(c);
        auto album = static_cast<AlbumData *>(c->user_data);
        const double dpi = rd->dpi;
        const auto now = std::chrono::steady_clock::now();
        rd->last_album_clicked = c;
        rd->last_album_click_time = now;
        rd->last_album_click_x = root->mouse_current_x;
        rd->last_album_click_y = root->mouse_current_y;
        const double size = std::max(0.0, c->real_bounds.w - 16 * dpi);
        const double dx = root->mouse_current_x - (c->real_bounds.x + c->real_bounds.w / 2);
        const double dy = root->mouse_current_y - (c->real_bounds.y + 8 * dpi + size / 2);
        if (std::hypot(dx, dy) <= std::min(26 * dpi, size / 3)) {
            album->play_pulse_start = now;
            play_album(*album, 0);
            open_album(root, c);
        } else if (rd->expanded_album == c) {
            close_album(root);
        } else {
            open_album(root, c);
        }
        windowing::redraw(rd->window->raw_window);
    };

    return c;
}

static Container *playlist_card(RootData *rd, const std::string &id) {
    for (auto card : rd->library->children) {
        if (card == rd->album_panel) continue;
        if (static_cast<AlbumData *>(card->user_data)->playlist_id == id)
            return card;
    }
    return nullptr;
}

static std::vector<std::string> unique_playlist_paths(const std::vector<std::string> &paths) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto &path : paths) {
        if (path.empty()) continue;
        auto preferred = preferred_audio_path(path);
        if (seen.insert(preferred).second) result.push_back(std::move(preferred));
    }
    return result;
}

static void sync_playlist_cards(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup) return;
    std::unordered_map<std::string, std::size_t> order;
    for (auto &playlist : rd->startup->session.playlists) {
        order[playlist.id] = order.size();
        playlist.tracks = unique_playlist_paths(playlist.tracks);
        AlbumOption option;
        for (const auto &path : playlist.tracks) {
            Option song;
            song.full = path;
            song.name = std::filesystem::path(path).stem().string();
            if (const auto found = rd->tracks.find(path); found != rd->tracks.end()) {
                song.name = found->second.title;
                song.artist = found->second.artist;
                song.album = found->second.album;
                song.length = found->second.length;
            }
            option.songs.push_back(std::move(song));
        }
        if (auto card = playlist_card(rd, playlist.id)) {
            auto album = static_cast<AlbumData *>(card->user_data);
            album->album = std::move(option);
            album->name = playlist.name;
            album->artist = "Playlist · " + std::to_string(playlist.tracks.size()) + " tracks";
            album->art_file = playlist.art_file;
            auto art = playlist_art_handle(rd, playlist);
            if (album->art != art) {
                rd->artwork->release(album->art);
                album->art = std::move(art);
                album->detail_fade.reset();
                album->palette_source.reset();
            }
        } else {
            add_album(rd->library, option, {}, {}, &playlist);
        }
    }
    auto &cards = rd->library->children;
    auto group = [rd](Container *card) {
        if (card == rd->album_panel) return 3;
        const auto album = static_cast<AlbumData *>(card->user_data);
        return !album->playlist_id.empty() ? 1 : album->name == "Unknown" ? 2 : 0;
    };
    std::stable_sort(cards.begin(), cards.end(), [&](Container *a, Container *b) {
        const int first = group(a), second = group(b);
        if (first != second) return first < second;
        if (first != 1) return false;
        return order.at(static_cast<AlbumData *>(a->user_data)->playlist_id) <
               order.at(static_cast<AlbumData *>(b->user_data)->playlist_id);
    });
    // Card indices changed; keep the panel last and rebuild visibility/prefetch.
    rd->album_first = rd->album_end = 0;
    rd->artwork_prefetch.reset();
    for (auto card : cards) {
        if (card == rd->album_panel) continue;
        card->exists = false;
        rd->artwork_prefetch.add(static_cast<AlbumData *>(card->user_data)->art);
    }
}

static void discard_playlist_art(RootData *rd, const std::string &file) {
    if (!rd->startup || file.empty()) return;
    rd->artwork->forget_file(playlist_art::owned_file(rd->startup->state_file, file));
    rd->playlist_art_writer.enqueue([session = rd->startup->state_file, file] {
        const auto error = playlist_art::remove_image(session, file);
        if (!error.empty()) std::cerr << error << '\n';
    });
}

static void finish_playlist_art_job(Container *root, bool closing = false) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->playlist_art_job.valid() || (!closing &&
        rd->playlist_art_job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
    const auto id = std::exchange(rd->playlist_art_job_id, {});
    const auto previous = std::exchange(rd->playlist_art_previous_file, {});
    const bool removing = std::exchange(rd->playlist_art_removing, false);
    playlist_art::ImportResult result;
    try { result = rd->playlist_art_job.get(); }
    catch (const std::exception &error) { result.error = error.what(); }
    if (!result.error.empty()) {
        rd->playlist_art_error_id = id;
        rd->playlist_art_error = result.error;
        std::cerr << "Playlist artwork: " << result.error << '\n';
        if (!closing) windowing::redraw(rd->window->raw_window);
        return;
    }
    auto &playlists = rd->startup->session.playlists;
    auto playlist = std::find_if(playlists.begin(), playlists.end(), [&](const auto &entry) { return entry.id == id; });
    if (playlist == playlists.end() || playlist->art_file != previous) {
        discard_playlist_art(rd, result.file);
        return;
    }
    playlist->art_file = result.file;
    if (removing)
        rd->artwork->forget_file(playlist_art::owned_file(rd->startup->state_file, previous));
    else if (previous != result.file)
        discard_playlist_art(rd, previous);
    if (rd->playlist_art_error_id == id) {
        rd->playlist_art_error_id.clear();
        rd->playlist_art_error.clear();
    }
    if (closing) return; // The final session checkpoint saves the completed job.
    sync_playlist_cards(root);
    if (rd->artwork_preview->exists && rd->preview_playlist_id == id) {
        if (auto card = playlist_card(rd, id))
            rd->preview_art = rd->artwork->create_preview(static_cast<AlbumData *>(card->user_data)->art);
    }
    layout(root, root, root->real_bounds);
    checkpoint_session(root, true);
    windowing::redraw(rd->window->raw_window);
}

static void poll_playlist_art_chooser(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    rd->playlist_art_chooser.poll();
    if (!rd->playlist_art_chooser.visible() || rd->playlist_art_chooser_poll_pending) return;
    rd->playlist_art_chooser_poll_pending = true;
    const auto window = rd->window->raw_window;
    std::weak_ptr<bool> lifetime = root->lifetime;
    windowing::timer(rd->app, 16, [root, lifetime, window](void *) {
        if (!lifetime.expired() && windowing::has_window(window)) {
            static_cast<RootData *>(root->user_data)->playlist_art_chooser_poll_pending = false;
            poll_playlist_art_chooser(root);
        }
    }, nullptr);
}

static void playlist_art_action(Container *root, const std::string &id) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup || rd->playlist_art_job.valid() || rd->playlist_art_chooser.visible()) return;
    finish_playlist_name_edit(root, true);
    cancel_playlist_track_drag(rd);
    auto &playlists = rd->startup->session.playlists;
    auto playlist = std::find_if(playlists.begin(), playlists.end(), [&](const auto &entry) { return entry.id == id; });
    if (playlist == playlists.end()) return;
    rd->playlist_art_error_id = id;
    rd->playlist_art_error.clear();
    if (!playlist->art_file.empty()) {
        rd->playlist_art_job_id = id;
        rd->playlist_art_previous_file = playlist->art_file;
        rd->playlist_art_removing = true;
        rd->playlist_art_job = rd->playlist_art_writer.enqueue([session = rd->startup->state_file, file = playlist->art_file] {
            return playlist_art::ImportResult{{}, playlist_art::remove_image(session, file)};
        });
    } else {
        rd->playlist_art_chooser_id = id;
        const auto window = rd->window->raw_window;
        std::weak_ptr<bool> lifetime = root->lifetime;
        const bool shown = rd->playlist_art_chooser.show([root, lifetime, window, id](std::filesystem::path source) {
            if (lifetime.expired() || !windowing::has_window(window)) return;
            auto rd = static_cast<RootData *>(root->user_data);
            rd->playlist_art_chooser_id.clear();
            if (!source.empty()) {
                auto &playlists = rd->startup->session.playlists;
                auto playlist = std::find_if(playlists.begin(), playlists.end(), [&](const auto &entry) { return entry.id == id; });
                if (playlist != playlists.end()) {
                    rd->playlist_art_job_id = id;
                    rd->playlist_art_previous_file = playlist->art_file;
                    rd->playlist_art_removing = false;
                    rd->playlist_art_job = rd->playlist_art_writer.enqueue([session = rd->startup->state_file, source = std::move(source)] {
                        return playlist_art::import_image(session, source);
                    });
                }
            }
            windowing::redraw(window);
        }, rd->playlist_art_error);
        if (shown) poll_playlist_art_chooser(root);
        else rd->playlist_art_chooser_id.clear();
    }
    windowing::redraw(rd->window->raw_window);
}

static void delete_context_playlist(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup || !rd->context_whole_playlist || rd->context_playlist_id.empty()) return;
    const auto id = rd->context_playlist_id;
    rd->context_overlay->exists = false;
    rd->context_playlist_id.clear();
    rd->context_whole_playlist = false;
    rd->context_paths.clear();
    rd->playlist_submenu = rd->playlist_scroll_dragging = false;
    cancel_playlist_track_drag(rd);
    if (rd->playlist_edit.id == id) rd->playlist_edit = {};
    if (rd->playlist_art_chooser_id == id) {
        rd->playlist_art_chooser.close();
        rd->playlist_art_chooser_id.clear();
    }
    for (const auto &playlist : rd->startup->session.playlists)
        if (playlist.id == id) discard_playlist_art(rd, playlist.art_file);
    if (rd->playlist_art_error_id == id) {
        rd->playlist_art_error_id.clear();
        rd->playlist_art_error.clear();
    }
    std::erase_if(rd->startup->session.playlists, [&](const auto &playlist) { return playlist.id == id; });
    if (rd->startup->session.expanded_playlist_id == id)
        rd->startup->session.expanded_playlist_id.clear();
    if (auto card = playlist_card(rd, id)) {
        std::erase_if(rd->closing_albums, [card](const auto &closing) { return closing.card == card; });
        if (rd->expanded_album == card || rd->outgoing_album == card) {
            rd->outgoing_album = nullptr;
            rd->album_reveal_start.reset();
            rd->album_reveal = 1;
        }
        if (rd->expanded_album == card) {
            rd->expanded_album = nullptr;
            rd->album_visible_height = rd->album_visible_gap = 0;
            rd->album_panel->exists = false;
        }
        rd->artwork->release(static_cast<AlbumData *>(card->user_data)->art);
        std::erase(rd->library->children, card);
        delete card;
    }
    rd->last_album_clicked = rd->album_double_click_target = nullptr;
    rd->album_scroll_start.reset();
    sync_playlist_cards(root);
    layout(root, root, root->real_bounds);
    checkpoint_session(root, true);
    windowing::redraw(rd->window->raw_window);
}

static void finish_playlist_track_drag(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    update_playlist_track_drag(root, false);
    const auto drag = rd->playlist_track_drag;
    cancel_playlist_track_drag(rd);
    windowing::redraw(rd->window->raw_window);
    if (!drag.active || !drag.can_drop || !rd->startup) return;
    auto &playlists = rd->startup->session.playlists;
    auto playlist = std::find_if(playlists.begin(), playlists.end(),
                                 [&](const auto &entry) { return entry.id == drag.playlist_id; });
    if (playlist == playlists.end()) return;
    auto &paths = playlist->tracks;
    const auto source = std::find(paths.begin(), paths.end(), drag.path);
    if (source == paths.end()) return;
    const auto from = static_cast<std::size_t>(source - paths.begin());
    auto destination = std::min(drag.slot, paths.size());
    if (destination > from) --destination;
    if (destination == from) return;
    auto path = std::move(*source);
    paths.erase(source);
    paths.insert(paths.begin() + destination, std::move(path));
    sync_playlist_cards(root);
    layout(root, root, root->real_bounds);
    checkpoint_session(root, true);
    windowing::redraw(rd->window->raw_window);
}

static void remove_context_from_playlist(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup || rd->context_whole_playlist || rd->context_playlist_id.empty() || rd->context_paths.size() != 1) return;
    const auto id = rd->context_playlist_id;
    const auto path = rd->context_paths.front();
    rd->context_overlay->exists = false;
    rd->context_playlist_id.clear();
    rd->playlist_submenu = rd->playlist_scroll_dragging = false;
    windowing::redraw(rd->window->raw_window);
    remove_playlist_track(root, id, path);
}

static void remove_playlist_track(Container *root, std::string id, std::string path) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup) return;
    const auto preferred = preferred_audio_path(path);
    auto &playlists = rd->startup->session.playlists;
    auto playlist = std::find_if(playlists.begin(), playlists.end(), [&](const auto &entry) { return entry.id == id; });
    if (playlist == playlists.end()) return;
    const auto removed = std::erase_if(playlist->tracks, [&](const auto &entry) { return entry == path || entry == preferred; });
    if (!removed) return;
    // An empty playlist keeps its name and identity so it can be filled again.
    sync_playlist_cards(root);
    layout(root, root, root->real_bounds);
    checkpoint_session(root, true);
    windowing::redraw(rd->window->raw_window);
}

static void load_playlist_metadata(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup || rd->playlist_metadata.valid()) return;
    std::set<std::string> missing;
    for (const auto &playlist : rd->startup->session.playlists)
        for (const auto &path : playlist.tracks)
            if (!rd->tracks.contains(path)) missing.insert(path);
    if (missing.empty()) return;
    rd->playlist_metadata = rd->scanner.enqueue([paths = std::move(missing)] {
        std::vector<Option> songs;
        for (const auto &path : paths) {
            Option song;
            try { song = read_track(path); } catch (const std::exception &) {}
            song.full = path;
            if (song.name.empty()) song.name = std::filesystem::path(path).stem().string();
            songs.push_back(std::move(song));
        }
        return songs;
    });
}

static void finish_playlist_metadata(Container *root) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->playlist_track_drag.playlist_id.empty() || rd->playlist_remove_press) return;
    if (!rd->playlist_metadata.valid() || rd->playlist_metadata.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        for (const auto &song : rd->playlist_metadata.get()) {
            if (!rd->tracks.contains(song.full))
                rd->tracks[song.full] = {song.name, song.artist, rd->artwork->create({song.full}), song.album, song.length};
        }
        sync_playlist_cards(root);
        layout(root, root, root->real_bounds);
        load_playlist_metadata(root);
        windowing::redraw(rd->window->raw_window);
    } catch (const std::exception &error) {
        std::cerr << "Could not load playlist metadata: " << error.what() << '\n';
    }
}

static void finish_playlist_name_edit(Container *root, bool commit) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (rd->playlist_edit.id.empty()) return;
    auto edit = std::exchange(rd->playlist_edit, {});
    if (commit && rd->startup) {
        const auto begin = edit.text.find_first_not_of(" \t\r\n");
        if (begin != std::string::npos) {
            const auto name = edit.text.substr(begin, edit.text.find_last_not_of(" \t\r\n") - begin + 1);
            for (auto &playlist : rd->startup->session.playlists) {
                if (playlist.id != edit.id) continue;
                playlist.name = name;
                if (auto card = playlist_card(rd, edit.id))
                    static_cast<AlbumData *>(card->user_data)->name = name;
                checkpoint_session(root, true);
                break;
            }
        }
    }
    windowing::redraw(rd->window->raw_window);
}

static void add_context_to_playlist(Container *root, const std::string &id) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (!rd->startup) return;
    const auto paths = unique_playlist_paths(rd->context_paths);
    if (paths.empty()) return;
    auto &playlists = rd->startup->session.playlists;
    std::string selected = id;
    if (id.empty()) {
        auto uuid = g_uuid_string_random();
        selected = uuid;
        g_free(uuid);
        auto title = std::filesystem::path(paths.front()).stem().string();
        auto found = rd->tracks.find(paths.front());
        if (found == rd->tracks.end() && !rd->context_paths.empty())
            found = rd->tracks.find(rd->context_paths.front());
        if (found != rd->tracks.end() && !found->second.title.empty())
            title = found->second.title;
        playlists.insert(playlists.begin(), {selected, title, paths});
    } else {
        auto playlist = std::find_if(playlists.begin(), playlists.end(), [&](const auto &entry) { return entry.id == id; });
        if (playlist == playlists.end()) return;
        auto combined = playlist->tracks;
        combined.insert(combined.end(), paths.begin(), paths.end());
        playlist->tracks = unique_playlist_paths(combined);
    }
    rd->context_overlay->exists = false;
    rd->playlist_submenu = rd->playlist_scroll_dragging = false;
    rd->last_album_clicked = rd->album_double_click_target = nullptr;
    sync_playlist_cards(root);
    load_playlist_metadata(root);
    layout(root, root, root->real_bounds);
    if (id.empty()) {
        rd->queue_overlay->exists = false;
        if (auto card = playlist_card(rd, selected)) {
            open_album(root, card, false);
            // Let the library calculate the reveal offset, including closing panels.
            rd->album_scroll_from = 0;
            rd->album_scroll_start = std::chrono::steady_clock::now() - std::chrono::milliseconds(320);
            layout(root, root, root->real_bounds);
        }
    }
    checkpoint_session(root, true);
    windowing::redraw(rd->window->raw_window);
}

static void add_song(Container *parent, const Option &option) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    struct OptionData : UserData {
        std::string name;
        std::string full_path;
    };
    auto option_data = new OptionData;
    option_data->name = option.name.empty()
        ? std::filesystem::path(option.full).filename().string()
        : option.name;
    auto append_info = [&](const std::string &value, const std::string &label = "") {
        if (!value.empty())
            option_data->name += " | " + label + value;
    };
    append_info(option.album);
    if (option.year != "0")
        append_info(option.year);
    append_info(option.artist);
    int duration = 0;
    const auto [end, error] = std::from_chars(
        option.length.data(), option.length.data() + option.length.size(), duration);
    if (error == std::errc{} && end == option.length.data() + option.length.size() && duration >= 0)
        append_info(seconds_to_mmss(duration));
    else
        append_info(option.length);
    append_info(option.genre);
    if (option.track != "0")
        append_info(option.track, "Track ");
    if (option.disc != "0")
        append_info(option.disc, "Disc ");
    option_data->full_path = option.full;
    c->user_data = option_data;
    
    c->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto root_data = (RootData *) root->user_data;
        auto dpi = root_data->window->raw_window->dpi;
        c->wanted_bounds.h  = 32 * dpi;
    };
    c->when_paint = [](Container *root, Container *c) {
        auto root_data = (RootData *) root->user_data;
        auto option_data = (OptionData *) c->user_data;
        auto dpi = root_data->window->raw_window->dpi;
        auto cr = root_data->window->raw_window->drawing_context;
        paint_button_bg(root, c);
        auto b = draw_text(cr, 0, 0, option_data->name, 12 * dpi, false, mylar_font, -1, -1, theme().text_primary, false, 0);
        draw_text(cr, 10, center_y(c, b.h), option_data->name, 12 * dpi, true, mylar_font, -1, -1, theme().text_primary, false, 0);
    };
    c->when_clicked = [](Container *root, Container *c) {
        auto option_data = (OptionData *) c->user_data;
        auto btn = c->state.mouse_button_pressed;
        // printf("here\n");
        if (btn == BTN_LEFT) {
            observe_queue();
            const auto index = playback_queue.upcoming_begin();
            playback_queue.add({option_data->full_path}, PlaybackQueue::Action::PlayNext);
            commit_queue(root);
            player->play_queued_item(index);
            // printf(fz("{}\n", option_data->full_path).c_str());
        } else if (btn == BTN_RIGHT) {
            open_queue_context(root, {option_data->full_path});
        }
    };
}

static void fill_out_for_songs(Container *root, const std::vector<Option> &playable) {
    root->type = ::fullycustom;
    root->when_paint = [](Container *root, Container *c) {
        auto root_data = (RootData *) root->user_data;
        first_scale_event_happened = root_data->window->raw_window->fractional_scale_set_once;
        auto cr = root_data->window->raw_window->drawing_context;
        auto b = c->real_bounds;
        set_rect(cr, b); 
        set_argb(cr, theme().background);
        cr->fill();
        // windowing::redraw(mylar_window->raw_window);
    };    
    root->receive_events_even_if_obstructed = true;
    root->when_fine_scrolled = [](Container *root, Container *container, double scroll_x, double scroll_y, bool came_from_touchpad) {
        container->scroll_v_real += 2 * scroll_y;
    };
    root->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        // The vbox pass clamps its own scroll fields while measuring each row.
        const double scroll_offset = c->scroll_v_real;
        c->scroll_v_real = 0;
        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;

        const double content_h = reserved_height(c) + c->wanted_pad.y + c->wanted_pad.h;
        c->scroll_v_real = std::clamp(scroll_offset, std::min(0.0, b.h - content_h), 0.0);

        for (auto child : c->children) {
            modify_all(child, 0, c->scroll_v_real);
        }

        c->real_bounds = b;
    };

    for (const auto &option : playable)
        add_song(root, option);
}

static void show_library_scrollbar(RootData *data) {
    data->scrollbar_activity = std::chrono::steady_clock::now();
    data->artwork_refresh->animating = true;
    poll_artwork(data->artwork_refresh);
}

static void fill_out_for_albums(Container *root, const std::vector<AlbumOption> &albums) {
    auto data = root_data_for(root);
    data->artwork = std::make_shared<AlbumArtCache>();
    data->artwork_refresh = std::make_shared<ArtRefresh>();
    data->artwork_refresh->app = data->app;
    data->artwork_refresh->window = data->window->raw_window;
    data->artwork_refresh->cache = data->artwork;
    root->automatically_paint_children = false;
    root->type = ::fullycustom;
    root->clip = true;
    root->receive_events_even_if_obstructed = true;
    root->when_paint = [](Container *root, Container *c) {
        auto cr = static_cast<RootData *>(root->user_data)->window->raw_window->drawing_context;
        first_scale_event_happened = static_cast<RootData *>(root->user_data)->window->raw_window->fractional_scale_set_once;
        
        set_rect(cr, c->real_bounds);
        cr->set_color(theme().background);
        cr->fill();
        const auto data = static_cast<RootData *>(root->user_data);
        data->artwork_frame_time = std::chrono::steady_clock::now();
        // Ready covers reveal independently; a slow cover must not hold back
        // the rest of the viewport or the playback artwork.
        bool foreground_ready = data->first_frame_shown;
        if (data->current_art_startup_fade && data->current_art) {
            const bool ready = data->first_frame_shown && data->artwork->detail_ready(data->current_art, 128);
            foreground_ready &= ready;
            if (ready && !data->current_art_fade)
                data->current_art_fade = data->artwork_frame_time;
        }
        for (auto i = data->album_first; i < data->album_end; ++i) {
            auto child = c->children[i];
            if (!child->exists)
                continue;
            auto album = static_cast<AlbumData *>(child->user_data);
            const int pixels = std::max(1, static_cast<int>(std::ceil(child->real_bounds.w - 16 * data->dpi)));
            const bool ready = data->first_frame_shown && data->artwork->detail_ready(album->art, pixels);
            foreground_ready &= ready;
            if (ready) {
                if (!album->detail_fade)
                    album->detail_fade = data->artwork_frame_time;
                if (!data->initial_shadow_fade)
                    data->initial_shadow_fade = album->detail_fade;
            }
        }
        // Only background prefetch waits for all foreground requests to finish.
        if (data->artwork_prefetch.advance(*data->artwork, data->artwork_prefetch_pixels, foreground_ready))
            poll_artwork(data->artwork_refresh);
        // Only the first sharp-art reveal controls shadows. Later scrolls,
        // artwork loads, and library rescans keep the full configured shadow.
        if (data->initial_shadow_fade && data->library_shadow_alpha < 1) {
            data->library_shadow_alpha = artwork_fade_duration_ms <= 0 ? 1.0 : std::clamp(
                std::chrono::duration<double, std::milli>(data->artwork_frame_time - *data->initial_shadow_fade).count() /
                artwork_fade_duration_ms, 0.0, 1.0);
            if (data->library_shadow_alpha < 1) {
                data->artwork_refresh->animating = true;
                poll_artwork(data->artwork_refresh);
            }
        }
        for (auto i = data->album_first; i < data->album_end; ++i) {
            auto child = c->children[i];
            if (child->exists)
                child->when_paint(root, child);
        }
        if (data->album_panel && (data->album_panel->exists || !data->closing_albums.empty()))
            data->album_panel->when_paint(root, data->album_panel);
        if (data->library_scroll_max + c->scroll_v_real > .5 * data->dpi) {
            const auto &b = c->real_bounds;
            cr->save();
            cr->rectangle(b.x, b.y, b.w, b.h); cr->clip();
            data->playback_bar_shadow.draw(*cr, {b.x, b.bottom(), b.w, 96 * data->dpi},
                                           0, {.28, 16, -3}, data->dpi);
            cr->restore();
        }
    };
    root->when_mouse_enters_container = root->when_mouse_motion = [](Container *root, Container *) {
        auto data = static_cast<RootData *>(root->user_data);
        if (data->library->interactable && !((data->queue_overlay && data->queue_overlay->exists) || (data->context_overlay && data->context_overlay->exists)))
            show_library_scrollbar(data);
    };
    root->when_mouse_leaves_container = [](Container *root, Container *) {
        auto data = static_cast<RootData *>(root->user_data);
        if (!data->scrollbar_dragging) {
            // Start the fade immediately when leaving the library.
            data->scrollbar_activity = std::chrono::steady_clock::now() - std::chrono::milliseconds(900);
            data->artwork_refresh->animating = true;
            poll_artwork(data->artwork_refresh);
        }
    };
    root->when_mouse_down = [](Container *root, Container *c) {
        auto rd = static_cast<RootData *>(root->user_data);
        if ((rd->queue_overlay && rd->queue_overlay->exists) || (rd->context_overlay && rd->context_overlay->exists)) return;
        if (rd->library_scrollbar && rd->library_scrollbar->interactable &&
            bounds_contains(rd->library_scrollbar->real_bounds, root->mouse_current_x, root->mouse_current_y)) return;
        const auto now = std::chrono::steady_clock::now();
        const bool double_click = c->state.mouse_button_pressed == BTN_LEFT && rd->last_album_clicked &&
            now - rd->last_album_click_time <= std::chrono::milliseconds(400) &&
            std::hypot(root->mouse_current_x - rd->last_album_click_x,
                       root->mouse_current_y - rd->last_album_click_y) <= 8 * rd->dpi;
        rd->album_double_click_target = double_click ? rd->last_album_clicked : nullptr;
        rd->album_double_click_handled = false;
        rd->last_album_clicked = nullptr;
    };
    root->when_clicked = [](Container *root, Container *) {
        // Also catch a second click over empty space after the card has scrolled away.
        auto rd = static_cast<RootData *>(root->user_data);
        if ((rd->queue_overlay && rd->queue_overlay->exists) || (rd->context_overlay && rd->context_overlay->exists)) return;
        if (rd->library_scrollbar && rd->library_scrollbar->interactable &&
            bounds_contains(rd->library_scrollbar->real_bounds, root->mouse_current_x, root->mouse_current_y)) return;
        consume_album_double_click(root);
    };
    root->when_fine_scrolled = [](Container *root, Container *c, double, double scroll_y, bool) {
        auto rd = static_cast<RootData *>(root->user_data);
        if ((rd->queue_overlay && rd->queue_overlay->exists) || (rd->context_overlay && rd->context_overlay->exists)) return;
        rd->album_scroll_start.reset();
        rd->last_album_clicked = nullptr;
        rd->album_double_click_target = nullptr;
        c->scroll_v_real += 2 * scroll_y;
        show_library_scrollbar(rd);
    };
    root->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto data = static_cast<RootData *>(root->user_data);
        const auto window = data->window->raw_window;
        const double dpi = window->dpi;
        const bool restore = !data->scroll_restored ||
            (window->fractional_scale_set_once && !data->scroll_restored_at_preferred_scale);
        if (restore && data->startup && !c->children.empty() && b.w > 0 && b.h > 0) {
            if (!data->scroll_restored)
                restore_expanded_album(root, data->startup->session.expanded_album_track,
                                       data->startup->session.expanded_playlist_id);
            // The provisional layout may clamp at DPI 1; reapply once the preferred scale arrives.
            c->scroll_v_real = data->initial_scroll_offset * dpi;
            data->scroll_restored = true;
            data->scroll_restored_at_preferred_scale = window->fractional_scale_set_once;
        } else if (data->scroll_restored && data->dpi > 0 && dpi != data->dpi) {
            // Later monitor changes preserve the user's current logical offset.
            c->scroll_v_real *= dpi / data->dpi;
        }
        data->dpi = dpi;
        const double pad = std::min(16 * dpi, std::max(0.0, b.w / 2));
        const double gap = 16 * dpi;
        const double width = std::max(0.0, b.w - 2 * pad);
        const double card_w = std::min(192 * dpi, width);
        const double card_h = card_w + 56 * dpi;
        data->artwork_prefetch_pixels = std::max(1, static_cast<int>(std::ceil(card_w - 16 * dpi)));
        const auto columns = std::max<std::size_t>(1, std::floor((width + gap) / (card_w + gap)));
        data->album_columns = columns;
        // Share remaining width across the outer margins and every column gap.
        const double column_gap = std::max(0.0, b.w - columns * card_w) / (columns + 1);
        const auto count = c->children.size() - (data->album_panel ? 1 : 0);
        const auto rows = (count + columns - 1) / columns;
        const auto now = std::chrono::steady_clock::now();
        std::erase_if(data->closing_albums, [&](const auto &closing) {
            return now - closing.start >= std::chrono::milliseconds(320);
        });
        for (auto &closing : data->closing_albums) {
            const auto index = std::distance(c->children.begin(),
                std::find(c->children.begin(), c->children.end(), closing.card));
            closing.row = index / columns;
            const double t = std::clamp(std::chrono::duration<double, std::milli>(now - closing.start).count() / 320.0, 0.0, 1.0);
            const double remaining = 1 - t * t * (3 - 2 * t);
            closing.visible_height = closing.initial_height * dpi * remaining;
            closing.occupied_height = (closing.initial_height + closing.initial_gap) * dpi * remaining;
            data->artwork_refresh->animating = true;
            poll_artwork(data->artwork_refresh);
        }
        auto closing_height_before = [&](std::size_t row) {
            double height = 0;
            for (const auto &closing : data->closing_albums)
                if (closing.row < row)
                    height += closing.occupied_height;
            return height;
        };
        std::size_t expanded_row = no_index;
        double panel_h = 0;
        double panel_full_h = 0;
        if (data->expanded_album) {
            const auto selected = std::find(c->children.begin(), c->children.end(), data->expanded_album);
            expanded_row = std::distance(c->children.begin(), selected) / columns;
            const auto album = static_cast<AlbumData *>(data->expanded_album->user_data);
            const auto tracks = album_track_layout(b.w, dpi, album->album.songs.size(), !album->playlist_id.empty());
            panel_full_h = std::max((150 + 32 * tracks.rows) * dpi, tracks.art_size + 32 * dpi);
            if (data->album_reveal_start) {
                const double t = std::clamp(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - *data->album_reveal_start).count() / 320.0, 0.0, 1.0);
                data->album_reveal = t * t * (3 - 2 * t);
                if (t >= 1) {
                    data->album_reveal_start.reset();
                    data->outgoing_album = nullptr;
                } else {
                    data->artwork_refresh->animating = true;
                    poll_artwork(data->artwork_refresh);
                }
            }
            const double previous_h = data->outgoing_album ? data->album_transition_from_height * dpi : 0;
            data->album_visible_height = previous_h + (panel_full_h - previous_h) * data->album_reveal;
            const double previous_gap = data->outgoing_album ? data->album_transition_from_gap * dpi : 0;
            data->album_visible_gap = previous_gap + (gap - previous_gap) * data->album_reveal;
            panel_h = data->album_visible_height + data->album_visible_gap;
        }
        double content_h = rows ? 2 * pad + rows * card_h + (rows - 1) * gap + panel_h + closing_height_before(rows) : 0;
        if (data->expanded_album) {
            const double target = -(pad + expanded_row * (card_h + gap) + closing_height_before(expanded_row));
            // Allow the final album to reach the viewport top as well.
            content_h = std::max(content_h, b.h - target);
            if (data->album_scroll_start) {
                const double t = std::clamp(std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - *data->album_scroll_start).count() / 320.0, 0.0, 1.0);
                c->scroll_v_real = target + data->album_scroll_from * dpi * (1 - t);
                // Do not clamp away compensation while an old panel above the selection closes.
                content_h = std::max(content_h, b.h - c->scroll_v_real);
                if (t >= 1)
                    data->album_scroll_start.reset();
                else {
                    data->artwork_refresh->animating = true;
                    poll_artwork(data->artwork_refresh);
                }
            }
        }
        const double scroll_max = data->album_scroll_start ? std::max(0.0, c->scroll_v_real) : 0;
        data->library_scroll_max = std::max(0.0, content_h - b.h);
        c->scroll_v_real = std::clamp(c->scroll_v_real, std::min(0.0, b.h - content_h), scroll_max);
        // Lay out the viewport plus one row on either side. Only visible cards
        // issue foreground requests; the prefetch cursor handles the rest.
        const double row_h = card_h + gap;
        const double panel_top = expanded_row == no_index ? 0 :
            (expanded_row + 1) * row_h + closing_height_before(expanded_row + 1);
        auto row_top = [&](std::size_t row) {
            return row * row_h + closing_height_before(row) + (row > expanded_row ? panel_h : 0);
        };
        // Binary searches keep viewport work bounded even with multiple closing rows.
        auto first_row_after = [&](double y) {
            std::size_t low = 0, high = rows;
            while (low < high) {
                const auto mid = low + (high - low) / 2;
                if (row_top(mid) <= y)
                    low = mid + 1;
                else
                    high = mid;
            }
            return low;
        };
        const auto visible_first = first_row_after(-c->scroll_v_real - pad);
        const auto first_row = visible_first > 1 ? visible_first - 2 : 0;
        const auto end_row = std::min(rows, first_row_after(b.h - c->scroll_v_real - pad) + 1);
        const auto first = std::min(count, first_row * columns);
        const auto end = std::min(count, end_row * columns);
        if (data->expanded_album) {
            layout(root, data->album_panel, Bounds(b.x, b.y + pad + panel_top + c->scroll_v_real,
                                                 b.w, panel_full_h));
            data->album_panel->exists = !data->album_panel->real_bounds.intersection(b).empty();
        }
        for (auto &closing : data->closing_albums) {
            closing.bounds = Bounds(b.x, b.y + pad + (closing.row + 1) * row_h +
                closing_height_before(closing.row) + (closing.row > expanded_row ? panel_h : 0) + c->scroll_v_real,
                b.w, closing.initial_height * dpi);
        }
        for (auto i = data->album_first; i < data->album_end; ++i) {
            if (i >= first && i < end)
                continue;
            auto child = c->children[i];
            child->exists = false;
            data->artwork->release(static_cast<AlbumData *>(child->user_data)->art);
        }
        data->album_first = first;
        data->album_end = end;
        for (auto i = first; i < end; ++i) {
            auto child = c->children[i];
            layout(root, child, Bounds(
                b.x + column_gap + (i % columns) * (card_w + column_gap),
                b.y + pad + row_top(i / columns) + c->scroll_v_real, card_w, card_h));
            child->exists = !child->real_bounds.intersection(b).empty();
            auto art = static_cast<AlbumData *>(child->user_data)->art;
            if (!child->exists)
                data->artwork->release(art);
            if (child->exists) {
                // Keep the first preview frame, then prioritize visible detail.
                data->artwork->request(art, data->first_frame_shown ? data->artwork_prefetch_pixels : 0);
            }
        }
        if (data->album_panel && data->album_panel->exists)
            data->artwork->request(static_cast<AlbumData *>(data->expanded_album->user_data)->art,
                                  std::max(1, static_cast<int>(std::ceil(std::min(360 * dpi, b.w * .4)))));
        if (data->first_frame_shown && data->current_art)
            data->artwork->request(data->current_art, 128);
        if (data->artwork->pending() || data->artwork->take_changed())
            poll_artwork(data->artwork_refresh);
    };
    for (const auto &album : albums)
        add_album(root, album);
    sync_playlist_cards(root->parent);
    load_playlist_metadata(root->parent);
}

struct PlaybackData : UserData {
    Container *rescan = nullptr;
    Container *settings = nullptr;
    Container *art_button = nullptr;
    Container *previous = nullptr;
    Container *play = nullptr;
    Container *next = nullptr;
    Container *seek = nullptr;
    Container *mute = nullptr;
    Container *volume = nullptr;
    Container *queue_button = nullptr;
    Bounds info_bounds;
    Bounds elapsed_bounds;
    Bounds duration_bounds;
    std::string path;
    std::string seek_path;
    double elapsed = 0;
    double duration = 0;
    float position = 0;
    float seek_preview = 0;
    float gain = 1;
    float unmuted_gain = .75f;
    bool playing = false;
    bool seeking = false;
};

static PlaybackData *playback_data(Container *root) {
    //static PlaybackData *d = new PlaybackData;
    //return d;
    return static_cast<PlaybackData *>(static_cast<RootData *>(root->user_data)->playback_bar->user_data);
}

static void checkpoint_session(Container *root, bool force) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!data->startup || !data->first_frame_shown)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - data->last_session_save < std::chrono::seconds(2))
        return;
    data->last_session_save = now;
    auto &startup = *data->startup;
    auto state = startup.session;
    const auto position = player->playback_position();
    state.music_root = startup.music_root;
    observe_queue();
    state.queue = player->queue();
    state.queue_playlist_ids = playback_queue.playlist_ids();
    state.current_path = position.path;
    state.current_index = position.index;
    state.seconds = position.seconds;
    state.volume = player->volume();
    state.sample_rate = player->sample_rate();
    state.unmuted_volume = playback_data(root)->unmuted_gain;
    if (data->scroll_restored && data->library) {
        state.scroll_offsets[state.music_root] = data->library->scroll_v_real / data->dpi;
        state.expanded_album_track = expanded_album_track(data);
        state.expanded_playlist_id = expanded_playlist_id(data);
    }
    startup.session = state;
    if (!force && startup.last_saved && *startup.last_saved == state)
        return;
    startup.last_saved = state;
    startup.writer.enqueue([path = startup.state_file, state = std::move(state)] {
        if (!save_session(path, state))
            std::cerr << "Could not save the library session.\n";
        if (!save_session(session_state_path(), state))
            std::cerr << "Could not save the last listening session.\n";
    });
}

// Prepare only display metadata and artwork. Audio restoration stays after the
// first frame, and cached library metadata avoids reopening the selected file.
static void prepare_session_display(Container *root, StartupState &startup) {
    auto rd = static_cast<RootData *>(root->user_data);
    auto display = playback_data(root);
    if (!startup.explicit_files) {
        remove_missing_tracks(startup.session);
        playback_queue.restore(startup.session.queue, startup.session.current_index, startup.session.queue_playlist_ids);
    } else {
        playback_queue.reset(startup.queue, 0);
    }
    const std::string path = startup.explicit_files
        ? (startup.queue.empty() ? std::string() : startup.queue.front())
        : startup.session.current_path;
    display->path = path;
    display->elapsed = startup.explicit_files ? 0 : startup.session.seconds;
    display->gain = startup.session.volume;
    display->previous->interactable = false;
    display->play->interactable = false;
    display->next->interactable = false;
    display->seek->interactable = false;
    if (path.empty())
        return;
    if (!rd->tracks.contains(path)) {
        const auto track = read_track(path);
        rd->tracks[path] = {
            track.name.empty() ? std::filesystem::path(path).stem().string() : track.name,
            track.artist, rd->artwork->create({path}), track.album, track.length};
    }
    const auto &track = rd->tracks.at(path);
    int duration = 0;
    const auto [end, error] = std::from_chars(track.length.data(), track.length.data() + track.length.size(), duration);
    if (error == std::errc{} && end == track.length.data() + track.length.size() && duration > 0) {
        display->duration = duration;
        display->position = std::clamp(static_cast<float>(display->elapsed / duration), 0.0f, 1.0f);
    }
    const auto current = playback_queue.current();
    rd->current_art = playback_art(rd, path, current ? current->playlist_id : std::string{}, 0);
    poll_artwork(rd->artwork_refresh);
}

static bool sync_playback(Container *root) {
    // The saved-session display is authoritative until player initialization.
    if (!static_cast<RootData *>(root->user_data)->playback_initialized)
        return false;
    observe_queue();
    auto data = playback_data(root);
    const auto path = player->current_path();
    const auto index = player->current_index();
    const auto elapsed = player->current_time_seconds();
    const auto duration = player->total_time_seconds();
    const auto gain = player->volume();
    const bool playing = player->is_playing();
    const float position = duration > 0 ? std::clamp(static_cast<float>(elapsed / duration), 0.0f, 1.0f) : 0;
    const bool can_play = !player->queue().empty();
    const bool can_previous = index != no_index && !path.empty();
    const bool can_next = index != no_index && index + 1 < player->queue().size();
    bool changed = data->path != path || data->playing != playing || data->gain != gain ||
        static_cast<int>(data->elapsed) != static_cast<int>(elapsed) || data->duration != duration ||
        std::abs(data->position - position) * data->seek->real_bounds.w >= .5 ||
        data->play->interactable != can_play || data->previous->interactable != can_previous ||
        data->next->interactable != can_next;
    if (data->path != path)
        data->seeking = false;
    auto root_data = static_cast<RootData *>(root->user_data);
    const auto current = playback_queue.current();
    const auto art = path.empty() ? AlbumArtCache::Handle{} : playback_art(root_data, path,
        current && current->path == path ? current->playlist_id : std::string{}, 128);
    // Reconsider fallback after artwork loading and playlist edits, including
    // consecutive queue entries for the same path from different playlists.
    if (art != root_data->current_art) {
        if (root_data->current_art) root_data->artwork->release(root_data->current_art);
        root_data->current_art = art;
        // Only the restored startup cover joins the blurred reveal.
        root_data->current_art_startup_fade = false;
        root_data->current_art_fade.reset();
        changed = true;
    }
    data->path = path;
    data->elapsed = elapsed;
    data->duration = duration;
    // Keep the last painted position until it advances by a visible amount.
    if (changed)
        data->position = position;
    data->gain = gain;
    if (gain > 0)
        data->unmuted_gain = gain;
    data->playing = playing;
    data->play->interactable = can_play;
    data->previous->interactable = can_previous;
    data->next->interactable = can_next;
    data->seek->interactable = duration > 0;
    return changed;
}

static void playback_changed(Container *root) {
    sync_playback(root);
    auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
    if (windowing::has_window(window))
        windowing::redraw(window);
}

static void start_library_rescan(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    if (data->scan.valid() || !data->startup)
        return;
    data->scan_failed = false;
    const auto path = data->startup->music_root;
    const bool launch = data->startup->defer_queue_until_scan;
    const auto inputs = data->startup->inputs;
    data->scan = data->scanner.enqueue([path, launch, inputs] {
        auto tracks = rescan_library(path, library_cache_path(path));
        sort_tracks(tracks);
        LibraryScanResult result;
        result.albums = to_albums(tracks);
        result.launch = launch;
        if (launch) {
            auto append = [&](const auto &albums) {
                for (const auto &album : albums)
                    for (const auto &song : album.songs)
                        result.launch_queue.push_back(song.full);
            };
            for (const auto &input : inputs) {
                if (std::filesystem::is_directory(input)) {
                    if (input == std::filesystem::path(path)) {
                        append(result.albums);
                    } else {
                        auto extra = rescan_library(input.string(), library_cache_path(input.string()));
                        sort_tracks(extra);
                        append(to_albums(extra));
                    }
                } else {
                    result.launch_queue.push_back(input.string());
                }
            }
        }
        return result;
    });
    playback_data(root)->rescan->interactable = false;
    windowing::redraw(data->window->raw_window);
}

static void finish_library_rescan(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    // Keep the pressed panel alive until the drag or ordinary track click ends.
    if (!data->playlist_track_drag.playlist_id.empty() || data->playlist_remove_press) return;
    if (!data->scan.valid() || data->scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        auto result = data->scan.get();
        // The worker publishes a complete snapshot. All container and artwork
        // mutations happen together on the event thread, between frames.
        auto library = data->library;
        struct RetainedArt {
            AlbumArtCache::Handle art;
            std::optional<std::chrono::steady_clock::time_point> detail_fade;
        };
        const auto expanded_track = expanded_album_track(data);
        const auto expanded_playlist = expanded_playlist_id(data);
        std::map<std::vector<std::string>, RetainedArt> retained_art;
        std::map<std::string, RetainedArt> retained_playlist_art;
        if (data->album_panel) {
            std::erase(library->children, data->album_panel);
            delete data->album_panel;
            data->album_panel = nullptr;
            data->expanded_album = nullptr;
            data->outgoing_album = nullptr;
            data->closing_albums.clear();
            data->last_album_clicked = nullptr;
            data->album_double_click_target = nullptr;
            data->album_scroll_start.reset();
            data->album_reveal_start.reset();
        }
        for (auto child : library->children) {
            auto album = static_cast<AlbumData *>(child->user_data);
            std::vector<std::string> paths;
            for (const auto &song : album->album.songs)
                paths.push_back(song.full);
            if (album->playlist_id.empty())
                retained_art.emplace(std::move(paths), RetainedArt{album->art, album->detail_fade});
            else
                retained_playlist_art.emplace(album->playlist_id, RetainedArt{album->art, album->detail_fade});
            delete child;
        }
        library->children.clear();
        data->artwork_prefetch.reset();
        data->album_first = data->album_end = 0;
        // Keep metadata for queue and playlist tracks outside the scanned library.
        std::set<std::string> playlist_paths;
        for (const auto &playlist : data->startup->session.playlists)
            playlist_paths.insert(playlist.tracks.begin(), playlist.tracks.end());
        std::erase_if(data->tracks, [&playlist_paths](const auto &entry) {
            const auto &queue = player->queue();
            return !playlist_paths.contains(entry.first) && std::find(queue.begin(), queue.end(), entry.first) == queue.end();
        });
        for (const auto &album : result.albums) {
            std::vector<std::string> paths;
            for (const auto &song : album.songs)
                paths.push_back(song.full);
            auto previous = retained_art.find(paths);
            // Rebuilding metadata must not restart a transition for resident artwork.
            // Preserve even an in-progress fade so it continues from the same time.
            if (previous != retained_art.end())
                add_album(library, album, previous->second.art, previous->second.detail_fade);
            else
                add_album(library, album);
            if (previous != retained_art.end())
                retained_art.erase(previous);
        }
        sync_playlist_cards(root);
        for (const auto &[id, retained] : retained_playlist_art) {
            if (auto card = playlist_card(data, id)) {
                auto album = static_cast<AlbumData *>(card->user_data);
                if (album->art == retained.art) album->detail_fade = retained.detail_fade;
            }
            data->artwork->release(retained.art);
        }
        load_playlist_metadata(root);
        restore_expanded_album(root, expanded_track, expanded_playlist);
        for (const auto &[paths, retained] : retained_art)
            data->artwork->release(retained.art);
        if (result.launch) {
            data->startup->defer_queue_until_scan = false;
            // A user may have chosen an album while the initial scan ran.
            if (player->queue().empty()) {
                const bool loaded = player->restore_session(std::move(result.launch_queue), 0, 0);
                if (!player->queue().empty() && (!loaded || !player->start()))
                    std::cerr << "Could not start playback: " << player->last_error() << '\n';
            }
        }
        sync_playback(root);
        layout(root, root, root->real_bounds);
        for (std::size_t i = 0; i < library->children.size(); ++i) {
            if (library->children[i] == data->album_panel || library->children[i] == data->expanded_album)
                continue;
            if (i < data->album_first || i >= data->album_end)
                data->artwork->release(static_cast<AlbumData *>(library->children[i]->user_data)->art);
        }
    } catch (const std::exception &error) {
        data->scan_failed = true;
        std::cerr << "Could not rescan library: " << error.what() << '\n';
    }
    playback_data(root)->rescan->interactable = true;
    windowing::redraw(data->window->raw_window);
}

static void poll_playback(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!data->app)
        return;
    std::weak_ptr<bool> lifetime = root->lifetime;
    auto window = data->window->raw_window;
    windowing::timer(data->app, player->is_playing() ? 50 : 200, [root, lifetime, window](void *) {
        if (lifetime.expired() || !windowing::has_window(window))
            return;
        const bool conversion_finished = player->poll_conversion();
        if (conversion_finished) {
            auto rd = static_cast<RootData *>(root->user_data);
            if (!rd->playlist_track_drag.path.empty())
                rd->playlist_track_drag.path = preferred_audio_path(rd->playlist_track_drag.path);
            for (auto card : rd->library->children) {
                // The expanded panel is also a library child, but has no AlbumData.
                if (card == rd->album_panel || !card->user_data) continue;
                auto album = static_cast<AlbumData *>(card->user_data);
                for (auto &song : album->album.songs) {
                    const auto converted = preferred_audio_path(song.full);
                    if (converted != song.full) {
                        if (auto it = rd->tracks.find(song.full); it != rd->tracks.end()) {
                            auto display = it->second;
                            rd->tracks[converted] = std::move(display);
                        }
                        song.full = converted;
                    }
                }
            }
            sync_playlist_cards(root);
            load_playlist_metadata(root);
            layout(root, root, root->real_bounds);
            checkpoint_session(root, true);
        }
        const auto conversion = player->conversion_progress();
        if (conversion_finished || conversion.active) windowing::redraw(window);
        finish_library_rescan(root);
        finish_playlist_metadata(root);
        auto data = static_cast<RootData *>(root->user_data);
        data->playlist_art_chooser.poll();
        finish_playlist_art_job(root);
        if (data->pipewire_action.valid() &&
            data->pipewire_action.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const auto result = data->pipewire_action.get();
            data->pipewire_status = result.message;
            data->pipewire_error = !result.success;
            std::error_code error;
            data->pipewire_config_exists = std::filesystem::exists(pipewire_rates_path(), error);
            windowing::redraw(window);
        }
        if (sync_playback(root))
            windowing::redraw(window);
        checkpoint_session(root);
        poll_playback(root);
    }, nullptr);
}

enum class PlaybackButton { Previous, Play, Next, Mute };

static void paint_playback_button(Container *root, Container *c, PlaybackButton button) {
    auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
    auto data = playback_data(root);
    auto cr = window->drawing_context;
    const double scale = std::min(c->real_bounds.w, c->real_bounds.h) / 32;
    const bool active = c->interactable;
    cr->save();
    cr->translate(c->real_bounds.x + c->real_bounds.w / 2, c->real_bounds.y + c->real_bounds.h / 2);
    cr->scale(scale, scale);
    if (!active)
        cr->set_color(theme().disabled);
    else if (c->state.mouse_hovering)
        cr->set_color(theme().accent_hover);
    else
        cr->set_color(theme().icon);
    if (button == PlaybackButton::Play) {
        if (!active)
            cr->set_color(theme().button_disabled);
        else if (c->state.mouse_pressing)
            cr->set_color(theme().accent_pressed);
        else if (c->state.mouse_hovering)
            cr->set_color(theme().accent);
        else
            cr->set_color(theme().accent_fill);
        cr->arc(0, 0, 16, 0, 2 * M_PI);
        cr->fill();
        cr->set_color(accent_colors::foreground);
        if (data->playing) {
            cr->rectangle(-5, -6, 3, 12);
            cr->rectangle(2, -6, 3, 12);
        } else {
            cr->move_to(-4, -7);
            cr->line_to(7, 0);
            cr->line_to(-4, 7);
            cr->close_path();
        }
        cr->fill();
    } else if (button == PlaybackButton::Previous || button == PlaybackButton::Next) {
        if (button == PlaybackButton::Next)
            cr->scale(-1, 1);
        cr->rectangle(-8, -6, 2.5, 12);
        cr->move_to(-5, 0);
        cr->line_to(6, -7);
        cr->line_to(6, 7);
        cr->close_path();
        cr->fill();
    } else {
        cr->move_to(-10, -3);
        cr->line_to(-6, -3);
        cr->line_to(-1, -7);
        cr->line_to(-1, 7);
        cr->line_to(-6, 3);
        cr->line_to(-10, 3);
        cr->close_path();
        cr->fill();
        cr->set_line_width(1.6);
        cr->set_line_cap(drawing::LineCap::Round);
        if (data->gain == 0) {
            cr->move_to(4, -3);
            cr->line_to(10, 3);
            cr->move_to(10, -3);
            cr->line_to(4, 3);
        } else {
            cr->arc(-1, 0, 7, -M_PI / 4, M_PI / 4);
            if (data->gain > .5f) {
                cr->new_sub_path();
                cr->arc(-1, 0, 11, -M_PI / 4, M_PI / 4);
            }
        }
        cr->stroke();
    }
    cr->restore();
}

static void activate_playback_button(Container *root, PlaybackButton button) {
    auto data = playback_data(root);
    const auto index = player->current_index();
    switch (button) {
        case PlaybackButton::Play:
            if (player->is_playing())
                player->pause();
            else
                player->start();
            break;
        case PlaybackButton::Previous:
            if (index != no_index && index > 0)
                player->play_queued_item(index - 1);
            else if (index == 0)
                player->seek_to_start();
            break;
        case PlaybackButton::Next:
            if (index != no_index && index + 1 < player->queue().size())
                player->play_queued_item(index + 1);
            break;
        case PlaybackButton::Mute:
            if (player->volume() > 0) {
                data->unmuted_gain = player->volume();
                player->set_volume(0);
            } else {
                player->set_volume(data->unmuted_gain);
            }
            break;
    }
    playback_changed(root);
}

static Container *add_playback_button(Container *root, Container *bar, const char *name, PlaybackButton button) {
    auto c = bar->child(FILL_SPACE, FILL_SPACE);
    c->name = name;
    c->when_drag_end_is_click = false;
    c->when_paint = [button](Container *root, Container *c) { paint_playback_button(root, c, button); };
    c->when_clicked = [button](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT || !c->interactable)
            return;
        activate_playback_button(root, button);
    };
    return c;
}

static void playback_key_event(Container *root, Container *, int, bool pressed,
                               xkb_keysym_t sym, int mods, bool is_text, std::string text) {
    auto rd = static_cast<RootData *>(root->user_data);
    if (rd->playlist_track_drag.active) {
        if (pressed && sym == XKB_KEY_Escape) {
            cancel_playlist_track_drag(rd);
            windowing::redraw(rd->window->raw_window);
        }
        return;
    }
    if (!rd->playlist_edit.id.empty()) {
        if (!pressed) return;
        if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_Tab) {
            finish_playlist_name_edit(root, sym != XKB_KEY_Escape);
            return;
        }
        auto &edit = rd->playlist_edit;
        if ((mods & MOD_CTRL) && !(mods & (MOD_ALT | MOD_SUPER)) && (sym == XKB_KEY_a || sym == XKB_KEY_A)) {
            edit.anchor = 0;
            edit.caret = edit.text.size();
        } else if (!(mods & (MOD_CTRL | MOD_ALT | MOD_SUPER))) {
            const bool selecting = mods & MOD_SHIFT;
            if (sym == XKB_KEY_Left || sym == XKB_KEY_Right || sym == XKB_KEY_Home || sym == XKB_KEY_End) {
                if (sym == XKB_KEY_Home) edit.caret = 0;
                else if (sym == XKB_KEY_End) edit.caret = edit.text.size();
                else if (!selecting && edit.caret != edit.anchor)
                    edit.caret = sym == XKB_KEY_Left ? std::min(edit.caret, edit.anchor) : std::max(edit.caret, edit.anchor);
                else edit.caret = sym == XKB_KEY_Left ? edit.previous(edit.caret) : edit.next(edit.caret);
                if (!selecting) edit.anchor = edit.caret;
            } else if (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Delete) {
                if (edit.caret == edit.anchor)
                    edit.anchor = sym == XKB_KEY_BackSpace ? edit.previous(edit.caret) : edit.next(edit.caret);
                edit.erase_selection();
            } else if (is_text && !text.empty() && g_utf8_validate(text.data(), text.size(), nullptr)) {
                bool printable = true;
                for (auto p = text.c_str(); *p; p = g_utf8_next_char(p))
                    if (g_unichar_iscntrl(g_utf8_get_char(p))) printable = false;
                if (printable) {
                    edit.erase_selection();
                    edit.text.insert(edit.caret, text);
                    edit.caret += text.size();
                    edit.anchor = edit.caret;
                }
            }
        }
        windowing::redraw(rd->window->raw_window);
        return;
    }
    if (!pressed || (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)))
        return;
    if (sym == XKB_KEY_Escape && (rd->queue_overlay->exists || rd->context_overlay->exists)) {
        rd->queue_overlay->exists = false;
        rd->context_overlay->exists = false;
        rd->playlist_submenu = rd->playlist_scroll_dragging = false;
        windowing::redraw(rd->window->raw_window);
        return;
    }
    switch (sym) {
        case XKB_KEY_space:
            activate_playback_button(root, PlaybackButton::Play);
            return;
        case XKB_KEY_n:
        case XKB_KEY_N:
            activate_playback_button(root, PlaybackButton::Next);
            return;
        case XKB_KEY_p:
        case XKB_KEY_P:
            activate_playback_button(root, PlaybackButton::Previous);
            return;
        case XKB_KEY_comma:
            player->seek_relative_seconds(-10);
            break;
        case XKB_KEY_period:
            player->seek_relative_seconds(10);
            break;
        case XKB_KEY_plus:
        case XKB_KEY_KP_Add:
            player->set_volume(std::clamp(player->volume() + .05f, 0.0f, 1.0f));
            break;
        case XKB_KEY_minus:
        case XKB_KEY_KP_Subtract:
            player->set_volume(std::clamp(player->volume() - .05f, 0.0f, 1.0f));
            break;
        default:
            return;
    }
    playback_changed(root);
}

static float slider_position(Container *root, Container *c) {
    const double dpi = static_cast<RootData *>(root->user_data)->window->raw_window->dpi;
    const double inset = std::min(6 * dpi, c->real_bounds.w / 2);
    const double width = c->real_bounds.w - 2 * inset;
    return width > 0 ? std::clamp(static_cast<float>(
        (root->mouse_current_x - c->real_bounds.x - inset) / width), 0.0f, 1.0f) : 0;
}

static Container *add_playback_slider(Container *bar, const char *name, bool volume) {
    auto c = bar->child(FILL_SPACE, FILL_SPACE);
    c->name = name;
    c->when_drag_end_is_click = false;
    c->when_paint = [volume](Container *root, Container *c) {
        auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
        auto cr = window->drawing_context;
        auto data = playback_data(root);
        const double dpi = window->dpi;
        const double inset = std::min(6 * dpi, c->real_bounds.w / 2);
        const double x = c->real_bounds.x + inset;
        const double width = std::max(0.0, c->real_bounds.w - 2 * inset);
        const double y = c->real_bounds.y + c->real_bounds.h / 2;
        const double value = volume ? data->gain : data->seeking ? data->seek_preview : data->position;
        const bool highlight = c->interactable && (c->state.mouse_hovering || c->state.mouse_pressing);
        cr->save();
        cr->set_line_cap(drawing::LineCap::Round);
        cr->set_line_width(4 * dpi);
        cr->set_color(theme().track);
        cr->move_to(x, y);
        cr->line_to(x + width, y);
        cr->stroke();
        if (highlight)
            cr->set_color(theme().accent_hover);
        else
            cr->set_color(theme().accent_fill);
        if (value > 0 && c->interactable) {
            cr->move_to(x, y);
            cr->line_to(x + width * value, y);
            cr->stroke();
        }
        if (highlight) {
            cr->set_color(theme().accent_hover);
            cr->arc(x + width * value, y, 5 * dpi, 0, 2 * M_PI);
            cr->fill();
        }
        cr->restore();
    };
    auto update = [volume](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT || !c->interactable)
            return;
        auto data = playback_data(root);
        const auto value = slider_position(root, c);
        if (volume) {
            player->set_volume(value);
            if (value > 0)
                data->unmuted_gain = value;
        } else {
            if (!data->seeking)
                data->seek_path = player->current_path();
            data->seeking = true;
            data->seek_preview = value;
        }
        playback_changed(root);
    };
    c->when_mouse_down = update;
    c->when_drag_start = update;
    c->when_drag = update;
    auto finish = [volume, update](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = playback_data(root);
        if (volume) {
            update(root, c);
        } else if (data->seeking) {
            if (player->current_path() == data->seek_path)
                player->seek(slider_position(root, c));
            data->seeking = false;
            playback_changed(root);
        }
    };
    c->when_clicked = finish;
    c->when_drag_end = finish;
    return c;
}

static void close_artwork_preview(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    if (data->preview_art)
        data->artwork->release(data->preview_art);
    data->preview_art.reset();
    data->preview_playlist_id.clear();
    data->artwork_preview->exists = false;
    data->library->interactable = true;
    data->playback_bar->interactable = true;
    playback_changed(root);
}

static void open_artwork_preview(Container *root, const AlbumArtCache::Handle &art, const std::string &playlist_id) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!art || !data->artwork_preview)
        return;
    data->preview_art = data->artwork->create_preview(art);
    data->preview_playlist_id = playlist_id;
    data->preview_bounds = {};
    data->artwork_preview->exists = true;
    data->library->interactable = false;
    data->playback_bar->interactable = false;
    poll_artwork(data->artwork_refresh);
    playback_changed(root);
}

static void fill_artwork_preview(Container *root, Container *overlay) {
    auto data = static_cast<RootData *>(root->user_data);
    data->artwork_preview = overlay;
    overlay->name = "artwork-preview";
    overlay->type = ::fullycustom;
    overlay->z_index = 100;
    overlay->exists = false;
    overlay->when_drag_end_is_click = false;
    auto close = overlay->child(FILL_SPACE, FILL_SPACE);
    close->name = "close-artwork-preview";
    close->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT)
            close_artwork_preview(root);
    };
    close->when_paint = [](Container *root, Container *c) {
        auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
        auto cr = window->drawing_context;
        const auto &b = c->real_bounds;
        const double inset = b.w * .32;
        cr->save();
        cr->set_color(with_alpha(theme().on_accent, c->state.mouse_hovering ? 1 : .7));
        cr->set_line_width(2 * window->dpi);
        cr->move_to(b.x + inset, b.y + inset);
        cr->line_to(b.right() - inset, b.bottom() - inset);
        cr->move_to(b.right() - inset, b.y + inset);
        cr->line_to(b.x + inset, b.bottom() - inset);
        cr->stroke();
        cr->restore();
    };
    overlay->pre_layout = [close](Container *root, Container *c, const Bounds &b) {
        auto data = static_cast<RootData *>(root->user_data);
        const double dpi = data->window->raw_window->dpi;
        const double size = std::min(40 * dpi, b.w);
        layout(root, close, Bounds(b.right() - size, b.y, size, size));
        if (c->exists && data->preview_art) {
            data->artwork->request(data->preview_art, -1);
            if (data->artwork->pending())
                poll_artwork(data->artwork_refresh);
        }
    };
    overlay->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        auto window = data->window->raw_window;
        auto cr = window->drawing_context;
        const auto &b = c->real_bounds;
        cr->save();
        set_rect(cr, b);
        cr->set_color(theme_colors::preview_scrim);
        cr->fill();
        const auto image = data->preview_art ? data->artwork->image(data->preview_art) : nullptr;
        data->preview_bounds = {};
        if (image) {
            const double padding = std::min({48.0 * window->dpi, b.w / 4, b.h / 4});
            const double scale = std::min((b.w - 2 * padding) / image->width, (b.h - 2 * padding) / image->height);
            const double width = image->width * scale, height = image->height * scale;
            data->preview_bounds = Bounds(b.x + (b.w - width) / 2, b.y + (b.h - height) / 2, width, height);
            cr->translate(data->preview_bounds.x, data->preview_bounds.y);
            cr->scale(scale, scale);
            cr->draw_image(*image, 1, drawing::ImageFilter::Good);
        } else {
            draw_text(cr, b.x, b.y + b.h / 2, data->artwork->pending() ? "Loading artwork…" : "No artwork available",
                      14 * window->dpi, true, mylar_font, b.w, -1, theme().on_accent, false, 1);
        }
        cr->restore();
    };
    overlay->when_clicked = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (c->state.mouse_button_pressed == BTN_LEFT &&
            !bounds_contains(data->preview_bounds, root->mouse_current_x, root->mouse_current_y))
            close_artwork_preview(root);
    };
    overlay->when_key_event = [](Container *root, Container *c, int, bool pressed, xkb_keysym_t sym, int, bool, std::string) {
        if (c->exists && pressed && sym == XKB_KEY_Escape)
            close_artwork_preview(root);
    };
}

static void fill_playback_bar(Container *root, Container *bar) {
    auto data = new PlaybackData;
    bar->user_data = data;
    bar->name = "playback-bar";
    bar->type = ::fullycustom;
    static_cast<RootData *>(root->user_data)->playback_bar = bar;
    data->previous = add_playback_button(root, bar, "previous-track", PlaybackButton::Previous);
    data->play = add_playback_button(root, bar, "play-pause", PlaybackButton::Play);
    data->next = add_playback_button(root, bar, "next-track", PlaybackButton::Next);
    data->seek = add_playback_slider(bar, "song-progress", false);
    data->mute = add_playback_button(root, bar, "mute", PlaybackButton::Mute);
    data->volume = add_playback_slider(bar, "volume", true);
    data->queue_button = bar->child(FILL_SPACE, FILL_SPACE);
    data->queue_button->name = "show-queue";
    data->queue_button->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT) return;
        auto rd = static_cast<RootData *>(root->user_data);
        rd->context_overlay->exists = false;
        rd->queue_overlay->exists = !rd->queue_overlay->exists;
        rd->queue_scroll = 0;
        layout(root, root, root->real_bounds);
        playback_changed(root);
    };
    data->queue_button->when_paint = [](Container *root, Container *c) {
        auto rd = static_cast<RootData *>(root->user_data);
        auto cr = rd->window->raw_window->drawing_context;
        const auto &b = c->real_bounds;
        const double scale = std::min(b.w / 72, b.h / 32);
        if (scale <= 0) return;
        const bool selected = rd->queue_overlay->exists;
        const auto foreground = selected || c->state.mouse_pressing
            ? accent_colors::foreground : theme().text;
        cr->save();
        const Bounds pill(b.x, b.y + (b.h - 28 * scale) / 2, b.w, 28 * scale);
        rounded_rectangle(cr, pill, pill.h / 2);
        cr->set_color(c->state.mouse_pressing ? theme().accent_pressed :
                      selected ? theme().accent_fill :
                      c->state.mouse_hovering ? theme().button_hover : theme().button);
        cr->fill();
        const double stroke = scale;
        rounded_rectangle(cr, Bounds(pill.x + stroke / 2, pill.y + stroke / 2,
                                    pill.w - stroke, pill.h - stroke), (pill.h - stroke) / 2);
        cr->set_line_width(stroke);
        cr->set_color(selected || c->state.mouse_hovering ? theme().accent : theme().border);
        cr->stroke();
        const int font_size = std::max(1, static_cast<int>(11 * scale));
        const auto text = draw_text(cr, 0, 0, "Queue", font_size, false,
                                    mylar_font, -1, -1, foreground, true);
        draw_text(cr, b.x, b.y + (b.h - text.h) / 2, "Queue", font_size, true,
                  mylar_font, b.w, -1, foreground, true, 1);
        cr->restore();
    };
    data->settings = bar->child(FILL_SPACE, FILL_SPACE);
    data->settings->name = "settings";
    data->settings->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = static_cast<RootData *>(root->user_data);
        data->settings_menu->exists = true;
        std::error_code error;
        data->pipewire_config_exists = std::filesystem::exists(pipewire_rates_path(), error);
        data->library->interactable = false;
        data->playback_bar->interactable = false;
        layout(root, root, root->real_bounds);
        playback_changed(root);
    };
    data->settings->when_paint = [](Container *root, Container *c) {
        auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
        auto cr = window->drawing_context;
        const auto &b = c->real_bounds;
        cr->save();
        cr->translate(b.x + b.w / 2, b.y + b.h / 2);
        const double scale = std::min(b.w, b.h) / 32;
        cr->scale(scale, scale);
        if (c->state.mouse_hovering || c->state.mouse_pressing)
            cr->set_color(theme().accent_hover);
        else
            cr->set_color(theme().icon);
        for (int i = 0; i < 48; ++i) {
            const double angle = i * 2 * M_PI / 48;
            const double radius = (i % 6 == 1 || i % 6 == 2) ? 10 : 8;
            const double x = std::cos(angle) * radius, y = std::sin(angle) * radius;
            if (i == 0) cr->move_to(x, y);
            else cr->line_to(x, y);
        }
        cr->close_path();
        cr->new_sub_path();
        cr->arc(0, 0, 3.5, 0, 2 * M_PI);
        cr->set_fill_rule(drawing::FillRule::EvenOdd);
        cr->fill();
        cr->restore();
    };
    data->art_button = bar->child(FILL_SPACE, FILL_SPACE);
    data->art_button->name = "preview-current-artwork";
    data->art_button->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT)
            open_artwork_preview(root, static_cast<RootData *>(root->user_data)->current_art);
    };
    if (auto startup = static_cast<RootData *>(root->user_data)->startup)
        data->unmuted_gain = startup->session.unmuted_volume;
    bar->pre_layout = [](Container *root, Container *bar, const Bounds &b) {
        auto data = static_cast<PlaybackData *>(bar->user_data);
        const double dpi = static_cast<RootData *>(root->user_data)->window->raw_window->dpi;
        const double pad = std::min(16 * dpi, b.w / 8);
        const bool compact = b.w < 1000 * dpi;
        const double right_width = std::min(272 * dpi, b.w * .42);
        const double center = compact ? b.x + (b.w - right_width) / 2 : b.x + b.w / 2;
        const double step = std::min(44 * dpi, (b.w - right_width - 2 * pad) / 3);
        const double button = std::max(0.0, std::min(32 * dpi, step));
        const double button_y = b.y + 20 * dpi;
        layout(root, data->previous, Bounds(center - step - button / 2, button_y, button, button));
        layout(root, data->play, Bounds(center - button / 2, button_y, button, button));
        layout(root, data->next, Bounds(center + step - button / 2, button_y, button, button));
        const double mute_size = std::min(32 * dpi, right_width / 3);
        const double volume_y = b.y + (b.h - mute_size) / 2;
        const double queue_width = std::min(72 * dpi, right_width * .3);
        const double queue_gap = std::min(8 * dpi, right_width * .03);
        const double controls_x = b.right() - right_width;
        layout(root, data->queue_button, Bounds(controls_x, volume_y, queue_width, mute_size));
        layout(root, data->mute, Bounds(controls_x + queue_width + queue_gap, volume_y, mute_size, mute_size));
        layout(root, data->volume, Bounds(controls_x + queue_width + queue_gap + mute_size, volume_y,
            std::max(0.0, right_width - queue_width - queue_gap - 2 * mute_size - pad), mute_size));
        layout(root, data->settings, Bounds(b.right() - pad - mute_size, volume_y,
            mute_size, mute_size));
        const double seek_left = compact ? b.x + pad : b.x + b.w * .28;
        const double seek_width = compact ? std::max(0.0, b.w - right_width - 2 * pad) : b.w * .44;
        const double time_width = std::min(38 * dpi, seek_width / 5);
        const double seek_y = b.y + 54 * dpi;
        layout(root, data->seek, Bounds(seek_left + time_width, seek_y,
            std::max(0.0, seek_width - 2 * time_width), 24 * dpi));
        data->elapsed_bounds = Bounds(seek_left, seek_y + 4 * dpi, time_width, 20 * dpi);
        data->duration_bounds = Bounds(seek_left + seek_width - time_width, seek_y + 4 * dpi, time_width, 20 * dpi);
        data->info_bounds = compact ? Bounds() : Bounds(b.x + pad, b.y + 20 * dpi, b.w * .26 - pad, 56 * dpi);
        data->art_button->exists = !data->info_bounds.empty();
        layout(root, data->art_button, Bounds(data->info_bounds.x, data->info_bounds.y, data->info_bounds.h, data->info_bounds.h));
    };
    bar->when_paint = [](Container *root, Container *bar) {
        auto root_data = static_cast<RootData *>(root->user_data);
        auto data = static_cast<PlaybackData *>(bar->user_data);
        auto window = root_data->window->raw_window;
        const double dpi = window->dpi;
        auto cr = window->drawing_context;
        sync_playback(root);
        cr->save();
        set_rect(cr, bar->real_bounds);
        cr->set_color(theme().playback_surface);
        cr->fill();
        cr->rectangle(bar->real_bounds.x, bar->real_bounds.y, bar->real_bounds.w, dpi);
        cr->set_color(theme().border);
        cr->fill();
        auto text = [&](const Bounds &b, const std::string &value, int size, RGBA color, bool bold, int align) {
            if (b.w > 0)
                draw_text(cr, b.x, b.y, value, size * dpi, true, mylar_font, b.w,
                          b.h, color, bold, align);
        };
        const double elapsed = data->seeking ? data->seek_preview * data->duration : data->elapsed;
        text(data->elapsed_bounds, seconds_to_mmss(std::max(0, static_cast<int>(elapsed))), 9,
             theme().body, false, 1);
        text(data->duration_bounds, seconds_to_mmss(std::max(0, static_cast<int>(data->duration))), 9,
             theme().body, false, 1);
        if (!data->info_bounds.empty()) {
            const auto &b = data->info_bounds;
            auto track = root_data->tracks.find(data->path);
            auto art = root_data->current_art ? root_data->artwork->image(root_data->current_art) : nullptr;
            const double size = b.h;
            cr->rectangle(b.x, b.y, size, size);
            cr->set_color(theme().art_placeholder);
            cr->fill();
            if (art) {
                paint_artwork(root_data, cr, root_data->current_art, art,
                              root_data->current_art_fade, b.x, b.y, size,
                              root_data->current_art_startup_fade);
            } else {
                text(Bounds(b.x, b.y + 14 * dpi, size, 30 * dpi), "♫", 18,
                     theme().art_placeholder_icon, false, 1);
            }
            const double text_x = b.x + size + 12 * dpi;
            const double text_w = std::max(0.0, b.right() - text_x);
            const std::string title = data->path.empty() ? "Choose an album" : track != root_data->tracks.end()
                ? track->second.title : std::filesystem::path(data->path).stem().string();
            text(Bounds(text_x, b.y + 7 * dpi, text_w, 22 * dpi), title, 10,
                 theme().heading, true, 0);
            std::string subtitle;
            if (track != root_data->tracks.end()) {
                subtitle = track->second.artist;
                if (!track->second.album.empty()) {
                    if (!subtitle.empty())
                        subtitle += " · ";
                    subtitle += track->second.album;
                }
            }
            text(Bounds(text_x, b.y + 28 * dpi, text_w, 18 * dpi), subtitle, 11,
                 theme().body, false, 0);
        }
        cr->restore();
    };
    sync_playback(root);
}

static void close_settings_menu(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    data->settings_menu->exists = false;
    data->settings_information = false;
    data->library->interactable = true;
    data->playback_bar->interactable = true;
    playback_changed(root);
}

static double settings_extra_height(Container *root) {
    const auto count = static_cast<RootData *>(root->user_data)->output_rates.size();
    const auto rows = (count + 2) / 3;
    // Space for the More information button above the sample-rate choices.
    return 40.0 + (rows > 2 ? (rows - 2) * 48.0 : 0.0);
}

static double settings_scale(Container *root) {
    const auto data = static_cast<RootData *>(root->user_data);
    const auto window = data->window->raw_window;
    const double width = data->settings_information ? 840 : 680;
    const double height = data->settings_information ? 644 : 548 + settings_extra_height(root);
    return std::max(.1, std::min({static_cast<double>(window->dpi),
        root->real_bounds.w / width, root->real_bounds.h / height}));
}

static void settings_back(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!data->settings_information) {
        close_settings_menu(root);
        return;
    }
    data->settings_information = false;
    layout(root, root, root->real_bounds);
    playback_changed(root);
}

static void fill_settings_menu(Container *root, Container *overlay) {
    auto data = static_cast<RootData *>(root->user_data);
    data->settings_menu = overlay;
    data->output_rates = output_sample_rates();
    overlay->name = "settings-menu";
    overlay->type = ::fullycustom;
    overlay->z_index = 90;
    overlay->exists = false;
    overlay->when_clicked = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (c->state.mouse_button_pressed == BTN_LEFT &&
            !bounds_contains(data->settings_bounds, root->mouse_current_x, root->mouse_current_y))
            close_settings_menu(root);
    };
    overlay->when_key_event = [](Container *root, Container *c, int, bool pressed,
                                 xkb_keysym_t sym, int, bool, std::string) {
        if (c->exists && pressed && sym == XKB_KEY_Escape)
            settings_back(root);
    };
    overlay->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        auto cr = data->window->raw_window->drawing_context;
        const double dpi = settings_scale(root);
        const auto &b = data->settings_bounds;
        cr->save();
        set_rect(cr, c->real_bounds);
        cr->set_color(theme().scrim);
        cr->fill();
        data->settings_shadow.draw(*cr, {b.x, b.y, b.w, b.h}, popup_corner_radius * dpi, popup_shadow, dpi);
        rounded_rectangle(cr, b, popup_corner_radius * dpi);
        cr->set_color(theme().panel);
        cr->fill();
        auto text = [&](double y, const std::string &label, int size, RGBA color, bool bold = false) {
            draw_text(cr, b.x + 28 * dpi, b.y + y * dpi, label, size * dpi, true,
                      mylar_font, b.w - 56 * dpi, -1, color, bold, 0);
        };
        if (data->settings_information) {
            const auto body = theme().body;
            const auto heading = theme().heading;
            // Explicit lines keep the instructions and command fully visible without wrapping.
            auto line = [&](double y, const std::string &label) {
                draw_text(cr, b.x + 28 * dpi, b.y + y * dpi, label, 10 * dpi, true,
                          mylar_font, -1, -1, body, false, 0);
            };
            text(24, "Playback rates", 22, heading, true);
            line(80, "Choose your track's rate in Settings. Your device must support it.");
            text(116, "Check your device", 12, heading, true);
            line(142, "USB audio rates:");
            line(160, "grep -H \"Rates:\" /proc/asound/card*/stream*");
            line(178, "Allow the desired rate in PipeWire's default.clock.allowed-rates.");
            line(196, "Stop other audio apps. Let the device idle a few seconds, then start playback.");
            line(214, "Use pw-top: Tunes and the device should both show the desired RATE.");
            text(250, "PipeWire clock", 12, heading, true);
            line(276, "Force uses the selected rate for all audio apps. Auto releases the override.");
            line(294, "Requires pw-metadata. The override ends when PipeWire restarts.");
            if (!player->uses_pipewire())
                line(332, "PipeWire not detected; controls unavailable.");
            text(380, "Rate configuration", 12, heading, true);
            line(406, pipewire_rates_path().string());
            line(424, "Add sets default.clock.rate and default.clock.allowed-rates in context.properties.");
            line(442, "Check the listed rates match your device; edit the file if needed.");
            line(460, "Remove deletes this file, including your edits. Add preserves existing files.");
            line(478, "Restart PipeWire to apply config changes.");
            if (player->uses_pipewire())
                text(558, data->pipewire_action.valid() ? "Applying…" : data->pipewire_status,
                     10, data->pipewire_error ? theme().error : theme().status);
        } else {
            text(24, "Settings", 22, theme().heading, true);
            text(87, "Output sample rate", 12, theme().heading, true);
            text(112, "Sets the playback rate if supported.", 10, theme().body);
            text(248 + settings_extra_height(root), data->settings_error.empty()
                 ? "Current output: " + std::to_string(player->sample_rate()) + " Hz" : data->settings_error,
                 10, data->settings_error.empty() ? theme().status : theme().error);
            text(302 + settings_extra_height(root), "Library", 12, theme().heading, true);
        }
        cr->restore();
    };
    auto make_button = [&](const char *name, std::function<std::string()> label, unsigned rate = 0) {
        auto item = overlay->child(FILL_SPACE, FILL_SPACE);
        item->name = name;
        item->z_index = 1;
        item->when_paint = [label, rate](Container *root, Container *c) {
            auto cr = static_cast<RootData *>(root->user_data)->window->raw_window->drawing_context;
            const auto &b = c->real_bounds;
            const double dpi = settings_scale(root);
            const bool selected = rate != 0 && player->sample_rate() == rate;
            cr->save();
            set_rect(cr, b);
            if (selected) cr->set_color(theme().accent_fill);
            else if (c->interactable && c->state.mouse_hovering) cr->set_color(theme().button_hover);
            else cr->set_color(theme().button);
            cr->fill();
            draw_text(cr, b.x, b.y + 10 * dpi, label(), 10 * dpi, true,
                      mylar_font, b.w, -1, selected ? accent_colors::foreground :
                      c->interactable ? theme().status : theme().disabled_text,
                      selected, 1);
            cr->restore();
        };
        return item;
    };
    auto close = make_button("close-settings", [data] { return data->settings_information ? "Back" : "Close"; });
    close->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT)
            settings_back(root);
    };
    auto information = make_button("playback-more-information", [] { return "More information"; });
    information->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = static_cast<RootData *>(root->user_data);
        data->settings_information = true;
        layout(root, root, root->real_bounds);
        playback_changed(root);
    };
    std::vector<Container *> rates;
    for (unsigned rate : data->output_rates) {
        auto item = make_button("output-sample-rate", [rate] { return std::to_string(rate) + " Hz"; }, rate);
        item->when_clicked = [rate](Container *root, Container *c) {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;
            auto data = static_cast<RootData *>(root->user_data);
            data->settings_error = player->set_sample_rate(rate) ? "" : player->last_error();
            layout(root, root, root->real_bounds);
            checkpoint_session(root, true);
            playback_changed(root);
        };
        rates.push_back(item);
    }
    auto rescan = make_button("rescan-library", [data] {
        return data->scan.valid() ? "Scanning library…" : data->scan_failed ? "Retry library scan" : "Rescan library";
    });
    playback_data(root)->rescan = rescan;
    rescan->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT && c->interactable)
            start_library_rescan(root);
    };
    auto auto_rescan = overlay->child(FILL_SPACE, FILL_SPACE);
    auto_rescan->name = "rescan-on-launch";
    auto_rescan->z_index = 1;
    auto_rescan->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        auto cr = data->window->raw_window->drawing_context;
        const double dpi = settings_scale(root);
        const auto &b = c->real_bounds;
        const bool enabled = data->startup->session.rescan_on_launch;
        cr->save();
        draw_text(cr, b.x, b.y + 10 * dpi, "Rescan on launch", 10 * dpi, true,
                  mylar_font, b.w - 100 * dpi, -1, theme().heading, false, 0);
        draw_text(cr, b.right() - 96 * dpi, b.y + 10 * dpi, enabled ? "On" : "Off", 10 * dpi, true,
                  mylar_font, 36 * dpi, -1, theme().body, false, 1);
        const double x = b.right() - 48 * dpi, y = b.y + b.h / 2;
        cr->set_line_width(24 * dpi);
        cr->set_line_cap(drawing::LineCap::Round);
        if (enabled) cr->set_color(theme().accent_fill);
        else cr->set_color(theme().disabled);
        cr->move_to(x + 12 * dpi, y);
        cr->line_to(x + 36 * dpi, y);
        cr->stroke();
        cr->set_color(theme().on_accent);
        cr->arc(x + (enabled ? 36 : 12) * dpi, y, 9 * dpi, 0, 2 * M_PI);
        cr->fill();
        cr->restore();
    };
    auto_rescan->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = static_cast<RootData *>(root->user_data);
        auto &enabled = data->startup->session.rescan_on_launch;
        enabled = !enabled;
        checkpoint_session(root, true);
        playback_changed(root);
    };
    auto theme_toggle = overlay->child(FILL_SPACE, FILL_SPACE);
    theme_toggle->name = "dark-theme";
    theme_toggle->z_index = 1;
    theme_toggle->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        auto cr = data->window->raw_window->drawing_context;
        const double dpi = settings_scale(root);
        const auto &b = c->real_bounds;
        const bool enabled = data->startup->session.dark_theme;
        cr->save();
        draw_text(cr, b.x, b.y + 10 * dpi, "Dark theme", 10 * dpi, true,
                  mylar_font, b.w - 100 * dpi, -1, theme().heading, false, 0);
        draw_text(cr, b.right() - 96 * dpi, b.y + 10 * dpi, enabled ? "On" : "Off", 10 * dpi, true,
                  mylar_font, 36 * dpi, -1, theme().body, false, 1);
        const double x = b.right() - 48 * dpi, y = b.y + b.h / 2;
        cr->set_line_width(24 * dpi);
        cr->set_line_cap(drawing::LineCap::Round);
        if (enabled) cr->set_color(theme().accent_fill);
        else cr->set_color(theme().disabled);
        cr->move_to(x + 12 * dpi, y);
        cr->line_to(x + 36 * dpi, y);
        cr->stroke();
        cr->set_color(theme().on_accent);
        cr->arc(x + (enabled ? 36 : 12) * dpi, y, 9 * dpi, 0, 2 * M_PI);
        cr->fill();
        cr->restore();
    };
    theme_toggle->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = static_cast<RootData *>(root->user_data);
        auto &enabled = data->startup->session.dark_theme;
        enabled = !enabled;
        dark_theme_enabled = enabled;
        data->window->bg_color = theme().background;
        checkpoint_session(root, true);
        playback_changed(root);
    };
    auto force_clock = make_button("pipewire-force-clock", [] {
        return "Force clock to " + std::to_string(player->sample_rate()) + " Hz";
    });
    auto auto_clock = make_button("pipewire-auto-clock", [] { return "Auto clock"; });
    auto rate_config = make_button("pipewire-rate-config", [data] {
        return data->pipewire_config_exists ? "Remove 10-rates.conf" : "Add 10-rates.conf";
    });
    auto change_clock = [](Container *root, Container *c, bool automatic) {
        auto data = static_cast<RootData *>(root->user_data);
        if (c->state.mouse_button_pressed != BTN_LEFT || !player->uses_pipewire() || data->pipewire_action.valid())
            return;
        const unsigned rate = automatic ? 0 : player->sample_rate();
        data->pipewire_action = data->scanner.enqueue([rate] { return set_pipewire_force_rate(rate); });
        playback_changed(root);
    };
    force_clock->when_clicked = [change_clock](Container *root, Container *c) { change_clock(root, c, false); };
    auto_clock->when_clicked = [change_clock](Container *root, Container *c) { change_clock(root, c, true); };
    rate_config->when_clicked = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (c->state.mouse_button_pressed != BTN_LEFT || !player->uses_pipewire() || data->pipewire_action.valid())
            return;
        const auto path = pipewire_rates_path();
        const auto rates = data->output_rates;
        const bool remove = data->pipewire_config_exists;
        data->pipewire_action = data->scanner.enqueue([path, rates, remove] {
            return change_pipewire_rates_config(path, rates, remove);
        });
        playback_changed(root);
    };
    overlay->pre_layout = [close, information, rates, rescan, auto_rescan, theme_toggle, force_clock, auto_clock, rate_config](Container *root, Container *, const Bounds &b) {
        auto data = static_cast<RootData *>(root->user_data);
        const double dpi = settings_scale(root);
        const double extra = settings_extra_height(root);
        const bool details = data->settings_information;
        const double width = (details ? 800 : 640) * dpi;
        const double height = (details ? 604 : 508 + extra) * dpi;
        const Bounds panel(b.x + (b.w - width) / 2, b.y + (b.h - height) / 2, width, height);
        data->settings_bounds = panel;
        layout(root, close, Bounds(panel.right() - 108 * dpi, panel.y + 24 * dpi, 80 * dpi, 36 * dpi));
        information->exists = rescan->exists = auto_rescan->exists = theme_toggle->exists = !details;
        for (std::size_t i = 0; i < rates.size(); ++i) {
            rates[i]->exists = !details;
            if (!details)
                layout(root, rates[i], Bounds(panel.x + (28 + (i % 3) * 198) * dpi,
                    panel.y + (190 + (i / 3) * 48) * dpi, 188 * dpi, 38 * dpi));
        }
        for (auto *control : {force_clock, auto_clock, rate_config})
            control->exists = details && player->uses_pipewire();
        if (details && player->uses_pipewire()) {
            layout(root, force_clock, Bounds(panel.x + 28 * dpi, panel.y + 322 * dpi, 286 * dpi, 38 * dpi));
            layout(root, auto_clock, Bounds(panel.x + 326 * dpi, panel.y + 322 * dpi, 188 * dpi, 38 * dpi));
            layout(root, rate_config, Bounds(panel.x + 28 * dpi, panel.y + 508 * dpi, 286 * dpi, 38 * dpi));
        } else if (!details) {
            layout(root, information, Bounds(panel.x + 28 * dpi, panel.y + 140 * dpi, 188 * dpi, 36 * dpi));
            layout(root, rescan, Bounds(panel.x + 28 * dpi, panel.y + (332 + extra) * dpi, 188 * dpi, 40 * dpi));
            layout(root, auto_rescan, Bounds(panel.x + 28 * dpi, panel.y + (380 + extra) * dpi, 240 * dpi, 40 * dpi));
            layout(root, theme_toggle, Bounds(panel.x + 28 * dpi, panel.y + (436 + extra) * dpi, 240 * dpi, 40 * dpi));
        }
    };
}

static LibraryScrollMetrics library_scroll_metrics(RootData *data) {
    return {data->library->real_bounds.h, data->library_scroll_max,
            data->library_scrollbar->real_bounds.h, data->library->scroll_v_real, 32 * data->dpi};
}

static void fill_library_scrollbar(Container *root, Container *scrollbar) {
    auto data = static_cast<RootData *>(root->user_data);
    data->library_scrollbar = scrollbar;
    scrollbar->name = "library-scrollbar";
    scrollbar->z_index = 1;
    scrollbar->when_drag_end_is_click = false;
    scrollbar->when_mouse_enters_container = scrollbar->when_mouse_motion = [](Container *root, Container *) {
        show_library_scrollbar(static_cast<RootData *>(root->user_data));
    };
    scrollbar->when_mouse_leaves_container = [](Container *root, Container *) {
        auto data = static_cast<RootData *>(root->user_data);
        data->artwork_refresh->animating = true;
        poll_artwork(data->artwork_refresh);
    };
    auto update = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (!data->scrollbar_dragging || !c->interactable) return;
        data->album_scroll_start.reset();
        data->last_album_clicked = data->album_double_click_target = nullptr;
        data->library->scroll_v_real = library_scroll_metrics(data).offset_at(
            root->mouse_current_y - c->real_bounds.y, data->scrollbar_grab);
        show_library_scrollbar(data);
        windowing::redraw(data->window->raw_window);
    };
    scrollbar->when_mouse_down = [update](Container *root, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT || !c->interactable) return;
        auto data = static_cast<RootData *>(root->user_data);
        const auto metrics = library_scroll_metrics(data);
        const double y = root->mouse_current_y - c->real_bounds.y;
        // Preserve the grab point on the thumb; track clicks center it at once.
        data->scrollbar_grab = y >= metrics.thumb_top && y <= metrics.thumb_top + metrics.thumb_height
            ? y - metrics.thumb_top : metrics.thumb_height / 2;
        data->scrollbar_dragging = true;
        update(root, c);
    };
    scrollbar->when_drag_start = scrollbar->when_drag = update;
    auto finish = [update](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (!data->scrollbar_dragging) return;
        update(root, c);
        data->scrollbar_dragging = false;
        show_library_scrollbar(data);
    };
    scrollbar->when_clicked = scrollbar->when_drag_end = finish;
    scrollbar->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<RootData *>(root->user_data);
        if (!c->interactable) return;
        const double age = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - data->scrollbar_activity).count();
        const bool held = data->scrollbar_dragging || c->state.mouse_hovering;
        const double alpha = held ? 1 : std::clamp(1 - (age - 900) / 250, 0.0, 1.0);
        if (alpha <= 0) return;
        auto cr = data->window->raw_window->drawing_context;
        const auto &b = c->real_bounds;
        const auto metrics = library_scroll_metrics(data);
        const double thickness = std::min((held ? 5 : 3) * data->dpi, b.w);
        cr->save();
        cr->rectangle(b.x, b.y, b.w, b.h); cr->clip();
        cr->set_color(with_alpha(theme().scrollbar, (held ? .65 : .4) * alpha));
        cr->rounded_rectangle({b.x + (b.w - thickness) / 2, b.y + metrics.thumb_top,
                               thickness, metrics.thumb_height}, thickness / 2);
        cr->fill();
        cr->restore();
        if (!held) {
            data->artwork_refresh->animating = true;
            poll_artwork(data->artwork_refresh);
        }
    };
}

static void fill_root(Container *root) {
    auto root_data = static_cast<RootData *>(root->user_data);
    auto startup = root_data->startup;
    auto playable = load_library(startup ? startup->music_root : std::string(), false);
    sort_tracks(playable);

    auto albums = to_albums(playable);

    root->after_paint = [](Container *root, Container *) {
        const auto progress = player->conversion_progress();
        if (!progress.active && progress.error.empty()) return;
        auto rd = static_cast<RootData *>(root->user_data);
        const double dpi = rd->dpi;
        auto cr = rd->window->raw_window->drawing_context;
        const double width = std::min(360 * dpi, std::max(0.0, root->real_bounds.w - 24 * dpi));
        Bounds b(root->real_bounds.right() - width - 12 * dpi, 12 * dpi, width, 110 * dpi);
        cr->save();
        rounded_rectangle(cr, b, 10 * dpi);
        cr->set_color(theme_colors::progress_surface); cr->fill();
        auto text = [&](double y, const std::string &value, int size) {
            draw_text(cr, b.x + 14 * dpi, b.y + y * dpi, value, size * dpi, true,
                      mylar_font, b.w - 28 * dpi, -1, theme().on_accent, false, 0);
        };
        text(12, progress.active ? "Converting audio to FLAC" : "Conversion failed", 13);
        if (progress.active) {
            text(37, std::filesystem::path(progress.path).filename().string(), 11);
        } else {
            std::size_t start = 0;
            double y = 37;
            do {
                const auto end = progress.error.find('\n', start);
                text(y, progress.error.substr(start, end - start), 11);
                if (end == std::string::npos) break;
                start = end + 1;
                y += 20;
            } while (start < progress.error.size());
        }
        double duration = 0;
        if (auto it = rd->tracks.find(progress.path); it != rd->tracks.end()) {
            try { duration = std::stod(it->second.length); } catch (...) {}
        }
        const double fraction = duration > 0 ? std::clamp(progress.seconds / duration, 0.0, 1.0) : 0;
        if (progress.active) {
            text(61, "Track " + std::to_string(progress.track) + " / " + std::to_string(progress.total) +
                "  ·  " + (duration > 0 ? std::to_string(static_cast<int>(fraction * 100)) + "%" :
                std::to_string(static_cast<int>(progress.seconds)) + " seconds converted"), 11);
            const double overall = progress.total ? (std::max(1ul, progress.track) - 1 + fraction) / progress.total : 0;
            cr->rectangle(b.x + 14 * dpi, b.y + 91 * dpi, (b.w - 28 * dpi) * overall, 4 * dpi);
            cr->set_color(theme_colors::progress_fill); cr->fill();
        }
        cr->restore();
    };
    root->type = ::fullycustom;
    root->receive_events_even_if_obstructed = true;
    root->when_mouse_down = [](Container *root, Container *) {
        auto rd = static_cast<RootData *>(root->user_data);
        if (rd->playlist_edit.id.empty()) return;
        bool inside = false;
        if (rd->expanded_album && rd->album_panel && rd->album_panel->exists && rd->library->interactable &&
            !rd->context_overlay->exists && !rd->queue_overlay->exists) {
            const auto album = static_cast<AlbumData *>(rd->expanded_album->user_data);
            inside = album->playlist_id == rd->playlist_edit.id &&
                bounds_contains(playlist_title_bounds(rd->album_panel->real_bounds, rd->dpi, album->album.songs.size())
                    .intersection(rd->library->real_bounds), root->mouse_current_x, root->mouse_current_y);
        }
        if (!inside) finish_playlist_name_edit(root, true);
    };
    auto library = root->child(FILL_SPACE, FILL_SPACE);
    root_data->library = library;
    library->name = "album-library";
    fill_out_for_albums(library, albums);
    auto bar = root->child(FILL_SPACE, FILL_SPACE);
    fill_playback_bar(root, bar);
    auto scrollbar = root->child(FILL_SPACE, FILL_SPACE);
    fill_library_scrollbar(root, scrollbar);
    root->when_key_event = playback_key_event;
    auto queue_overlay = root->child(FILL_SPACE, FILL_SPACE);
    fill_queue_overlay(root, queue_overlay);
    auto context_overlay = root->child(FILL_SPACE, FILL_SPACE);
    fill_queue_overlay(root, context_overlay, true);
    auto overlay = root->child(FILL_SPACE, FILL_SPACE);
    fill_artwork_preview(root, overlay);
    auto settings_menu = root->child(FILL_SPACE, FILL_SPACE);
    fill_settings_menu(root, settings_menu);
    root->pre_layout = [library, bar, scrollbar, overlay, settings_menu, queue_overlay, context_overlay](Container *root, Container *, const Bounds &b) {
        const double dpi = static_cast<RootData *>(root->user_data)->window->raw_window->dpi;
        const double bar_height = std::min(96 * dpi, b.h);
        layout(root, library, Bounds(b.x, b.y, b.w, std::max(0.0, b.h - bar_height)));
        layout(root, bar, Bounds(b.x, b.bottom() - bar_height, b.w, bar_height));
        auto data = static_cast<RootData *>(root->user_data);
        const auto &viewport = library->real_bounds;
        const double gutter = std::min(14 * dpi, viewport.w);
        const double inset = std::min(6 * dpi, viewport.h / 2);
        layout(root, scrollbar, Bounds(viewport.right() - gutter, viewport.y + inset,
                                      gutter, std::max(0.0, viewport.h - 2 * inset)));
        scrollbar->interactable = library->interactable && !queue_overlay->exists && !context_overlay->exists &&
            data->library_scroll_max > 0 && scrollbar->real_bounds.h > 0;
        if (!scrollbar->interactable) data->scrollbar_dragging = false;
        if (queue_overlay->exists)
            layout(root, queue_overlay, b);
        if (context_overlay->exists)
            layout(root, context_overlay, b);
        if (overlay->exists)
            layout(root, overlay, b);
        if (settings_menu->exists)
            layout(root, settings_menu, b);
    };
}

void open_window(StartupState &startup) {
    dark_theme_enabled = startup.session.dark_theme;
    RawWindowSettings settings;
    settings.name = "Tunes";
    settings.defer_initial_frame = true;
    settings.app_id = "Tunes";
    if (startup.session.window_width > 0 && startup.session.window_height > 0) {
        settings.pos.w = startup.session.window_width;
        settings.pos.h = startup.session.window_height;
    }
    
    auto app = windowing::open_app();
    auto window = open_mylar_window(app, WindowType::NORMAL, settings);
    window->bg_color = theme().background;
    auto resize = window->raw_window->on_resize;
    window->raw_window->on_resize = [&startup, resize](RawWindow *window, int w, int h) {
        if (w > 0 && h > 0 && window->dpi > 0) {
            startup.session.window_width = std::max(1, static_cast<int>(std::lround(w / window->dpi)));
            startup.session.window_height = std::max(1, static_cast<int>(std::lround(h / window->dpi)));
        }
        if (resize)
            resize(window, w, h);
    };
    auto root = window->root;
    auto root_data = new RootData;
    root_data->app = app;
    root_data->window = window;
    root_data->startup = &startup;
    if (const auto saved = startup.session.scroll_offsets.find(startup.music_root);
        saved != startup.session.scroll_offsets.end())
        root_data->initial_scroll_offset = saved->second;
    root->user_data = root_data;
    fill_root(root);
    prepare_session_display(root, startup);
    window->raw_window->first_frame_ready = [root, root_data](RawWindow *rw, int w, int h) {
        if (w <= 0 || h <= 0)
            return false;
        root->real_bounds = Bounds(0, 0, w, h);
        root->wanted_bounds = root->real_bounds;
        layout(root, root, root->real_bounds);
        // The bottom bar waits only for its tiny blurred preview, like the grid.
        const auto display = playback_data(root);
        if (!display->path.empty()) {
            const auto current = playback_queue.current();
            // Once the track is known to have no cover, preload its saved
            // playlist fallback before committing the first frame.
            root_data->current_art = playback_art(root_data, display->path,
                current ? current->playlist_id : std::string{}, 0);
        }
        bool ready = !root_data->current_art || root_data->artwork->preview_ready(root_data->current_art);
        for (auto i = root_data->album_first; i < root_data->album_end; ++i) {
            auto child = root_data->library->children[i];
            if (child->exists && !root_data->artwork->preview_ready(static_cast<AlbumData *>(child->user_data)->art))
                ready = false;
        }
        return ready;
    };
    window->raw_window->on_next_frame = [root, root_data, &startup](RawWindow *rw) {
        root_data->first_frame_shown = true;
        // Upgrade the blurred previews after their first frame is committed.
        layout(root, root, root->real_bounds);
        initialize_playback(startup);
        root_data->playback_initialized = true;
        // Populate other queued tracks after the first frame. The selected
        // track already has display metadata, even outside the cached library.
        for (const auto &path : player->queue()) {
            if (root_data->tracks.contains(path))
                continue;
            const auto track = read_track(path);
            root_data->tracks[path] = {
                track.name.empty() ? std::filesystem::path(path).stem().string() : track.name,
                track.artist, root_data->artwork->create({path}), track.album, track.length};
        }
        sync_playback(root);
        // Explicit mixed file/folder inputs require discovery to build their queue.
        if (startup.session.rescan_on_launch || startup.defer_queue_until_scan)
            start_library_rescan(root);
        poll_playback(root);
        windowing::redraw(rw);
    };
    windowing::redraw(window->raw_window);
    windowing::main_loop(app);
    root_data->playlist_art_chooser.close();
    finish_playlist_art_job(root, true);
    player->pause();
    checkpoint_session(root, true);
    delete root; // Cancels queued artwork work and joins the workers before exit.
    window->root = nullptr;
}

int main(int argc, char **argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: tunes [audio-file | music-folder]...\n";
        return 0;
    }
    try {
        StartupState startup;
        configure_startup(startup, argc, argv);
        auto owned_player = std::make_unique<Player>();
        player = owned_player.get();
        open_window(startup);
        player->stop();
        player = nullptr;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Tunes: " << error.what() << '\n';
        return 1;
    }
}
