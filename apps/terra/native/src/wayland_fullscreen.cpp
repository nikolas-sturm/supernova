#include "wayland_fullscreen.h"

#if defined(__linux__) && defined(TERRA_HAS_LINUX_VIDEO)
#include <SDL.h>
#include <SDL_syswm.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-output-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

#if SDL_VERSION_ATLEAST(2, 0, 18) && defined(SDL_VIDEO_DRIVER_WAYLAND)
namespace terra {
namespace {

struct Output {
    std::uint32_t name = 0;
    wl_output* handle = nullptr;
    zxdg_output_v1* logical = nullptr;
    MouseRectangle rectangle;
    bool position = false;
    bool size = false;

    ~Output() {
        if (logical) zxdg_output_v1_destroy(logical);
        if (!handle) return;
        if (wl_output_get_version(handle) >= 3) wl_output_release(handle);
        else wl_output_destroy(handle);
    }
};

// Bind at most v3: geometry, mode, done and scale are the only possible events.
const wl_output_listener outputListener = {
    .geometry = [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t,
                   int32_t, const char*, const char*, int32_t) noexcept {},
    .mode = [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) noexcept {},
    .done = [](void*, wl_output*) noexcept {},
    .scale = [](void*, wl_output*, int32_t) noexcept {},
};

const zxdg_output_v1_listener logicalListener = {
    .logical_position = [](void* data, zxdg_output_v1*, int32_t x, int32_t y) noexcept {
        auto& output = *static_cast<Output*>(data);
        output.rectangle.x = x;
        output.rectangle.y = y;
        output.position = true;
    },
    .logical_size = [](void* data, zxdg_output_v1*, int32_t width, int32_t height) noexcept {
        auto& output = *static_cast<Output*>(data);
        output.rectangle.width = width;
        output.rectangle.height = height;
        output.size = true;
    },
    .done = [](void*, zxdg_output_v1*) noexcept {},
    .name = [](void*, zxdg_output_v1*, const char*) noexcept {},
    .description = [](void*, zxdg_output_v1*, const char*) noexcept {},
};

struct Discovery {
    wl_registry* registry = nullptr;
    zxdg_output_manager_v1* manager = nullptr;
    std::uint32_t managerName = 0;
    std::vector<std::unique_ptr<Output>> outputs;
    std::exception_ptr error;
    bool enumerated = false;
    bool changed = false;

    ~Discovery() {
        outputs.clear();
        if (manager) zxdg_output_manager_v1_destroy(manager);
        if (registry) wl_registry_destroy(registry);
    }

    void roundtrip(wl_display* display) {
        const int result = wl_display_roundtrip(display);
        if (error) std::rethrow_exception(error);
        if (result < 0) throw std::runtime_error("Wayland fullscreen roundtrip failed");
        if (changed) throw std::runtime_error("Wayland output topology changed; retry targeting");
    }

    static void global(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t version) noexcept {
        auto& state = *static_cast<Discovery*>(data);
        if (state.error || state.changed) return;
        try {
            const bool output = std::strcmp(interface, wl_output_interface.name) == 0;
            const bool manager = std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0;
            if (!output && !manager) return;
            if (state.enumerated) {
                state.changed = true;
                return;
            }
            if (output) {
                auto entry = std::make_unique<Output>();
                entry->name = name;
                entry->handle = static_cast<wl_output*>(
                    wl_registry_bind(registry, name, &wl_output_interface, std::min(version, 3u)));
                if (!entry->handle) throw std::runtime_error("Cannot bind Wayland output");
                if (wl_output_add_listener(entry->handle, &outputListener, entry.get()) < 0)
                    throw std::runtime_error("Cannot listen to Wayland output");
                state.outputs.push_back(std::move(entry));
            } else {
                if (state.manager) throw std::runtime_error("Multiple xdg-output managers");
                state.managerName = name;
                state.manager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(
                    registry, name, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
                if (!state.manager) throw std::runtime_error("Cannot bind xdg-output manager");
            }
        } catch (...) {
            state.error = std::current_exception();
        }
    }

    static void removed(void* data, wl_registry*, uint32_t name) noexcept {
        auto& state = *static_cast<Discovery*>(data);
        if (state.manager && state.managerName == name) state.changed = true;
        for (const auto& output : state.outputs)
            if (output->name == name) state.changed = true;
    }
};

const wl_registry_listener registryListener = {Discovery::global, Discovery::removed};

} // namespace

void requestWaylandFullscreenOutput(SDL_Window* window, const MouseRectangle& assigned) {
    if (!window || assigned.width <= 0 || assigned.height <= 0)
        throw std::runtime_error("Invalid Wayland fullscreen window or assigned rectangle");
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN))
        throw std::runtime_error("Wayland targeting requires an already-fullscreen SDL window");
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) throw std::runtime_error(SDL_GetError());
    if (info.subsystem != SDL_SYSWM_WAYLAND)
        throw std::runtime_error("Wayland targeting requires a Wayland SDL window");
    const auto& native = info.info.wl;
    if (!native.display || !native.surface || !native.xdg_toplevel)
        throw std::runtime_error("SDL did not expose a mapped Wayland xdg_toplevel");

    // All proxies inherit SDL's default queue. SDL retains its listeners and
    // handles its own configure/enter/leave events during these roundtrips.
    Discovery state;
    state.registry = wl_display_get_registry(native.display);
    if (!state.registry) throw std::runtime_error("Cannot create Wayland registry");
    if (wl_registry_add_listener(state.registry, &registryListener, &state) < 0)
        throw std::runtime_error("Cannot listen to Wayland registry");
    state.roundtrip(native.display);
    state.enumerated = true;
    if (!state.manager) throw std::runtime_error("Compositor lacks xdg-output logical rectangles");
    for (const auto& output : state.outputs) {
        output->logical = zxdg_output_manager_v1_get_xdg_output(state.manager, output->handle);
        if (!output->logical) throw std::runtime_error("Cannot create xdg-output");
        if (zxdg_output_v1_add_listener(output->logical, &logicalListener, output.get()) < 0)
            throw std::runtime_error("Cannot listen to xdg-output");
    }
    // This sync bounds initial logical events for both xdg-output v1/v2 (done)
    // and v3 (wl_output.done); no extra roundtrip is needed per output.
    state.roundtrip(native.display);
    wl_output* target = nullptr;
    for (const auto& output : state.outputs) {
        if (!output->position || !output->size || output->rectangle.width <= 0 ||
            output->rectangle.height <= 0)
            throw std::runtime_error("Incomplete Wayland output logical rectangle");
        if (output->rectangle != assigned) continue;
        if (target) throw std::runtime_error("Ambiguous Wayland output logical rectangle");
        target = output->handle;
    }
    if (!target) throw std::runtime_error("Assigned rectangle matches no Wayland output");
    // Deliberately bypass SDL positioning and its cached fullscreen target.
    xdg_toplevel_set_fullscreen(native.xdg_toplevel, target);
    state.roundtrip(native.display); // Flush the request; not a placement acknowledgement.
}

} // namespace terra
#else
namespace terra {
void requestWaylandFullscreenOutput(SDL_Window*, const MouseRectangle&) {
    throw std::runtime_error("Explicit Wayland workspace placement requires SDL 2.0.18 or newer with Wayland support.");
}
}
#endif
#endif
