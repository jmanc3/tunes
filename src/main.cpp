
#include "client/raw_windowing.h"
#include "container.h"
#include "player.h"
#include "client/windowing.h"
#include "utility.h"
#include "audio_data.h"
#include "album_art.h"

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


static std::string mylar_font = "Segoe UI";
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

struct RootData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
    std::shared_ptr<AlbumArtCache> artwork;
    std::shared_ptr<ArtRefresh> artwork_refresh;
    std::size_t album_first = 0;
    std::size_t album_end = 0;
};

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

static void add_album(Container *parent, const AlbumOption &option) {
    if (option.songs.empty())
        return;
    auto data = new AlbumData;
    data->album = option;
    std::vector<std::string> tracks;
    tracks.reserve(option.songs.size());
    for (const auto &song : option.songs)
        tracks.push_back(song.full);
    data->art = static_cast<RootData *>(parent->user_data)->artwork->create(std::move(tracks));
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
        if (c->real_bounds.intersection(root->real_bounds).empty())
            return;
        const auto art = static_cast<RootData *>(root->user_data)->artwork->image(data->art);

        cairo_save(cr);
        set_rect(cr, root->real_bounds);
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
    auto data = static_cast<RootData *>(root->user_data);
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

static void fill_root(Container *root) {

    auto playable = load_library();
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

    auto albums = to_albums(playable);
    fill_out_for_albums(root, albums);
}

void open_window() {
    RawWindowSettings settings;
    settings.name = "Tunes";
    settings.app_id = "Tunes";
    
    auto app = windowing::open_app();
    auto window = open_mylar_window(app, WindowType::NORMAL, settings);
    auto root = window->root;
    auto root_data = new RootData;
    root_data->app = app;
    root_data->window = window;
    root->user_data = root_data;
    fill_root(root);
    
    windowing::main_loop(app);
    delete root; // Cancels queued artwork work and joins the workers before exit.
    window->root = nullptr;
}

int main() {
    player = new Player;
    
    open_window();

    player->stop();
    cleanup_cached_fonts();
    
    return 0;
}
