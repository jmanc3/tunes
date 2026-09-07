#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "client/raw_windowing.h"
#include "player.h"
#include "client/windowing.h"
#include "utility.h"

static long get_current_time_in_ms() {
    using namespace std::chrono;
    milliseconds currentTime = duration_cast<milliseconds>(system_clock::now().time_since_epoch());
    return currentTime.count();
}

void open_window() {
    RawWindowSettings settings;
    settings.name = "Title";
    auto app = windowing::open_app();
    auto mylar_window = open_mylar_window(app, WindowType::NORMAL, settings);
    long start_time = get_current_time_in_ms();
    mylar_window->root->when_paint = [mylar_window, start_time](Container *root, Container *c) {
        auto cr = mylar_window->raw_window->cr;
        auto b = c->real_bounds;
        auto current = get_current_time_in_ms();
        float offset = ((float) (current - start_time)) / 10000.0f;
        b.x += b.w * offset;
        set_rect(cr, b); 
        set_argb(cr, RGBA(1, 1, 1, 1));
        cairo_fill(cr);
        // windowing::redraw(mylar_window->raw_window);
    };
    windowing::main_loop(app);
}

int main() {
    open_window();
    return 0;
    /*
    Player player(5.0); // 5 ms crossfade

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