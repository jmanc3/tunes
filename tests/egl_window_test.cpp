#include "client/egl_window.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-client.h>
#include <wayland-server.h>

// An isolated in-process compositor exercises real Mesa Wayland/EGL swaps. It
// needs no desktop socket and cannot change the user's session or music state.
struct Compositor {
    struct Surface {
        Compositor *server;
        wl_resource *pending = nullptr;
        std::vector<wl_resource *> callbacks;
    };
    wl_display *server = wl_display_create();
    wl_display *client = nullptr;
    wl_compositor *compositor = nullptr;
    wl_registry *registry = nullptr;
    std::thread thread;
    std::atomic<bool> running{true};
    std::mutex mutex;
    struct Frame {
        int width, height;
        uint32_t center;
    };
    std::vector<Frame> frames;

    static void destroy(wl_client *, wl_resource *resource) { wl_resource_destroy(resource); }
    static void attach(wl_client *, wl_resource *surface, wl_resource *buffer, int32_t, int32_t) {
        static_cast<Surface *>(wl_resource_get_user_data(surface))->pending = buffer;
    }
    static void damage(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
    static void frame(wl_client *client, wl_resource *surface, uint32_t id) {
        auto data = static_cast<Surface *>(wl_resource_get_user_data(surface));
        data->callbacks.push_back(wl_resource_create(client, &wl_callback_interface, 1, id));
    }
    static void region(wl_client *, wl_resource *, wl_resource *) {}
    static void value(wl_client *, wl_resource *, int32_t) {}
    static void commit(wl_client *client, wl_resource *surface) {
        auto data = static_cast<Surface *>(wl_resource_get_user_data(surface));
        if (auto buffer = data->pending ? wl_shm_buffer_get(data->pending) : nullptr) {
            wl_shm_buffer_begin_access(buffer);
            const int width = wl_shm_buffer_get_width(buffer), height = wl_shm_buffer_get_height(buffer);
            auto bytes = static_cast<unsigned char *>(wl_shm_buffer_get_data(buffer));
            auto row = reinterpret_cast<uint32_t *>(bytes + height / 2 * wl_shm_buffer_get_stride(buffer));
            {
                std::lock_guard lock(data->server->mutex);
                data->server->frames.push_back({width, height, row[width / 2]});
            }
            wl_shm_buffer_end_access(buffer);
            wl_buffer_send_release(data->pending);
            data->pending = nullptr;
        }
        for (auto callback : data->callbacks) {
            wl_callback_send_done(callback, 0);
            wl_resource_destroy(callback);
        }
        data->callbacks.clear();
        wl_client_flush(client);
    }
    static void create_surface(wl_client *client, wl_resource *compositor, uint32_t id) {
        static const struct wl_surface_interface implementation = {
            .destroy = destroy,
            .attach = attach,
            .damage = damage,
            .frame = frame,
            .set_opaque_region = region,
            .set_input_region = region,
            .commit = commit,
            .set_buffer_transform = value,
            .set_buffer_scale = value,
            .damage_buffer = damage,
        };
        auto resource = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(compositor), id);
        auto data = new Surface{static_cast<Compositor *>(wl_resource_get_user_data(compositor))};
        wl_resource_set_implementation(resource, &implementation, data, [](wl_resource *resource) {
            auto data = static_cast<Surface *>(wl_resource_get_user_data(resource));
            for (auto callback : data->callbacks)
                wl_resource_destroy(callback);
            delete data;
        });
    }
    static void create_region(wl_client *client, wl_resource *, uint32_t id) {
        static const struct wl_region_interface implementation = {
            .destroy = destroy, .add = damage, .subtract = damage};
        auto resource = wl_resource_create(client, &wl_region_interface, 1, id);
        wl_resource_set_implementation(resource, &implementation, nullptr, nullptr);
    }
    Compositor() {
        wl_display_init_shm(server);
        wl_global_create(server, &wl_compositor_interface, 4, this,
                         [](wl_client *client, void *data, uint32_t version, uint32_t id) {
                             static const struct wl_compositor_interface implementation = {create_surface,
                                                                                           create_region};
                             auto resource = wl_resource_create(client, &wl_compositor_interface, version, id);
                             wl_resource_set_implementation(resource, &implementation, data, nullptr);
                         });
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
            throw std::runtime_error("socketpair failed");
        wl_client_create(server, sockets[0]);
        client = wl_display_connect_to_fd(sockets[1]);
        thread = std::thread([this] {
            while (running) {
                wl_event_loop_dispatch(wl_display_get_event_loop(server), 10);
                wl_display_flush_clients(server);
            }
        });
        registry = wl_display_get_registry(client);
        static const wl_registry_listener listener = {
            .global =
                [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
                    if (std::strcmp(interface, "wl_compositor") == 0)
                        static_cast<Compositor *>(data)->compositor = static_cast<wl_compositor *>(
                            wl_registry_bind(registry, name, &wl_compositor_interface, std::min(version, 4u)));
                },
            .global_remove = [](void *, wl_registry *, uint32_t) {},
        };
        wl_registry_add_listener(registry, &listener, this);
        wl_display_roundtrip(client);
    }
    ~Compositor() {
        if (compositor)
            wl_compositor_destroy(compositor);
        if (registry)
            wl_registry_destroy(registry);
        if (client)
            wl_display_disconnect(client);
        running = false;
        if (thread.joinable())
            thread.join();
        wl_display_destroy_clients(server);
        wl_display_destroy(server);
    }
    void expect(int width, int height, uint32_t pixel) {
        wl_display_roundtrip(client);
        std::lock_guard lock(mutex);
        if (frames.empty() || frames.back().width != width || frames.back().height != height ||
            frames.back().center != pixel)
            throw std::runtime_error("Wayland compositor received incorrect frame pixels or dimensions");
    }
};
int main() try {
    Compositor compositor;
    auto first = wl_compositor_create_surface(compositor.compositor);
    auto second = wl_compositor_create_surface(compositor.compositor);
    {
        EGLWindow a(compositor.client, first, 64, 48);
        EGLWindow b(compositor.client, second, 32, 24);
        auto draw = [](EGLWindow &window, RGBA color) {
            window.begin_frame();
            window.context()->set_color(color);
            window.context()->paint_source();
            window.present();
        };
        draw(a, RGBA(1, 0, 0, 1));
        compositor.expect(64, 48, 0xffff0000);
        draw(b, RGBA(0, 1, 0, 1));
        compositor.expect(32, 24, 0xff00ff00);
        a.resize(96, 72);
        draw(a, RGBA(0, 0, 1, 1));
        compositor.expect(96, 72, 0xff0000ff);
        draw(b, RGBA(1, 1, 1, 1));
        compositor.expect(32, 24, 0xffffffff);
    }
    wl_surface_destroy(first);
    wl_surface_destroy(second);
    wl_display_roundtrip(compositor.client);
    std::cout << "Native Wayland/EGL presentation, resize, context switching, and cleanup passed\n";
    return 0;
} catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
}
