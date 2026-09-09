#include "pipewire_settings.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static std::string read(const std::filesystem::path &path) {
    std::ifstream stream(path);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int main() {
    char pattern[] = "/tmp/tunes-pipewire-test-XXXXXX";
    const char *directory = mkdtemp(pattern);
    if (!directory) return 1;
    const std::filesystem::path root(directory);
    try {
        const auto config = root / "pipewire.conf.d/10-rates.conf";
        require(change_pipewire_rates_config(config, {44100, 48000, 192000}, false).success, "create config");
        const auto original = read(config);
        require(original.find("default.clock.rate = 48000") != std::string::npos, "default rate");
        require(original.find("[ 44100 48000 192000 ]") != std::string::npos, "allowed rates");
        require(!change_pipewire_rates_config(config, {96000}, false).success, "must not overwrite existing file");
        require(read(config) == original, "existing file preserved");
        require(change_pipewire_rates_config(config, {}, true).success, "remove config");
        require(!std::filesystem::exists(config), "config removed");
        require(!change_pipewire_rates_config(config, {}, false).success, "reject empty rates");
        require(!change_pipewire_rates_config(config, {0}, false).success, "reject invalid rates");
        require(change_pipewire_rates_config(config, {96000}, false).success, "single-rate config");
        require(read(config).find("default.clock.rate = 96000") != std::string::npos, "default must be allowed");
        const auto link = root / "linked.conf";
        std::filesystem::create_symlink(config, link);
        require(!change_pipewire_rates_config(link, {}, true).success, "reject symlink removal");
        require(std::filesystem::exists(config), "symlink target preserved");

        // Execute a fake utility so tests never change the user's PipeWire server.
        const auto utility = root / "pw-metadata";
        auto script = [&](const std::string &body) {
            std::ofstream(utility) << "#!/bin/sh\n" << body;
            std::filesystem::permissions(utility, std::filesystem::perms::owner_all);
        };
        setenv("PATH", root.c_str(), 1);
        script("exit 7\n");
        require(!set_pipewire_force_rate(192000).success, "nonzero command exit reported");
        script("[ \"$#\" = 5 ] && [ \"$1\" = -n ] && [ \"$2\" = settings ] && "
               "[ \"$3\" = 0 ] && [ \"$4\" = clock.force-rate ] && [ \"$5\" = 192000 ]\n");
        require(set_pipewire_force_rate(192000).success, "force-rate arguments");
        script("[ \"$#\" = 5 ] && [ \"$5\" = 0 ]\n");
        require(set_pipewire_force_rate(0).success, "automatic rate arguments");
        require(!set_pipewire_force_rate(1).success, "reject invalid force rate");
        script("while :; do :; done\n");
        require(!set_pipewire_force_rate(48000).success, "timeout reported");
        std::filesystem::remove(utility);
        require(!set_pipewire_force_rate(48000).success, "missing utility reported");
        std::filesystem::remove_all(root);
        std::cout << "PipeWire settings tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
