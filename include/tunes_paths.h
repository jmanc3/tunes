#pragma once

#include <filesystem>
#include <glib.h>

inline std::filesystem::path tunes_cache_directory() {
    return std::filesystem::path(g_get_home_dir()) / ".cache" / "tunes";
}
