#include "playlist_art.h"

#include <gtk/gtk.h>
#include <unistd.h>
#include <utility>

namespace playlist_art {
namespace fs = std::filesystem;

fs::path owned_file(const fs::path &session, const std::string &file) {
    if (file.size() != 42 || !file.ends_with(".image") || !g_uuid_string_is_valid(file.substr(0, 36).c_str()))
        return {};
    return session.parent_path() / (session.filename().string() + ".art") / file;
}

ImportResult import_image(const fs::path &session, const fs::path &source) {
    auto uuid = g_uuid_string_random();
    const std::string file = std::string(uuid) + ".image";
    g_free(uuid);
    const auto destination = owned_file(session, file);
    std::error_code error;
    fs::create_directories(destination.parent_path(), error);
    if (error) return {{}, "Could not save artwork: " + error.message()};
    auto staging = destination.string() + ".XXXXXX";
    const int fd = g_mkstemp(staging.data());
    if (fd < 0) return {{}, "Could not create the artwork file."};
    close(fd);
    struct Cleanup {
        fs::path path;
        ~Cleanup() { std::error_code error; fs::remove(path, error); }
    } cleanup{staging};
    fs::copy_file(source, staging, fs::copy_options::overwrite_existing, error);
    if (error) return {{}, "Could not copy artwork: " + error.message()};
    GError *decode_error = nullptr;
    auto image = gdk_pixbuf_new_from_file(staging.c_str(), &decode_error);
    if (!image) {
        const auto message = decode_error ? std::string(decode_error->message) : "This image format is not supported.";
        g_clear_error(&decode_error);
        return {{}, message};
    }
    g_object_unref(image);
    fs::rename(staging, destination, error);
    if (error) return {{}, "Could not save artwork: " + error.message()};
    return {file, {}};
}

std::string remove_image(const fs::path &session, const std::string &file) {
    const auto path = owned_file(session, file);
    if (path.empty()) return {};
    std::error_code error;
    fs::remove(path, error);
    return error ? "Could not remove artwork: " + error.message() : std::string{};
}

struct Chooser::Impl {
    GtkFileChooserNative *dialog = nullptr;
    std::function<void(fs::path)> completed;
    bool initialized = false;
};

Chooser::Chooser() : impl_(std::make_unique<Impl>()) {}
Chooser::~Chooser() { close(); }

bool Chooser::show(std::function<void(fs::path)> completed, std::string &error) {
    if (impl_->dialog) return false;
    if (!impl_->initialized) {
        if (!gtk_init_check(nullptr, nullptr)) {
            error = "Could not open the image chooser.";
            return false;
        }
        impl_->initialized = true;
    }
    impl_->completed = std::move(completed);
    impl_->dialog = gtk_file_chooser_native_new("Choose playlist artwork", nullptr,
                                               GTK_FILE_CHOOSER_ACTION_OPEN, "Choose art", "Cancel");
    auto filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Supported images");
    gtk_file_filter_add_pixbuf_formats(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(impl_->dialog), filter);
    gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(impl_->dialog), filter);
    gtk_file_chooser_set_local_only(GTK_FILE_CHOOSER(impl_->dialog), TRUE);
    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(impl_->dialog), FALSE);
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(impl_->dialog), TRUE);
    g_signal_connect(impl_->dialog, "response", G_CALLBACK(+[](GtkNativeDialog *dialog, gint response, gpointer data) {
        auto self = static_cast<Impl *>(data);
        fs::path selected;
        if (response == GTK_RESPONSE_ACCEPT) {
            auto filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
            if (filename) selected = filename;
            g_free(filename);
        }
        auto completed = std::move(self->completed);
        self->dialog = nullptr;
        g_signal_handlers_disconnect_by_data(dialog, self);
        gtk_native_dialog_destroy(dialog);
        g_object_unref(dialog);
        if (completed) completed(std::move(selected));
    }), impl_.get());
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(impl_->dialog));
    return true;
}

bool Chooser::visible() const { return impl_->dialog != nullptr; }

void Chooser::poll() {
    if (!impl_->initialized) return;
    for (int i = 0; i < 32 && g_main_context_pending(nullptr); ++i)
        g_main_context_iteration(nullptr, FALSE);
}

void Chooser::close() {
    if (!impl_->dialog) return;
    auto dialog = std::exchange(impl_->dialog, nullptr);
    impl_->completed = {};
    g_signal_handlers_disconnect_by_data(dialog, impl_.get());
    gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(dialog));
    g_object_unref(dialog);
}

}
