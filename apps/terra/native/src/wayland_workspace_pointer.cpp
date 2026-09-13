#include "wayland_workspace_pointer.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <stdexcept>

#if defined(__linux__)
#include <SDL_syswm.h>
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
#include <wayland-client.h>
#endif
#endif

namespace terra {

namespace {
struct PointerMessage {
    Uint32 type;
    Uint32 timestamp;
    Uint32 windowId;
    Uint32 generation;
    WorkspacePointerEvent pointer;
};
static_assert(sizeof(PointerMessage) <= sizeof(SDL_Event));
}

struct WaylandWorkspacePointer::Impl {
    WorkspacePointerState state;
    bool installed = false;
    std::exception_ptr callbackError;
    bool cancelPending = false;
    Uint32 eventType = static_cast<Uint32>(-1);
    Uint32 windowId = 0;
    Uint32 generation = 0;

    void postEvents() {
        for (const auto& pointerEvent : state.takeEvents()) {
            const PointerMessage message{eventType, 0, windowId, generation, pointerEvent};
            SDL_Event event{};
            std::memcpy(&event, &message, sizeof(message));
            if (SDL_PushEvent(&event) != 1) {
                throw std::runtime_error("Cannot queue native workspace pointer event.");
            }
        }
    }

    void failClosed() noexcept {
        state.clear();
        ++generation;
        cancelPending = true;
    }

    template<class Callback>
    void callback(Callback&& fn) noexcept {
        if (callbackError) return;
        try {
            fn();
            // Queue immediately alongside SDL keyboard/wheel events. Draining an
            // independent pointer batch afterward would reorder modifier clicks.
            postEvents();
        } catch (...) {
            callbackError = std::current_exception();
            failClosed();
        }
    }

#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_WAYLAND)
    wl_display* display = nullptr;  // Borrowed from SDL; never disconnect.
    wl_surface* surface = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    std::uint32_t seatName = 0;
    bool selectedSeat = false;

    static int coordinate(wl_fixed_t value) noexcept {
        return static_cast<int>(std::floor(wl_fixed_to_double(value)));
    }

    static const wl_pointer_listener& pointerListener() {
        static const wl_pointer_listener listener = [] {
            wl_pointer_listener result{};
            result.enter = [](void* data, wl_pointer*, std::uint32_t, wl_surface* target,
                              wl_fixed_t x, wl_fixed_t y) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    if (target == self.surface) self.state.enter(coordinate(x), coordinate(y));
                    else self.state.leave();
                });
            };
            result.leave = [](void* data, wl_pointer*, std::uint32_t, wl_surface* target) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    if (target == self.surface) self.state.leave();
                });
            };
            result.motion = [](void* data, wl_pointer*, std::uint32_t, wl_fixed_t x, wl_fixed_t y) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] { self.state.motion(coordinate(x), coordinate(y)); });
            };
            result.button = [](void* data, wl_pointer*, std::uint32_t, std::uint32_t,
                               std::uint32_t code, std::uint32_t buttonState) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    unsigned char button = 0;
                    switch (code) {
                        case 0x110: button = 1; break;
                        case 0x112: button = 2; break;
                        case 0x111: button = 3; break;
                        case 0x113: button = 4; break;
                        case 0x114: button = 5; break;
                        default: return;
                    }
                    self.state.button(button, buttonState == WL_POINTER_BUTTON_STATE_PRESSED);
                });
            };
            // Wheel events continue through SDL, not this adapter.
            result.axis = [](void*, wl_pointer*, std::uint32_t, std::uint32_t, wl_fixed_t) {};
            result.frame = [](void*, wl_pointer*) {};
            result.axis_source = [](void*, wl_pointer*, std::uint32_t) {};
            result.axis_stop = [](void*, wl_pointer*, std::uint32_t, std::uint32_t) {};
            result.axis_discrete = [](void*, wl_pointer*, std::uint32_t, std::int32_t) {};
            return result;
        }();
        return listener;
    }

    void destroyPointer() noexcept {
        if (!pointer) return;
        if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION) {
            wl_pointer_release(pointer);
        } else {
            wl_pointer_destroy(pointer);
        }
        pointer = nullptr;
    }

    void destroySeat() noexcept {
        destroyPointer();
        if (!seat) return;
        if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
            wl_seat_release(seat);
        } else {
            wl_seat_destroy(seat);
        }
        seat = nullptr;
    }

    static const wl_seat_listener& seatListener() {
        static const wl_seat_listener listener{
            [](void* data, wl_seat*, std::uint32_t capabilities) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    if (!(capabilities & WL_SEAT_CAPABILITY_POINTER)) {
                        self.destroyPointer();
                        self.state.leave();
                    } else if (!self.pointer) {
                        self.pointer = wl_seat_get_pointer(self.seat);
                        if (!self.pointer ||
                            wl_pointer_add_listener(self.pointer, &pointerListener(), &self) != 0) {
                            throw std::runtime_error("Failed to bind Wayland workspace pointer.");
                        }
                    }
                });
            },
            [](void*, wl_seat*, const char*) {},
        };
        return listener;
    }

    static const wl_registry_listener& registryListener() {
        static const wl_registry_listener listener{
            [](void* data, wl_registry* registry, std::uint32_t name,
               const char* interface, std::uint32_t version) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    if (std::strcmp(interface, wl_seat_interface.name) != 0) return;
                    if (self.selectedSeat) {
                        throw std::runtime_error("Workspace pointer requires an unambiguous single Wayland seat.");
                    }
                    self.selectedSeat = true;
                    self.seatName = name;
                    if (version == 0) throw std::runtime_error("Invalid Wayland seat version.");
                    self.seat = static_cast<wl_seat*>(
                        wl_registry_bind(registry, name, &wl_seat_interface, std::min(version, 5u)));
                    if (!self.seat || wl_seat_add_listener(self.seat, &seatListener(), &self) != 0) {
                        throw std::runtime_error("Failed to bind Wayland workspace seat.");
                    }
                });
            },
            [](void* data, wl_registry*, std::uint32_t name) {
                auto& self = *static_cast<Impl*>(data);
                self.callback([&] {
                    if (!self.selectedSeat || name != self.seatName || !self.seat) return;
                    self.destroySeat();
                    self.state.leave();
                    // Do not switch to another seat or reuse a removed global ID.
                });
            },
        };
        return listener;
    }
