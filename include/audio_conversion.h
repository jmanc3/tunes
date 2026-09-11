#pragma once
#include <functional>
#include <string>

// Tests this build's decoder against the actual file contents.
bool needs_audio_conversion(const std::string &path);
std::string preferred_audio_path(const std::string &path);
struct AudioConversionResult {
    std::string path;
    std::string error;
    explicit operator bool() const { return error.empty() && !path.empty(); }
};
// Optional conversion: expected failures are returned for the notification, never thrown.
// Progress is elapsed output seconds. The source is always retained.
AudioConversionResult convert_to_flac(const std::string &path, const std::function<void(double)> &progress);
