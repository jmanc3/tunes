
#include "client/raw_windowing.h"
#include "container.h"
#include "player.h"
#include "client/windowing.h"
#include "utility.h"
#include "audio_data.h"
#include "album_art.h"
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
#include <pango/pango-layout.h>
#include <pango/pango-types.h>
#include <pango/pangocairo.h>
#include <cmath>
#include <gdk/gdk.h>
#include <unordered_map>
#include <optional>


static std::string mylar_font = "SF Pro";
static Player *player = nullptr;

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
    // if (startup.defer_queue_until_scan)
    //     return;
    // Opening a library restores paused playback; explicit files request playback.
    // if (startup.explicit_files) {
    //     const bool loaded = player->restore_session(std::move(startup.queue), 0, 0);
    //     if (!player->queue().empty() && (!loaded || !player->start()))
    //         std::cerr << "Could not start playback: " << player->last_error() << '\n';
    // } else {
    //     remove_missing_tracks(startup.session);
    //     if (!player->restore_session(startup.session.queue, startup.session.current_index, startup.session.seconds))
    //         std::cerr << "Could not restore playback: " << player->last_error() << '\n';
    // }
}

struct ArtRefresh {
    RawApp *app = nullptr;
    RawWindow *window = nullptr;
    std::weak_ptr<AlbumArtCache> cache;
    int timer = -1;
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
        if (cache->take_changed())
            windowing::redraw(refresh->window);
        if (pending)
            poll_artwork(refresh);
    }, nullptr);
}

struct TrackDisplay {
    std::string title;
    std::string artist;
    AlbumArtCache::Handle art;
};

struct LibraryScanResult {
    std::vector<AlbumOption> albums;
    std::vector<std::string> launch_queue;
    bool launch = false;
};

struct RootData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::shared_ptr<AlbumArtCache> artwork;
    std::shared_ptr<ArtRefresh> artwork_refresh;
    std::size_t album_first = 0;
    std::size_t album_end = 0;
    std::unordered_map<std::string, TrackDisplay> tracks;
    Container *playback_bar = nullptr;
    Container *library = nullptr;
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
    AlbumArtCache::Handle preview_art;
    Bounds preview_bounds;
    StartupState *startup = nullptr;
    double dpi = 1;
    bool scroll_restored = false;
    std::chrono::steady_clock::time_point last_session_save;
    std::future<LibraryScanResult> scan;
    bool scan_failed = false;
    ThreadPool scanner{1};
};

static RootData *root_data_for(Container *c) {
    while (c->parent)
        c = c->parent;
    return static_cast<RootData *>(c->user_data);
}

struct CachedFont {
    std::string name;
    int size;
    int used_count;
    bool italic = false;
    PangoWeight weight;
    PangoLayout *layout;
    cairo_t *cr; // Creator
    
    ~CachedFont() { g_object_unref(layout); }
};

static std::vector<CachedFont *> cached_fonts;

static PangoLayout *
get_cached_pango_font(cairo_t *cr, std::string name, int pixel_height, PangoWeight weight, bool italic) {
#ifdef TRACY_ENABLE
    ZoneScoped;
#endif
    // Look for a matching font in the cache (including italic style)
    for (int i = cached_fonts.size() - 1; i >= 0; i--) {
        auto font = cached_fonts[i];
        if (font->name == name &&
            font->size == pixel_height &&
            font->weight == weight &&
            font->cr == cr &&
            font->italic == italic) { // New italic check
            pango_layout_set_attributes(font->layout, nullptr);
            return font->layout;
        }
    }

    // Create a new CachedFont entry
    auto *font = new CachedFont;
    assert(font);
    font->name = name;
    font->size = pixel_height;
    font->weight = weight;
    font->cr = cr;
    font->italic = italic; // Save the italic setting
    font->used_count = 0;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *desc = pango_font_description_new();

    pango_font_description_set_size(desc, pixel_height * PANGO_SCALE);
    pango_font_description_set_family_static(desc, name.c_str());
    pango_font_description_set_weight(desc, weight);
    // Set the style to italic or normal based on the parameter
    pango_font_description_set_style(desc, italic ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL);

    pango_layout_set_font_description(layout, desc);
    pango_font_description_free(desc);
    pango_layout_set_attributes(layout, nullptr);

    assert(layout);

    font->layout = layout;
    //printf("new: %p\n", font->layout);

    cached_fonts.push_back(font);

    assert(font->layout);

    return font->layout;
}

static void cleanup_cached_fonts() {
    for (auto font: cached_fonts)
        delete font;
    cached_fonts.clear();
    cached_fonts.shrink_to_fit();
}

