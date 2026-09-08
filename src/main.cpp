
#include "client/raw_windowing.h"
#include "container.h"
#include "player.h"
#include "client/windowing.h"
#include "utility.h"

#include <chrono>
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

struct RootData : UserData {
    RawApp *app = nullptr;
    MylarWindow *window = nullptr;
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

static void add_option(Container *parent, std::filesystem::path file) {
    auto c = parent->child(FILL_SPACE, FILL_SPACE);
    struct OptionData : UserData {
        std::string name;
        std::string full_path;
    };
    auto option_data = new OptionData;
    option_data->name = file.filename();
    option_data->full_path = file.string();
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

static void fill_root(Container *root) {
    namespace fs = std::filesystem;

    root->type = ::vbox;
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
    static float yoff = 0;
    root->when_fine_scrolled = [](Container *root, Container *container, int scroll_x, int scroll_y, bool came_from_touchpad) {
        yoff += scroll_y;
        yoff += scroll_y;
    };
    root->pre_layout = [](Container *root, Container *c, const Bounds &b) {
        auto root_data = (RootData*) root->user_data;
        float screen_h = b.h;

        c->type = ::vbox;
        layout(root, c, b);
        c->type = ::fullycustom;

        float content_h = actual_true_height(c);

        // yoff is 0 at the top and negative while scrolling down.
        // If content fits on screen, don't allow scrolling at all.
        float min_yoff = std::min(0.0f, screen_h - content_h);
        yoff = std::clamp(yoff, min_yoff, 0.0f);

        for (auto child : c->children) {
            modify_all(child, 0, yoff);
        }

        c->real_bounds = b;
        c->wanted_bounds = b;
    };
    
    const char* home = std::getenv("HOME");
    if (!home) {
        throw std::runtime_error("HOME environment variable not set");
    }

    // Target path
    std::filesystem::path path = std::filesystem::path(home) / "Music";

    struct Possible {
        fs::path path;
        bool is_audio = false;
    };

    std::vector<Possible> possible_files;
    for (auto const& entry : fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied))
        if (entry.is_regular_file())
            possible_files.push_back({entry, false});

    for (auto& file : possible_files)
        file.is_audio = is_audio(file.path);

    for (auto& file : possible_files) {
        if (file.is_audio) {
            //std::cout << file.path.string() << std::endl;
            add_option(root, file.path);
        }
    }
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
}

int main() {
    player = new Player;
    
    open_window();

    player->stop();
    cleanup_cached_fonts();
    
    return 0;
    /*

    // Queue an album.
    player.queue() = {
        "/home/jmanc3/speak.flac",
        "/home/jmanc3/breathe.flac",
    };

    // Start playing song_01.
    if (!player.start()) {
        std::cerr << "Failed to start: "
                  << player.last_error() << '\n';
        return 1;
    }

    player.set_volume(0.8f);

    std::cout << "Commands:\n"
              << "  p          pause/resume\n"
              << "  s          stop\n"
              << "  r          restart current song\n"
              << "  h          seek halfway\n"
              << "  f          force-play single\n"
              << "  0-9        play queued item\n"
              << "  q          quit\n";

    bool paused = false;

    while (true) {
        std::cout
            << "\nPlaying: " << player.current_path()
            << "\nTime: "
            << player.current_time_seconds()
            << " / "
            << player.total_time_seconds()
            << "\nQueue index: "
            << player.current_index()
            << "\n> ";

        std::string command;
        std::getline(std::cin, command);

        if (command == "q") {
            break;
        }

        if (command == "p") {
            if (player.is_playing()) {
                player.pause();
                paused = true;
            } else {
                player.start();
                paused = false;
            }
        }

        else if (command == "s") {
            player.stop();
        }

        else if (command == "r") {
            player.seek_to_start();
        }

        else if (command == "m") {
            // Seek to 50% through current song.
            player.seek(0.5f);
        }

        else if (command == "l") {
            // Seek to 50% through current song.
            player.seek_relative_seconds(10);
        }


        else if (command == "h") {
            // Seek to 50% through current song.
            player.seek_relative_seconds(-10);
        }

        else if (command == "f") {
            // Imagine the user clicked a single from somewhere else
            // while listening to the album.
            //
            // If song_01 is currently playing:
            //
            // Before:
            //   song_01
            //   song_02
            //   song_03
            //   song_04
            //
            // After:
            //   song_01
            //   random_single
            //   song_02
            //   song_03
            //   song_04
            //
            // random_single starts IMMEDIATELY.
            // When it finishes, song_02 plays next.

            if (!player.play_track("/home/jmanc3/time.flac")) {
                std::cerr
                    << "Could not force-play track: "
                    << player.last_error()
                    << '\n';
            }

            std::cout << "\nQueue is now:\n";

            for (std::size_t i = 0; i < player.queue().size(); ++i) {
                std::cout
                    << i << ": "
                    << player.queue()[i];

                if (i == player.current_index())
                    std::cout << "  <-- PLAYING";

                std::cout << '\n';
            }
        }

        else if (
            command.size() == 1 &&
            command[0] >= '0' &&
            command[0] <= '9'
        ) {
            const std::size_t index =
                static_cast<std::size_t>(command[0] - '0');

            if (!player.play_queued_item(index)) {
                std::cerr
                    << "Could not play queue item: "
                    << player.last_error()
                    << '\n';
            }
        }
    }

    player.stop();
    return 0;
    */
}