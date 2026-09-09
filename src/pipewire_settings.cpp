#include "pipewire_settings.h"

#include <gio/gio.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <thread>
#include <csignal>
#include <sys/wait.h>

std::filesystem::path pipewire_rates_path() {
    return std::filesystem::path(g_get_home_dir()) / ".config/pipewire/pipewire.conf.d/10-rates.conf";
}

PipeWireSettingsResult set_pipewire_force_rate(unsigned rate) {
    if (rate != 0 && (rate < 8000 || rate > 384000))
        return {false, "Invalid PipeWire clock rate."};
    std::string value = std::to_string(rate);
    const char *args[] = {"pw-metadata", "-n", "settings", "0", "clock.force-rate", value.c_str(), nullptr};
    GPid pid = 0;
    GError *error = nullptr;
    if (!g_spawn_async(nullptr, const_cast<char **>(args), nullptr,
                       static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
                                                G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL),
                       nullptr, nullptr, &pid, &error)) {
        g_clear_error(&error);
        return {false, "Could not run pw-metadata. Install the PipeWire command-line tools."};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    int status = 0;
    for (;;) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) {
            g_spawn_close_pid(pid);
            return {false, "Could not read the pw-metadata result."};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            g_spawn_close_pid(pid);
            return {false, "pw-metadata timed out. Check that PipeWire is running."};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    g_spawn_close_pid(pid);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return {false, "pw-metadata failed. Check PipeWire and access to its settings."};
    return {true, rate == 0 ? "PipeWire clock set to Auto." : "PipeWire clock forced to " + value + " Hz."};
}

PipeWireSettingsResult change_pipewire_rates_config(const std::filesystem::path &path,
                                                   const std::vector<unsigned> &rates, bool remove) {
    std::error_code error;
    if (remove) {
        // The remove button names this file explicitly; never remove a directory or follow a symlink.
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
            return {false, "Rate config is missing or is not a regular file."};
        if (!std::filesystem::remove(path, error) || error)
            return {false, "Could not remove 10-rates.conf. Check file permissions."};
        return {true, "Rate config removed. Restart PipeWire to apply."};
    }
    if (rates.empty()) return {false, "No sample rates available for the rate config."};
    const unsigned default_rate = std::find(rates.begin(), rates.end(), 48000) != rates.end() ? 48000 : rates.front();
    std::string contents = "# Sample rates configured by Tunes\ncontext.properties = {\n    default.clock.rate = " +
        std::to_string(default_rate) + "\n    default.clock.allowed-rates = [";
    for (unsigned rate : rates) {
        if (rate < 8000 || rate > 384000) return {false, "Invalid sample rate in the rate config."};
        contents += " " + std::to_string(rate);
    }
    contents += " ]\n}\n";
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return {false, "Could not create the PipeWire configuration directory."};
    auto *file = g_file_new_for_path(path.c_str());
    GError *io_error = nullptr;
    // Exclusive creation preserves any existing user configuration.
    auto *stream = g_file_create(file, G_FILE_CREATE_NONE, nullptr, &io_error);
    g_object_unref(file);
    if (!stream) {
        g_clear_error(&io_error);
        return {false, "Could not create 10-rates.conf. Check whether it already exists."};
    }
    const bool written = g_output_stream_write_all(G_OUTPUT_STREAM(stream), contents.data(), contents.size(),
                                                   nullptr, nullptr, &io_error);
    g_clear_error(&io_error);
    const bool closed = g_output_stream_close(G_OUTPUT_STREAM(stream), nullptr, &io_error);
    g_clear_error(&io_error);
    g_object_unref(stream);
    if (!written || !closed) {
        std::filesystem::remove(path, error);
        return {false, "Could not write 10-rates.conf. Check disk space and permissions."};
    }
    return {true, "Rate config added. Restart PipeWire to apply."};
}