static void remove_cached_fonts(cairo_t *cr) {
    for (int i = cached_fonts.size() - 1; i >= 0; --i) {
        if (cached_fonts[i]->cr == cr) {
            delete cached_fonts[i];
            cached_fonts.erase(cached_fonts.begin() + i);
        }
    }
}

static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color, bool bold, int align = 0) {
    auto layout = get_cached_pango_font(cr, mylar_font, size, bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, false);
    
    //pango_layout_set_text(layout, "\uE7E7", strlen("\uE83F"));
    pango_layout_set_text(layout, text.data(), text.size());
    pango_layout_set_alignment(layout, (PangoAlignment) align);
    if (wrap == -1) {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_NONE);
        pango_layout_set_width(layout, -1);
        pango_layout_set_height(layout, -1);
        pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_NONE);
    } else {
        pango_layout_set_wrap(layout, PangoWrapMode::PANGO_WRAP_WORD_CHAR);
        pango_layout_set_width(layout, wrap * PANGO_SCALE);
        pango_layout_set_height(layout, h);
        if (h != -1)
            pango_layout_set_ellipsize(layout, PangoEllipsizeMode::PANGO_ELLIPSIZE_MIDDLE);
    }
    set_argb(cr, color);
    PangoRectangle ink;
    PangoRectangle logical;
    pango_layout_get_pixel_extents(layout, &ink, &logical);
    if (draw) {
        cairo_move_to(cr, std::round(x), std::round(y));
        pango_cairo_show_layout(cr, layout);
    }
    return Bounds(ink.width, ink.height, logical.width, logical.height);
}

static void paint_button_bg(Container *root, Container *c) {
    auto root_data = (RootData *) root->user_data;
    auto dpi = root_data->window->raw_window->dpi;
    auto cr = root_data->window->raw_window->cr;
    
    if (c->state.mouse_pressing) {
        cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_set_source_rgba(cr, 0, 0, 0, .4);
        cairo_fill(cr);
    } else if (c->state.mouse_hovering) {
        cairo_rectangle(cr, c->real_bounds.x, c->real_bounds.y, c->real_bounds.w, c->real_bounds.h);
        cairo_set_source_rgba(cr, 0, 0, 0, .2);
        cairo_fill(cr);
    }
}

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

struct AlbumData : UserData {
    AlbumOption album;
    std::string name;
    std::string artist;
    AlbumArtCache::Handle art;
};

static void add_album(Container *parent, const AlbumOption &option, AlbumArtCache::Handle existing_art = {}) {
    if (option.songs.empty())
        return;
    auto data = new AlbumData;
    data->album = option;
    std::vector<std::string> tracks;
    tracks.reserve(option.songs.size());
    for (const auto &song : option.songs)
        tracks.push_back(song.full);
    auto root_data = root_data_for(parent);
    data->art = existing_art ? std::move(existing_art) : root_data->artwork->create(std::move(tracks));
    for (const auto &song : option.songs)
        root_data->tracks[song.full] = {
            song.name.empty() ? std::filesystem::path(song.full).stem().string() : song.name,
            song.artist, data->art};
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

    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    c->user_data = data;
    c->exists = false;
    c->when_paint = [](Container *root, Container *c) {
        auto data = static_cast<AlbumData *>(c->user_data);
        auto window = static_cast<RootData *>(root->user_data)->window->raw_window;
        auto cr = window->cr;
        const double dpi = window->dpi;
        if (c->real_bounds.intersection(c->parent->real_bounds).empty())
            return;
        const auto art = static_cast<RootData *>(root->user_data)->artwork->image(data->art);
        // AlbumTexture *art = nullptr;

        cairo_save(cr);
        set_rect(cr, c->parent->real_bounds);
        cairo_clip(cr);
        set_rect(cr, c->real_bounds);
        cairo_clip(cr);
        paint_button_bg(root, c);
        const double pad = 8 * dpi;
        const double x = c->real_bounds.x + pad;
        const double y = c->real_bounds.y + pad;
        const double size = std::max(0.0, c->real_bounds.w - 2 * pad);
        cairo_rectangle(cr, x, y, size, size);
        cairo_set_source_rgb(cr, .9, .91, .93);
        cairo_fill(cr);
        if (art && size > 0) {
            const int width = art->width;
            const int height = art->height;
            const double scale = std::min(size / width, size / height);
            cairo_save(cr);
            cairo_translate(cr, x + (size - width * scale) / 2, y + (size - height * scale) / 2);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, art->surface, 0, 0);
            cairo_paint(cr);
            cairo_restore(cr);
        } else {
            draw_text(cr, x, y + size / 2 - 12 * dpi, "♫", 24 * dpi, true,
                      mylar_font, size, -1, RGBA(.45, .47, .5, 1), false, PANGO_ALIGN_CENTER);
        }
        draw_text(cr, x, y + size + 8 * dpi, data->name, 12 * dpi, true,
                  mylar_font, size, 20 * dpi * PANGO_SCALE, RGBA(0, 0, 0, 1), true);
        draw_text(cr, x, y + size + 30 * dpi, data->artist, 10 * dpi, true,
                  mylar_font, size, 18 * dpi * PANGO_SCALE, RGBA(.4, .4, .4, 1), false);
        cairo_restore(cr);
    };
    c->when_clicked = [](Container *, Container *c) {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;
        auto data = static_cast<AlbumData *>(c->user_data);
        auto &queue = player->queue();
        queue.clear();
        for (const auto &song : data->album.songs)
            queue.push_back(song.full);
        if (!player->play_queued_item(0))
            std::cerr << "Album playback failed: " << player->last_error() << '\n';
    };

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
        auto cr = root_data->window->raw_window->cr;
        paint_button_bg(root, c);
