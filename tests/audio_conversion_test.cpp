#include "audio_conversion.h"
#include "audio_data.h"
#include "player.h"
#include "miniaudio.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <glib.h>

static ma_context context;
extern "C" ma_result __real_ma_device_init(ma_context *, const ma_device_config *, ma_device *);
extern "C" void __real_ma_device_uninit(ma_device *);
extern "C" ma_result __real_ma_device_stop(ma_device *);
extern "C" ma_result __real_ma_device_start(ma_device *);
extern "C" ma_result __wrap_ma_device_init(ma_context *, const ma_device_config *config, ma_device *device) {
    return __real_ma_device_init(&context, config, device);
}
extern "C" void __wrap_ma_device_uninit(ma_device *device) { __real_ma_device_uninit(device); }
extern "C" ma_result __wrap_ma_device_stop(ma_device *device) { return __real_ma_device_stop(device); }
extern "C" ma_result __wrap_ma_device_start(ma_device *device) { return __real_ma_device_start(device); }

void check(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void poll(Player &player) {
    for (int i = 0; i < 1000 && player.conversion_progress().active; ++i) {
        player.poll_conversion();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(!player.conversion_progress().active, "conversion timed out");
}
int main() {
    auto ffmpeg = g_find_program_in_path("ffmpeg");
    if (!ffmpeg) return 77;
    const std::string real_ffmpeg(ffmpeg);
    g_free(ffmpeg);
    ma_backend backend = ma_backend_null;
    check(ma_context_init(&backend, 1, nullptr, &context) == MA_SUCCESS, "null audio backend");
    struct AudioCleanup { ~AudioCleanup() { ma_context_uninit(&context); } } audio_cleanup;
    char pattern[] = "/tmp/tunes-conversion-XXXXXX";
    const std::filesystem::path base(mkdtemp(pattern));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{base};
    const auto first = (base / "song ' $literal.m4a").string();
    const auto second = (base / "second.M4A").string();
    const char *argv[] = {"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "sine=frequency=440:duration=1",
        "-metadata", "title=Test song", "-metadata", "album=Test album", "-c:a", "aac", first.c_str(), nullptr};
    int status = -1;
    check(g_spawn_sync(nullptr, const_cast<char **>(argv), nullptr, G_SPAWN_SEARCH_PATH,
                       nullptr, nullptr, nullptr, nullptr, &status, nullptr) && status == 0, "generate M4A");
    std::filesystem::copy_file(first, second);
    {
        Player player;
        player.queue() = {first, second};
        check(player.play_queued_item(1), "schedule album");
        poll(player);
        check(player.conversion_progress().error.empty(), "album conversion failed");
        check(player.current_path() == second + ".flac", "wrong track selected");
        check(player.queue()[0] == first + ".flac" && std::filesystem::exists(first + ".flac"), "album not preconverted");
    }
    check(std::filesystem::exists(first), "source removed");
    auto metadata = read_track(first + ".flac");
    check(metadata.name == "Test song" && metadata.album == "Test album", "metadata lost");
    const auto third = (base / "progress.m4a").string();
    std::filesystem::copy_file(first, third);
    double elapsed = -1;
    convert_to_flac(third, [&](double seconds) { elapsed = seconds; });
    check(elapsed > 0, "no FFmpeg progress received");
    const auto modified = std::filesystem::last_write_time(first + ".flac");
    check(convert_to_flac(first, {}).path == first + ".flac", "reuse path");
    check(std::filesystem::last_write_time(first + ".flac") == modified, "existing FLAC rewritten");
    std::vector<Option> tracks{read_track(first), metadata};
    auto albums = to_albums(tracks);
    check(albums.size() == 1 && albums[0].songs.size() == 1 && albums[0].songs[0].full == first + ".flac", "duplicate album entry");
    tracks = {read_track(first)};
    albums = to_albums(tracks);
    check(albums[0].songs[0].full == first + ".flac", "stale cache did not prefer FLAC");
    // Both common cover formats must survive the real conversion byte-for-byte.
    for (const std::string codec : {"png", "mjpeg"}) {
        const auto covered = (base / ("covered-" + codec + ".m4a")).string();
        const char *args[] = {"ffmpeg", "-v", "error", "-i", first.c_str(),
            "-f", "lavfi", "-i", "color=c=red:s=16x16", "-map", "0:a", "-map", "1:v",
            "-c:a", "copy", "-c:v", codec.c_str(), "-frames:v", "1",
            "-disposition:v", "attached_pic", covered.c_str(), nullptr};
        check(g_spawn_sync(nullptr, const_cast<char **>(args), nullptr, G_SPAWN_SEARCH_PATH,
            nullptr, nullptr, nullptr, nullptr, &status, nullptr) && status == 0, "generate M4A with cover");
        const auto original_art = read_album_art(covered);
        check(!original_art.bytes.empty(), "cover fixture is missing artwork");
        Player player;
        player.queue() = {covered};
        check(player.play_queued_item(0), "schedule covered track");
        poll(player);
        check(player.conversion_progress().error.empty(), "covered track conversion failed");
        const auto converted_art = read_album_art(covered + ".flac");
        check(converted_art.bytes == original_art.bytes && converted_art.extension == original_art.extension,
              "conversion lost or changed embedded cover art");
        check(read_track(covered + ".flac").name == "Test song", "covered track lost metadata");
        check(player.current_path() == covered + ".flac", "covered FLAC did not load for playback");
    }
    const auto invalid = (base / "invalid.m4a").string();
    std::ofstream(invalid) << "invalid";
    const auto failed = convert_to_flac(invalid, {});
    check(!failed && !failed.error.empty() && !std::filesystem::exists(invalid + ".flac"),
          "failed conversion must return an error without publishing output");
    const auto missing_source = convert_to_flac((base / "missing.m4a").string(), {});
    check(!missing_source && !missing_source.error.empty(), "missing source must return an error");
    for (auto &entry : std::filesystem::directory_iterator(base))
        check(entry.path().string().find(".tmp-") == std::string::npos, "temporary output leaked");
    {
        Player player;
        check(player.restore_session({first, second}, 1, .25), "schedule restore");
        check(player.start(), "schedule start");
        poll(player);
        check(player.current_index() == 1 && player.current_time_seconds() > .2, "restore lost index/position");
    }
    {
        Player player;
        player.queue() = {first, second};
        check(player.play_queued_item(1), "schedule selection");
        check(player.start(), "repeat play while converting");
        poll(player);
        check(player.current_index() == 1, "repeat play lost selected track");
    }
    {
        Player player;
        player.queue() = {first, second};
        check(player.play_queued_item(1), "schedule paused selection");
        player.pause();
        poll(player);
        check(!player.is_playing() && player.current_index() == 1, "pause during conversion ignored");
        player.queue() = {invalid};
        check(player.play_queued_item(0), "schedule old request");
        player.queue() = {first};
        check(player.play_queued_item(0), "schedule newer request");
        poll(player);
        check(player.current_path() == first + ".flac" && player.is_playing(), "stale request won");
    }
    for (const auto &[extension, codec] : std::vector<std::pair<std::string, std::string>>{
            {"ogg", "libvorbis"}, {"opus", "libopus"}, {"aac", "aac"},
            {"wav", "pcm_s16le"}, {"mp3", "libmp3lame"}, {"aiff", "pcm_s16be"}}) {
        const auto path = (base / ("format." + extension)).string();
        const char *args[] = {"ffmpeg", "-v", "error", "-i", first.c_str(),
            "-c:a", codec.c_str(), path.c_str(), nullptr};
        check(g_spawn_sync(nullptr, const_cast<char **>(args), nullptr, G_SPAWN_SEARCH_PATH,
            nullptr, nullptr, nullptr, nullptr, &status, nullptr) && status == 0, "generate format fixture");
        const bool native = extension == "wav" || extension == "mp3" || extension == "aiff";
        check(needs_audio_conversion(path) != native, "incorrect decoder support detection");
        if (native) {
            check(convert_to_flac(path, {}).path == path, "native audio unnecessarily converted");
            const auto renamed = path + ".unknown";
            std::filesystem::copy_file(path, renamed);
            check(!needs_audio_conversion(renamed), "decoder check relied on extension");
        } else {
            Player player;
            player.queue() = {path};
            check(player.play_queued_item(0), "schedule other format");
            poll(player);
            check(player.conversion_progress().error.empty() && player.current_path() == path + ".flac",
                  "other format not converted and played");
            check(!needs_audio_conversion(path + ".flac"), "converted audio is not decodable");
            std::vector<Option> options{read_track(path), read_track(path + ".flac")};
            auto grouped = to_albums(options);
            check(grouped.size() == 1 && grouped[0].songs.size() == 1, "other format duplicate not hidden");
        }
    }
    {
        const auto missing = (base / "missing-ffmpeg.m4a").string();
        std::filesystem::copy_file(first, missing);
        Player player;
        const std::string previous_path = g_getenv("PATH") ? g_getenv("PATH") : "";
        g_setenv("PATH", base.c_str(), true);
        check(convert_to_flac(first, {}).path == first + ".flac", "existing conversion needs FFmpeg");
        const auto unavailable = convert_to_flac(missing, {});
        check(!unavailable && unavailable.error.find("FFmpeg") != std::string::npos,
              "missing FFmpeg must return a notification result");
        check(convert_to_flac(first + ".flac", {}).path == first + ".flac",
              "native playback must not require FFmpeg");
        player.queue() = {missing};
        check(player.play_queued_item(0), "schedule without FFmpeg");
        poll(player);
        g_setenv("PATH", previous_path.c_str(), true);
        const auto error = player.conversion_progress().error;
        check(error.find("not supported") != std::string::npos &&
              error.find("Cannot auto-convert") != std::string::npos &&
              error.find("FFmpeg was not\nfound on this system") != std::string::npos,
              "missing FFmpeg explanation not delivered to notification");
        check(!std::filesystem::exists(missing + ".flac"), "missing FFmpeg published output");
        player.queue() = {first + ".flac"};
        check(player.play_queued_item(0) && player.is_playing(), "conversion failure disabled supported playback");
    }
    {
        const auto source = (base / "failure-cases.m4a").string();
        std::filesystem::copy_file(first, source);
        const auto bin = base / "failure-bin";
        std::filesystem::create_directory(bin);
        const auto fake = bin / "ffmpeg";
        const std::string old_path = g_getenv("PATH");
        for (const std::string script : {"#!/bin/sh\nexit 1\n", "#!/bin/sh\nexit 0\n", "invalid executable\n"}) {
            std::ofstream(fake) << script;
            std::filesystem::permissions(fake, std::filesystem::perms::owner_all);
            g_setenv("PATH", bin.c_str(), true);
            const auto result = convert_to_flac(source, {});
            g_setenv("PATH", old_path.c_str(), true);
            check(!result && !result.error.empty(), "FFmpeg process failure must return an error");
            check(!std::filesystem::exists(source + ".flac"), "process failure published output");
        }
        const auto long_source = (base / (std::string(244, 'a') + ".m4a")).string();
        std::filesystem::copy_file(first, long_source);
        const auto creation_failure = convert_to_flac(long_source, {});
        check(!creation_failure && creation_failure.error.find("Cannot create") != std::string::npos,
              "output creation failure must return an error");
        std::filesystem::create_directory(source + ".flac");
        check(!convert_to_flac(source, {}), "unusable destination must return an error");
        check(std::filesystem::is_directory(source + ".flac"), "existing destination was overwritten");
        for (const auto &entry : std::filesystem::directory_iterator(base))
            check(entry.path().string().find(".tmp-") == std::string::npos, "failure leaked partial output");
    }
    {
        const auto bin = base / "bin";
        std::filesystem::create_directory(bin);
        const auto wrapper = bin / "ffmpeg";
        std::ofstream(wrapper) << R"(#!/bin/sh
for arg do
    case "$arg" in
        */slow-background.m4a)
            while [ ! -e "$TUNES_CONVERSION_GATE" ]; do /bin/sleep 0.01; done ;;
    esac
done
exec "$TUNES_TEST_FFMPEG" "$@"
)";
        std::filesystem::permissions(wrapper, std::filesystem::perms::owner_all);
        const auto gate = base / "conversion-gate";
        const auto fast = (base / "fast.m4a").string();
        const auto slow = (base / "slow-background.m4a").string();
        const auto other = (base / "other-album.m4a").string();
        for (const auto &path : {fast, slow, other}) std::filesystem::copy_file(first, path);
        const std::string old_path = g_getenv("PATH");
        g_setenv("PATH", bin.c_str(), true);
        g_setenv("TUNES_TEST_FFMPEG", real_ffmpeg.c_str(), true);
        g_setenv("TUNES_CONVERSION_GATE", gate.c_str(), true);
        {
            Player player;
            // Unblock the child before Player waits for it, including on test failures.
            struct ReleaseGate { std::filesystem::path path; ~ReleaseGate() { std::ofstream(path) << "go"; } } release{gate};
            auto wait_for_track = [&](const std::string &path) {
                for (int i = 0; i < 500 && player.current_path() != path; ++i) {
                    player.poll_conversion();
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                check(player.current_path() == path && player.is_playing(), "selected track did not start promptly");
            };
            player.queue() = {fast, slow, first + ".flac"};
            check(player.play_queued_item(0), "schedule incremental album");
            wait_for_track(fast + ".flac");
            check(player.conversion_progress().active && !std::filesystem::exists(slow + ".flac"),
                  "first track waited for the rest of the album");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            check(player.current_index() == 0, "playback skipped a converting track");
            player.queue() = {first + ".flac"};
            check(player.play_queued_item(0) && player.current_path() == first + ".flac",
                  "native album switch blocked on background conversion");
            player.queue() = {other};
            check(player.play_queued_item(0), "schedule another album");
            wait_for_track(other + ".flac");
            check(!std::filesystem::exists(slow + ".flac"), "new album waited for old conversion");
            std::ofstream(gate) << "go";
            poll(player);
            check(std::filesystem::exists(slow + ".flac"), "old album conversion was abandoned");
            check(player.queue() == std::vector<std::string>{other + ".flac"} &&
                  player.current_path() == other + ".flac", "old completion replaced newer playback");
            const auto next_dir = base / "late-next";
            std::filesystem::create_directory(next_dir);
            const auto late = (next_dir / "slow-background.m4a").string();
            std::filesystem::copy_file(first, late);
            std::filesystem::remove(gate);
            player.queue() = {first + ".flac", late, first + ".flac"};
            check(player.play_queued_item(0), "schedule late next track");
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            check(player.current_index() == 0, "late next track was skipped");
            std::ofstream(gate) << "go";
            wait_for_track(late + ".flac");
            check(player.current_index() == 1, "late conversion did not continue in album order");
            poll(player);
        }
        g_setenv("PATH", old_path.c_str(), true);
        g_unsetenv("TUNES_TEST_FFMPEG");
        g_unsetenv("TUNES_CONVERSION_GATE");
    }
    std::cout << "Format detection, conversion, missing FFmpeg notification, album preparation and failure cleanup pass\n";
}