#endif

    void stop() noexcept {
#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_WAYLAND)
        destroySeat();
        if (registry) wl_registry_destroy(registry);
        registry = nullptr;
        display = nullptr;
        surface = nullptr;
        selectedSeat = false;
        seatName = 0;
#endif
        state.clear();
        installed = false;
        callbackError = nullptr;
        cancelPending = false;
        windowId = 0;
        ++generation;
    }

    ~Impl() { stop(); }
};

WaylandWorkspacePointer::WaylandWorkspacePointer() : impl_(std::make_unique<Impl>()) {}
WaylandWorkspacePointer::~WaylandWorkspacePointer() = default;

void WaylandWorkspacePointer::start(SDL_Window* window) {
    stop();
#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_WAYLAND)
    try {
        SDL_SysWMinfo info{};
        SDL_VERSION(&info.version);
        if (!window || SDL_GetWindowWMInfo(window, &info) != SDL_TRUE ||
            info.subsystem != SDL_SYSWM_WAYLAND || !info.info.wl.display || !info.info.wl.surface) {
            throw std::runtime_error("Wayland workspace pointer requires an SDL Wayland window.");
        }
        impl_->display = info.info.wl.display;
        impl_->surface = info.info.wl.surface;
        static const Uint32 pointerEventType = SDL_RegisterEvents(1);
        if (pointerEventType == static_cast<Uint32>(-1)) {
            throw std::runtime_error("Cannot register native workspace pointer events.");
        }
        impl_->eventType = pointerEventType;
        impl_->windowId = SDL_GetWindowID(window);
        impl_->registry = wl_display_get_registry(impl_->display);
        if (!impl_->registry ||
            wl_registry_add_listener(impl_->registry, &Impl::registryListener(), impl_.get()) != 0) {
            throw std::runtime_error("Failed to bind Wayland workspace registry.");
        }
        // Registry globals, seat capabilities, then initial pointer events.
        // These startup roundtrips also dispatch SDL's existing default listeners.
        for (int i = 0; i < 3; ++i) {
            if (wl_display_roundtrip(impl_->display) < 0) {
                throw std::runtime_error("Wayland workspace pointer startup roundtrip failed.");
            }
            if (impl_->callbackError) std::rethrow_exception(impl_->callbackError);
        }
        if (!impl_->selectedSeat) throw std::runtime_error("No Wayland workspace seat available.");
        if (!impl_->pointer) throw std::runtime_error("No Wayland workspace pointer available.");
        impl_->installed = true;
    } catch (...) {
        stop();
        throw;
    }
#else
    (void)window;
    throw std::runtime_error("Wayland workspace pointer is unavailable in this build.");
#endif
}

void WaylandWorkspacePointer::stop() { impl_->stop(); }

bool WaylandWorkspacePointer::active() const noexcept { return impl_->installed; }

std::optional<WorkspacePointerEvent> WaylandWorkspacePointer::decodeEvent(const SDL_Event& event) const {
    if (!active() || event.type != impl_->eventType) return std::nullopt;
    PointerMessage message{};
    std::memcpy(&message, &event, sizeof(message));
    if (message.windowId != impl_->windowId || message.generation != impl_->generation) return std::nullopt;
    return message.pointer;
}

void WaylandWorkspacePointer::discardPendingEvents() {
    ++impl_->generation;
    static_cast<void>(impl_->state.takeEvents());
}

std::vector<WorkspacePointerEvent> WaylandWorkspacePointer::takeEvents() {
#if defined(__linux__) && defined(SDL_VIDEO_DRIVER_WAYLAND)
    if (impl_->display && wl_display_get_error(impl_->display) != 0 && !impl_->callbackError) {
        impl_->callback([&] { throw std::runtime_error("Wayland workspace display failed."); });
    }
#endif
    if (impl_->cancelPending) {
        // Allocate outside the C callback; if allocation fails, cancellation is
        // still pending. Stay installed but suppress all input until stop/start.
        std::vector<WorkspacePointerEvent> events{{WorkspacePointerEventType::cancel}};
        impl_->cancelPending = false;
        return events;
    }
    if (impl_->callbackError) std::rethrow_exception(impl_->callbackError);
    return impl_->state.takeEvents();
}

}  // namespace terra