//static Bounds draw_text(cairo_t *cr, int x, int y, std::string text, int size, bool draw, std::string font, int wrap, int h, RGBA color, bool bold, int align = 0) {
        auto b = draw_text(cr, 0, 0, option_data->name, 12 * dpi, false, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false, 0);
        draw_text(cr, 10, center_y(c, b.h), option_data->name, 12 * dpi, true, mylar_font, -1, -1, RGBA(0, 0, 0, 1), false, 0);
    };
    c->when_clicked = [](Container *root, Container *c) {
        auto option_data = (OptionData *) c->user_data;
        auto btn = c->state.mouse_button_pressed;
        // printf("here\n");
        if (btn == BTN_LEFT) {
            player->play_track(option_data->full_path);
            player->start();
            // printf(fz("{}\n", option_data->full_path).c_str());
        } else if (btn == BTN_RIGHT) {
            printf("%s queued\n", option_data->full_path.c_str());
            player->queue().push_back(option_data->full_path);
            player->queue_changed();
        }
    };
}

static void fill_out_for_songs(Container *root, const std::vector<Option> &playable) {
    root->type = ::fullycustom;
    root->when_paint = [](Container *root, Container *c) {
        auto root_data = (RootData *) root->user_data;
        auto cr = root_data->window->raw_window->cr;
        auto b = c->real_bounds;
        set_rect(cr, b); 
        set_argb(cr, RGBA(1, 1, 1, 1));
        cairo_fill(cr);
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
        auto cr = static_cast<RootData *>(root->user_data)->window->raw_window->cr;
        set_rect(cr, c->real_bounds);
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_fill(cr);
        const auto data = static_cast<RootData *>(root->user_data);
        for (auto i = data->album_first; i < data->album_end; ++i) {
            auto child = c->children[i];
            if (child->exists)
                child->when_paint(root, child);
        }
    };
    root->when_fine_scrolled = [](Container *, Container *c, double, double scroll_y, bool) {
        c->scroll_v_real += 2 * scroll_y;
    };
    root->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto data = static_cast<RootData *>(root->user_data);
        const double dpi = data->window->raw_window->dpi;
        data->dpi = dpi;
        if (!data->scroll_restored && data->startup && !c->children.empty() && b.w > 0 && b.h > 0) {
            const auto &offsets = data->startup->session.scroll_offsets;
            const auto saved = offsets.find(data->startup->music_root);
            if (saved != offsets.end())
                c->scroll_v_real = saved->second * dpi;
            data->scroll_restored = true;
        }
        const double pad = std::min(16 * dpi, std::max(0.0, b.w / 2));
        const double gap = 16 * dpi;
        const double width = std::max(0.0, b.w - 2 * pad);
        const double card_w = std::min(192 * dpi, width);
        const double card_h = card_w + 56 * dpi;
        const auto columns = std::max<std::size_t>(1, std::floor((width + gap) / (card_w + gap)));
        // Share remaining width across the outer margins and every column gap.
        const double column_gap = std::max(0.0, b.w - columns * card_w) / (columns + 1);
        const auto rows = (c->children.size() + columns - 1) / columns;
        const double content_h = rows ? 2 * pad + rows * card_h + (rows - 1) * gap : 0;
        c->scroll_v_real = std::clamp(c->scroll_v_real, std::min(0.0, b.h - content_h), 0.0);
        // Lay out and preload the viewport plus one row on either side.
        // Paint and image requests now scale with visible cards, not library size.
        const double row_h = card_h + gap;
        const auto first_row = static_cast<std::size_t>(std::max(0.0,
            std::floor((-c->scroll_v_real - pad) / row_h) - 1));
        const auto end_row = static_cast<std::size_t>(std::max(0.0,
            std::ceil((b.h - c->scroll_v_real - pad) / row_h) + 1));
        const auto first = std::min(c->children.size(), first_row * columns);
        const auto end = std::min(c->children.size(), end_row * columns);
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
                b.y + pad + (i / columns) * row_h + c->scroll_v_real, card_w, card_h));
            child->exists = !child->real_bounds.intersection(b).empty();
            auto art = static_cast<AlbumData *>(child->user_data)->art;
            if (!child->exists)
                data->artwork->release(art);
            data->artwork->request(art, child->exists ? std::max(1, static_cast<int>(std::ceil(card_w - 16 * dpi))) : 0);
        }
        if (data->artwork->pending() || data->artwork->take_changed())
            poll_artwork(data->artwork_refresh);
    };
    for (const auto &album : albums)
        add_album(root, album);
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

