#pragma once

#include <filesystem>
#include <string>
#include <vector>

struct PipeWireSettingsResult {
    bool success;
    std::string message;
};

std::filesystem::path pipewire_rates_path();
PipeWireSettingsResult set_pipewire_force_rate(unsigned rate);
PipeWireSettingsResult change_pipewire_rates_config(const std::filesystem::path &path,
                                                   const std::vector<unsigned> &rates, bool remove);
