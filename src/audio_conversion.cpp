#include "audio_conversion.h"
#include "miniaudio.h"
#include <glib.h>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

bool needs_audio_conversion(const std::string &path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    if (preferred_audio_path(path) != path) return true;
    // Probe the actual decoder, not the suffix: containers can hold codecs that
    // this miniaudio build cannot decode, and supported files may be renamed.
    ma_decoder decoder;
    const auto config = ma_decoder_config_init(ma_format_f32, 2, 48000);
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) != MA_SUCCESS) return true;
    ma_decoder_uninit(&decoder);
    return false;
}

std::string preferred_audio_path(const std::string &path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path + ".flac", ec) &&
        std::filesystem::file_size(path + ".flac", ec) > 0 && !ec)
        return path + ".flac";
    return path;
}

AudioConversionResult convert_to_flac(const std::string &path, const std::function<void(double)> &progress) {
    const auto existing = preferred_audio_path(path);
    if (existing != path) return {existing, {}};
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec))
        return {{}, "Source audio is missing or inaccessible: " + path};
    if (!needs_audio_conversion(path)) return {path, {}};
    const std::string unavailable = "This format is not supported directly.\nCannot auto-convert: FFmpeg was not\nfound on this system. Install FFmpeg.";
    auto executable = g_find_program_in_path("ffmpeg");
    if (!executable) return {{}, unavailable};
    const std::string ffmpeg(executable);
    g_free(executable);
    const auto input = std::filesystem::absolute(path, ec).string();
    if (ec) return {{}, "Cannot locate source audio: " + path};
    const auto target = path + ".flac";
    std::string temporary = target + ".tmp-XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) return {{}, "Cannot create FLAC beside " + path};
    close(fd);
    struct Cleanup {
        std::string path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove(path, ec); }
    } cleanup{temporary};
    int pipes[2];
    // Concurrent children must not inherit one another's progress pipes.
    if (pipe2(pipes, O_CLOEXEC) != 0) return {{}, "Cannot read FFmpeg progress"};
    FILE *stream = fdopen(pipes[0], "r");
    if (!stream) {
        close(pipes[0]);
        close(pipes[1]);
        return {{}, "Cannot read FFmpeg progress"};
    }
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) {
        fclose(stream);
        close(pipes[1]);
        return {{}, "Cannot prepare FFmpeg process"};
    }
    error = posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipes[0]);
    if (!error) error = posix_spawn_file_actions_addclose(&actions, pipes[1]);
    // argv keeps filenames literal, including spaces and shell metacharacters.
    const char *args[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
        "-i", input.c_str(), "-map", "0:a:0", "-map", "0:v?",
        "-map_metadata", "0", "-c:a", "flac", "-compression_level", "8",
        "-c:v", "copy", "-disposition:v", "attached_pic",
        "-progress", "pipe:1", "-nostats", "-f", "flac", temporary.c_str(), nullptr};
    pid_t pid;
    if (!error) error = posix_spawn(&pid, ffmpeg.c_str(), &actions, nullptr, const_cast<char **>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipes[1]);
    if (error) {
        fclose(stream);
        return {{}, error == ENOENT ? unavailable : "FFmpeg was found but could not start."};
    }
    char line[512];
    while (fgets(line, sizeof(line), stream)) {
        double micros;
        if (sscanf(line, "out_time_us=%lf", &micros) == 1 && progress)
            progress(std::max(0.0, micros / 1000000.0));
    }
    const bool read_failed = ferror(stream);
    fclose(stream);
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {{}, "FFmpeg could not convert " + path};
    if (read_failed) return {{}, "Could not read FFmpeg progress"};
    // Publish only complete, nonempty output without replacing an existing destination.
    const auto size = std::filesystem::file_size(temporary, ec);
    if (ec || size == 0) return {{}, "FFmpeg produced no audio for " + path};
    if (link(temporary.c_str(), target.c_str()) != 0 && errno != EEXIST)
        return {{}, "Cannot save " + target};
    if (preferred_audio_path(path) == path)
        return {{}, "FLAC output is unavailable: " + target};
    return {target, {}};
}