static void checkpoint_session(Container *root, bool force = false) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!data->startup)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - data->last_session_save < std::chrono::seconds(2))
        return;
    data->last_session_save = now;
    auto &startup = *data->startup;
    auto state = startup.session;
    const auto position = player->playback_position();
    state.music_root = startup.music_root;
    state.queue = player->queue();
    state.current_path = position.path;
    state.current_index = position.index;
    state.seconds = position.seconds;
    state.volume = player->volume();
    state.sample_rate = player->sample_rate();
    state.unmuted_volume = playback_data(root)->unmuted_gain;
    if (data->scroll_restored && data->library)
        state.scroll_offsets[state.music_root] = data->library->scroll_v_real / data->dpi;
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

static bool sync_playback(Container *root) {
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
    const bool changed = data->path != path || data->playing != playing || data->gain != gain ||
        static_cast<int>(data->elapsed) != static_cast<int>(elapsed) || data->duration != duration ||
        std::abs(data->position - position) * data->seek->real_bounds.w >= .5 ||
        data->play->interactable != can_play || data->previous->interactable != can_previous ||
        data->next->interactable != can_next;
    if (data->path != path) {
        data->seeking = false;
        auto root_data = static_cast<RootData *>(root->user_data);
        if (root_data->artwork) {
            if (root_data->current_art)
                root_data->artwork->release(root_data->current_art);
            root_data->current_art.reset();
            if (!path.empty()) {
                const auto track = root_data->tracks.find(path);
                root_data->current_art = track != root_data->tracks.end()
                    ? root_data->artwork->clone(track->second.art) : root_data->artwork->create({path});
                root_data->artwork->request(root_data->current_art, 128);
                poll_artwork(root_data->artwork_refresh);
            }
        }
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
    if (!data->scan.valid() || data->scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
    try {
        auto result = data->scan.get();
        // The worker publishes a complete snapshot. All container and artwork
        // mutations happen together on the event thread, between frames.
        auto library = data->library;
        std::map<std::vector<std::string>, AlbumArtCache::Handle> retained_art;
        for (auto child : library->children) {
            auto album = static_cast<AlbumData *>(child->user_data);
            std::vector<std::string> paths;
            for (const auto &song : album->album.songs)
                paths.push_back(song.full);
            retained_art.emplace(std::move(paths), album->art);
            delete child;
        }
        library->children.clear();
        data->album_first = data->album_end = 0;
        // Keep metadata for queued tracks even if they are outside the library.
        std::erase_if(data->tracks, [](const auto &entry) {
            const auto &queue = player->queue();
            return std::find(queue.begin(), queue.end(), entry.first) == queue.end();
        });
        for (const auto &album : result.albums) {
            std::vector<std::string> paths;
            for (const auto &song : album.songs)
                paths.push_back(song.full);
            auto previous = retained_art.find(paths);
            add_album(library, album, previous != retained_art.end() ? previous->second : AlbumArtCache::Handle{});
            if (previous != retained_art.end())
                retained_art.erase(previous);
        }
        for (const auto &[paths, art] : retained_art)
            data->artwork->release(art);
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
        finish_library_rescan(root);
        auto data = static_cast<RootData *>(root->user_data);
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
    auto cr = window->cr;
    const double scale = std::min(c->real_bounds.w, c->real_bounds.h) / 32;
    const bool active = c->interactable;
    cairo_save(cr);
    cairo_translate(cr, c->real_bounds.x + c->real_bounds.w / 2, c->real_bounds.y + c->real_bounds.h / 2);
    cairo_scale(cr, scale, scale);
    if (!active)
        cairo_set_source_rgb(cr, .66, .73, .77);
    else if (c->state.mouse_hovering)
        cairo_set_source_rgb(cr, .02, .52, .68);
    else
        cairo_set_source_rgb(cr, .24, .34, .40);
    if (button == PlaybackButton::Play) {
        if (!active)
            cairo_set_source_rgb(cr, .76, .84, .88);
        else if (c->state.mouse_pressing)
            cairo_set_source_rgb(cr, .02, .43, .58);
        else if (c->state.mouse_hovering)
            cairo_set_source_rgb(cr, .02, .56, .73);
        else
            cairo_set_source_rgb(cr, .04, .62, .79);
        cairo_arc(cr, 0, 0, 16, 0, 2 * M_PI);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1, 1, 1);
        if (data->playing) {
            cairo_rectangle(cr, -5, -6, 3, 12);
            cairo_rectangle(cr, 2, -6, 3, 12);
        } else {
            cairo_move_to(cr, -4, -7);
            cairo_line_to(cr, 7, 0);
            cairo_line_to(cr, -4, 7);
            cairo_close_path(cr);
        }
        cairo_fill(cr);
    } else if (button == PlaybackButton::Previous || button == PlaybackButton::Next) {
        if (button == PlaybackButton::Next)
            cairo_scale(cr, -1, 1);
        cairo_rectangle(cr, -8, -6, 2.5, 12);
        cairo_move_to(cr, -5, 0);
        cairo_line_to(cr, 6, -7);
        cairo_line_to(cr, 6, 7);
        cairo_close_path(cr);
        cairo_fill(cr);
    } else {
        cairo_move_to(cr, -10, -3);
        cairo_line_to(cr, -6, -3);
        cairo_line_to(cr, -1, -7);
        cairo_line_to(cr, -1, 7);
        cairo_line_to(cr, -6, 3);
        cairo_line_to(cr, -10, 3);
        cairo_close_path(cr);
        cairo_fill(cr);
        cairo_set_line_width(cr, 1.6);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        if (data->gain == 0) {
            cairo_move_to(cr, 4, -3);
            cairo_line_to(cr, 10, 3);
            cairo_move_to(cr, 10, -3);
            cairo_line_to(cr, 4, 3);
        } else {
            cairo_arc(cr, -1, 0, 7, -M_PI / 4, M_PI / 4);
            if (data->gain > .5f) {
                cairo_new_sub_path(cr);
                cairo_arc(cr, -1, 0, 11, -M_PI / 4, M_PI / 4);
            }
        }
        cairo_stroke(cr);
    }
    cairo_restore(cr);
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
                               xkb_keysym_t sym, int mods, bool, std::string) {
    if (!pressed || (mods & (MOD_CTRL | MOD_ALT | MOD_SUPER)))
        return;
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
        auto cr = window->cr;
        auto data = playback_data(root);
        const double dpi = window->dpi;
        const double inset = std::min(6 * dpi, c->real_bounds.w / 2);
        const double x = c->real_bounds.x + inset;
        const double width = std::max(0.0, c->real_bounds.w - 2 * inset);
        const double y = c->real_bounds.y + c->real_bounds.h / 2;
        const double value = volume ? data->gain : data->seeking ? data->seek_preview : data->position;
        const bool highlight = c->interactable && (c->state.mouse_hovering || c->state.mouse_pressing);
        cairo_save(cr);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_set_line_width(cr, 4 * dpi);
        cairo_set_source_rgb(cr, .80, .86, .89);
        cairo_move_to(cr, x, y);
        cairo_line_to(cr, x + width, y);
        cairo_stroke(cr);
        if (highlight)
            cairo_set_source_rgb(cr, .02, .52, .68);
        else
            cairo_set_source_rgb(cr, .04, .62, .79);
        if (value > 0 && c->interactable) {
            cairo_move_to(cr, x, y);
            cairo_line_to(cr, x + width * value, y);
            cairo_stroke(cr);
        }
        if (highlight) {
            cairo_set_source_rgb(cr, .02, .52, .68);
            cairo_arc(cr, x + width * value, y, 5 * dpi, 0, 2 * M_PI);
            cairo_fill(cr);
        }
        cairo_restore(cr);
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
    data->artwork_preview->exists = false;
    data->library->interactable = true;
    data->playback_bar->interactable = true;
    playback_changed(root);
}

static void open_artwork_preview(Container *root) {
    auto data = static_cast<RootData *>(root->user_data);
    if (!data->current_art || !data->artwork_preview)
        return;
    data->preview_art = data->artwork->create_preview(data->current_art);
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
        auto cr = window->cr;
        const auto &b = c->real_bounds;
        const double inset = b.w * .32;
        cairo_save(cr);
        cairo_set_source_rgba(cr, 1, 1, 1, c->state.mouse_hovering ? 1 : .7);
        cairo_set_line_width(cr, 2 * window->dpi);
        cairo_move_to(cr, b.x + inset, b.y + inset);
        cairo_line_to(cr, b.right() - inset, b.bottom() - inset);
        cairo_move_to(cr, b.right() - inset, b.y + inset);
        cairo_line_to(cr, b.x + inset, b.bottom() - inset);
        cairo_stroke(cr);
        cairo_restore(cr);
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
        auto cr = window->cr;
        const auto &b = c->real_bounds;
        cairo_save(cr);
        set_rect(cr, b);
        cairo_set_source_rgba(cr, 0, 0, 0, .82);
        cairo_fill(cr);
        const auto image = data->preview_art ? data->artwork->image(data->preview_art) : nullptr;
        data->preview_bounds = {};
        if (image) {
            const double padding = std::min({48.0 * window->dpi, b.w / 4, b.h / 4});
            const double scale = std::min((b.w - 2 * padding) / image->width, (b.h - 2 * padding) / image->height);
            const double width = image->width * scale, height = image->height * scale;
            data->preview_bounds = Bounds(b.x + (b.w - width) / 2, b.y + (b.h - height) / 2, width, height);
            cairo_translate(cr, data->preview_bounds.x, data->preview_bounds.y);
            cairo_scale(cr, scale, scale);
            cairo_set_source_surface(cr, image->surface, 0, 0);
            cairo_paint(cr);
        } else {
            draw_text(cr, b.x, b.y + b.h / 2, data->artwork->pending() ? "Loading artwork…" : "No artwork available",
                      14 * window->dpi, true, mylar_font, b.w, -1, RGBA(1, 1, 1, 1), false, PANGO_ALIGN_CENTER);
        }
        cairo_restore(cr);
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
        auto cr = window->cr;
        const auto &b = c->real_bounds;
        cairo_save(cr);
        cairo_translate(cr, b.x + b.w / 2, b.y + b.h / 2);
        const double scale = std::min(b.w, b.h) / 32;
        cairo_scale(cr, scale, scale);
        if (c->state.mouse_hovering || c->state.mouse_pressing)
            cairo_set_source_rgb(cr, .02, .52, .68);
        else
            cairo_set_source_rgb(cr, .24, .34, .40);
        for (int i = 0; i < 48; ++i) {
            const double angle = i * 2 * M_PI / 48;
            const double radius = (i % 6 == 1 || i % 6 == 2) ? 10 : 8;
            const double x = std::cos(angle) * radius, y = std::sin(angle) * radius;
            if (i == 0) cairo_move_to(cr, x, y);
            else cairo_line_to(cr, x, y);
        }
        cairo_close_path(cr);
        cairo_new_sub_path(cr);
        cairo_arc(cr, 0, 0, 3.5, 0, 2 * M_PI);
        cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
        cairo_fill(cr);
        cairo_restore(cr);
    };
    data->art_button = bar->child(FILL_SPACE, FILL_SPACE);
    data->art_button->name = "preview-current-artwork";
    data->art_button->when_clicked = [](Container *root, Container *c) {
        if (c->state.mouse_button_pressed == BTN_LEFT)
            open_artwork_preview(root);
    };
    if (auto startup = static_cast<RootData *>(root->user_data)->startup)
        data->unmuted_gain = startup->session.unmuted_volume;
    bar->pre_layout = [](Container *root, Container *bar, const Bounds &b) {
        auto data = static_cast<PlaybackData *>(bar->user_data);
        const double dpi = static_cast<RootData *>(root->user_data)->window->raw_window->dpi;
        const double pad = std::min(16 * dpi, b.w / 8);
        const bool compact = b.w < 1000 * dpi;
        const double right_width = std::min(240 * dpi, b.w * .42);
        const double center = compact ? b.x + (b.w - right_width) / 2 : b.x + b.w / 2;
        const double step = std::min(44 * dpi, (b.w - right_width - 2 * pad) / 3);
        const double button = std::max(0.0, std::min(32 * dpi, step));
        const double button_y = b.y + 20 * dpi;
        layout(root, data->previous, Bounds(center - step - button / 2, button_y, button, button));
        layout(root, data->play, Bounds(center - button / 2, button_y, button, button));
        layout(root, data->next, Bounds(center + step - button / 2, button_y, button, button));
        const double mute_size = std::min(32 * dpi, right_width / 3);
        const double volume_y = b.y + (b.h - mute_size) / 2;
        layout(root, data->mute, Bounds(b.right() - right_width, volume_y, mute_size, mute_size));
        layout(root, data->volume, Bounds(b.right() - right_width + mute_size, volume_y,
            std::max(0.0, right_width - 2 * mute_size - pad), mute_size));
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
        auto cr = window->cr;
        sync_playback(root);
        cairo_save(cr);
        set_rect(cr, bar->real_bounds);
        cairo_set_source_rgb(cr, .96, .98, .99);
        cairo_fill(cr);
        cairo_rectangle(cr, bar->real_bounds.x, bar->real_bounds.y, bar->real_bounds.w, dpi);
        cairo_set_source_rgb(cr, .82, .88, .91);
        cairo_fill(cr);
        auto text = [&](const Bounds &b, const std::string &value, int size, RGBA color, bool bold, int align) {
            if (b.w > 0)
                draw_text(cr, b.x, b.y, value, size * dpi, true, mylar_font, b.w,
                          b.h * PANGO_SCALE, color, bold, align);
        };
        const double elapsed = data->seeking ? data->seek_preview * data->duration : data->elapsed;
        text(data->elapsed_bounds, seconds_to_mmss(std::max(0, static_cast<int>(elapsed))), 9,
             RGBA(.38, .47, .53, 1), false, PANGO_ALIGN_CENTER);
        text(data->duration_bounds, seconds_to_mmss(std::max(0, static_cast<int>(data->duration))), 9,
             RGBA(.38, .47, .53, 1), false, PANGO_ALIGN_CENTER);
        if (!data->info_bounds.empty()) {
            const auto &b = data->info_bounds;
            auto track = root_data->tracks.find(data->path);
            auto art = root_data->current_art ? root_data->artwork->image(root_data->current_art) : nullptr;
            const double size = b.h;
            cairo_rectangle(cr, b.x, b.y, size, size);
            cairo_set_source_rgb(cr, .87, .93, .96);
            cairo_fill(cr);
            if (art) {
                const double scale = std::min(size / art->width, size / art->height);
                cairo_save(cr);
                cairo_translate(cr, b.x + (size - art->width * scale) / 2, b.y + (size - art->height * scale) / 2);
                cairo_scale(cr, scale, scale);
                cairo_set_source_surface(cr, art->surface, 0, 0);
                cairo_paint(cr);
                cairo_restore(cr);
            } else {
                text(Bounds(b.x, b.y + 14 * dpi, size, 30 * dpi), "♫", 18,
                     RGBA(.20, .52, .64, 1), false, PANGO_ALIGN_CENTER);
            }
            const double text_x = b.x + size + 12 * dpi;
            const double text_w = std::max(0.0, b.right() - text_x);
            const std::string title = data->path.empty() ? "Choose an album" : track != root_data->tracks.end()
                ? track->second.title : std::filesystem::path(data->path).stem().string();
            text(Bounds(text_x, b.y + 7 * dpi, text_w, 22 * dpi), title, 10,
                 RGBA(.12, .22, .29, 1), true, PANGO_ALIGN_LEFT);
            const std::string artist = track != root_data->tracks.end() ? track->second.artist : "";
            text(Bounds(text_x, b.y + 31 * dpi, text_w, 18 * dpi), artist, 9,
                 RGBA(.38, .47, .53, 1), false, PANGO_ALIGN_LEFT);
        }
        cairo_restore(cr);
    };
    sync_playback(root);
    poll_playback(root);
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
    const double height = data->settings_information ? 644 : 440 + settings_extra_height(root);
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
        auto cr = data->window->raw_window->cr;
        const double dpi = settings_scale(root);
        const auto &b = data->settings_bounds;
        cairo_save(cr);
        set_rect(cr, c->real_bounds);
        cairo_set_source_rgba(cr, .05, .12, .17, .48);
        cairo_fill(cr);
        set_rect(cr, b);
        cairo_set_source_rgb(cr, .97, .99, 1);
        cairo_fill(cr);
        auto text = [&](double y, const std::string &label, int size, RGBA color, bool bold = false) {
            draw_text(cr, b.x + 28 * dpi, b.y + y * dpi, label, size * dpi, true,
                      mylar_font, b.w - 56 * dpi, -1, color, bold, PANGO_ALIGN_LEFT);
        };
        if (data->settings_information) {
            const RGBA body(.38, .47, .53, 1);
            const RGBA heading(.12, .22, .29, 1);
            // Explicit lines keep the instructions and command fully visible without wrapping.
            auto line = [&](double y, const std::string &label) {
                draw_text(cr, b.x + 28 * dpi, b.y + y * dpi, label, 10 * dpi, true,
                          mylar_font, -1, -1, body, false, PANGO_ALIGN_LEFT);
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
                     10, data->pipewire_error ? RGBA(.65, .16, .12, 1) : RGBA(.02, .39, .53, 1));
        } else {
            text(24, "Settings", 22, RGBA(.12, .22, .29, 1), true);
            text(87, "Output sample rate", 12, RGBA(.12, .22, .29, 1), true);
            text(112, "Sets the playback rate if supported.", 10, RGBA(.38, .47, .53, 1));
            text(248 + settings_extra_height(root), data->settings_error.empty()
                 ? "Current output: " + std::to_string(player->sample_rate()) + " Hz" : data->settings_error,
                 10, data->settings_error.empty() ? RGBA(.02, .39, .53, 1) : RGBA(.65, .16, .12, 1));
            text(302 + settings_extra_height(root), "Library", 12, RGBA(.12, .22, .29, 1), true);
        }
        cairo_restore(cr);
    };
    auto make_button = [&](const char *name, std::function<std::string()> label, unsigned rate = 0) {
        auto item = overlay->child(FILL_SPACE, FILL_SPACE);
        item->name = name;
        item->z_index = 1;
        item->when_paint = [label, rate](Container *root, Container *c) {
            auto cr = static_cast<RootData *>(root->user_data)->window->raw_window->cr;
            const auto &b = c->real_bounds;
            const double dpi = settings_scale(root);
            const bool selected = rate != 0 && player->sample_rate() == rate;
            cairo_save(cr);
            set_rect(cr, b);
            if (selected) cairo_set_source_rgb(cr, .04, .62, .79);
            else if (c->interactable && c->state.mouse_hovering) cairo_set_source_rgb(cr, .78, .91, .96);
            else cairo_set_source_rgb(cr, .87, .94, .97);
            cairo_fill(cr);
            draw_text(cr, b.x, b.y + 10 * dpi, label(), 10 * dpi, true,
                      mylar_font, b.w, -1, selected ? RGBA(1, 1, 1, 1) :
                      c->interactable ? RGBA(.02, .39, .53, 1) : RGBA(.45, .52, .56, 1),
                      selected, PANGO_ALIGN_CENTER);
            cairo_restore(cr);
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
    overlay->pre_layout = [close, information, rates, rescan, force_clock, auto_clock, rate_config](Container *root, Container *, const Bounds &b) {
        auto data = static_cast<RootData *>(root->user_data);
        const double dpi = settings_scale(root);
        const double extra = settings_extra_height(root);
        const bool details = data->settings_information;
        const double width = (details ? 800 : 640) * dpi;
        const double height = (details ? 604 : 400 + extra) * dpi;
        const Bounds panel(b.x + (b.w - width) / 2, b.y + (b.h - height) / 2, width, height);
        data->settings_bounds = panel;
        layout(root, close, Bounds(panel.right() - 108 * dpi, panel.y + 24 * dpi, 80 * dpi, 36 * dpi));
        information->exists = rescan->exists = !details;
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
        }
    };
}

static void fill_root(Container *root) {
    auto root_data = static_cast<RootData *>(root->user_data);
    auto startup = root_data->startup;
    auto playable = load_library(startup ? startup->music_root : std::string(), false);
    sort_tracks(playable);

    auto albums = to_albums(playable);

    root->type = ::fullycustom;
    auto library = root->child(FILL_SPACE, FILL_SPACE);
    root_data->library = library;
    library->name = "album-library";
    fill_out_for_albums(library, albums);
    // A restored queue or an explicitly opened file may be outside this library.
    // Read its metadata during startup, never from the paint callback.
    for (const auto &path : player->queue()) {
        if (root_data->tracks.contains(path))
            continue;
        const auto track = read_track(path);
        root_data->tracks[path] = {track.name.empty() ? std::filesystem::path(path).stem().string() : track.name,
                                  track.artist, root_data->artwork->create({path})};
    }
    auto bar = root->child(FILL_SPACE, FILL_SPACE);
    fill_playback_bar(root, bar);
    root->when_key_event = playback_key_event;
    auto overlay = root->child(FILL_SPACE, FILL_SPACE);
    fill_artwork_preview(root, overlay);
    auto settings_menu = root->child(FILL_SPACE, FILL_SPACE);
    fill_settings_menu(root, settings_menu);
    root->pre_layout = [library, bar, overlay, settings_menu](Container *root, Container *, const Bounds &b) {
        const double dpi = static_cast<RootData *>(root->user_data)->window->raw_window->dpi;
        const double bar_height = std::min(96 * dpi, b.h);
        layout(root, library, Bounds(b.x, b.y, b.w, std::max(0.0, b.h - bar_height)));
        layout(root, bar, Bounds(b.x, b.bottom() - bar_height, b.w, bar_height));
        if (overlay->exists)
            layout(root, overlay, b);
        if (settings_menu->exists)
            layout(root, settings_menu, b);
    };
}

void open_window(StartupState &startup) {
    RawWindowSettings settings;
    settings.name = "Tunes";
    settings.app_id = "Tunes";
    if (startup.session.window_width > 0 && startup.session.window_height > 0) {
        settings.pos.w = startup.session.window_width;
        settings.pos.h = startup.session.window_height;
    }
    
    auto app = windowing::open_app();
    auto window = open_mylar_window(app, WindowType::NORMAL, settings);
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
    root->user_data = root_data;
    fill_root(root);
    start_library_rescan(root);
    windowing::main_loop(app);
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
        initialize_playback(startup);
        open_window(startup);
        player->stop();
        cleanup_cached_fonts();
        player = nullptr;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Tunes: " << error.what() << '\n';
        return 1;
    }
}
