#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace playlist_art {

struct ImportResult {
    std::string file;
    std::string error;
};

// Session records contain only generated filenames, never the selected source path.
std::filesystem::path owned_file(const std::filesystem::path &session, const std::string &file);
ImportResult import_image(const std::filesystem::path &session, const std::filesystem::path &source);
std::string remove_image(const std::filesystem::path &session, const std::string &file);

// GTK runs on the application's event thread; poll() dispatches without blocking.
class Chooser {
public:
    Chooser();
    ~Chooser();
    bool show(std::function<void(std::filesystem::path)> completed, std::string &error);
    bool visible() const;
    void poll();
    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
