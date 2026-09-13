#include "input_forwarder.h"
#include "wayland_workspace_pointer.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

extern "C" {
#include <Limelight.h>
}

namespace {
using Kind = terra::WorkspacePointerEventType;
struct Sent {
    char kind;
    int action = 0;
    int code = 0;
    std::array<short, 4> position{};
    bool operator==(const Sent&) const = default;
};
std::vector<Sent> sent;
static std::array<short, 4> lastMousePosition{};
struct Record {
    std::uint64_t generation;
    terra::WorkspacePointerEvent event;
};
struct MockQueue {
    bool active = false;
    Uint32 windowID = 0;
    std::uint64_t generation = 0;
    std::unordered_map<Sint32, Record> records;
};
MockQueue* adapter = nullptr;
Uint32 markerType = 0;
Sint32 nextID = 0;
bool advertiseWayland = false;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void postNative(Kind kind, int x = 0, int y = 0,
                unsigned char button = SDL_BUTTON_LEFT, bool pressed = false) {
    expect(adapter && adapter->active, "Native adapter was not started.");
    const auto id = ++nextID;
    adapter->records.emplace(id, Record{adapter->generation, {kind, x, y, button, pressed}});
    SDL_Event marker{};
    marker.type = markerType;
    marker.user.windowID = adapter->windowID;
    marker.user.code = id;
    marker.user.data1 = adapter;
    expect(SDL_PushEvent(&marker) == 1, "Could not enqueue native marker.");
}
void drain(terra::InputForwarder& input) {
    SDL_Event event{};
    while (SDL_PollEvent(&event)) input.handleEvent(event);
    input.processWorkspacePointerEvents();
    expect(!adapter || adapter->records.empty(), "Native markers were not drained.");
}
SDL_Event key(Uint32 windowID, SDL_Scancode code, bool pressed, Uint16 modifiers = 0) {
    SDL_Event event{};
    event.type = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.windowID = windowID;
    event.key.state = pressed ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.scancode = code;
    event.key.keysym.mod = modifiers;
    return event;
}
void windowEvent(terra::InputForwarder& input, Uint32 windowID, Uint8 kind) {
    SDL_Event event{};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = windowID;
    event.window.event = kind;
    input.handleEvent(event);
}
Sent position(const terra::InputSettings& settings, int x, int y) {
    const auto& local = settings.workspaceMouse.front().local;
    const auto mapped = terra::workspaceMousePosition(settings.workspaceMouse, local.x + x, local.y + y);
    expect(mapped.has_value(), "Test position is outside workspace.");
    return {'P', 0, 0, {mapped->x, mapped->y, mapped->width, mapped->height}};
}
const Sent down{'B', BUTTON_ACTION_PRESS, BUTTON_LEFT};
const Sent up{'B', BUTTON_ACTION_RELEASE, BUTTON_LEFT};
}

// Executable-only interposition. Keep SDL bootstrap on dummy; advertise Wayland
// only around InputForwarder::start(). This tests dispatch, not libwayland focus.
extern "C" const char* SDLCALL SDL_GetCurrentVideoDriver(void) {
    return advertiseWayland ? "wayland" : "dummy";
}

namespace terra {
struct WaylandWorkspacePointer::Impl : MockQueue {};
WaylandWorkspacePointer::WaylandWorkspacePointer() : impl_(std::make_unique<Impl>()) {}
WaylandWorkspacePointer::~WaylandWorkspacePointer() { stop(); }
void WaylandWorkspacePointer::start(SDL_Window* window) {
    stop();
    expect(!adapter, "Only one mock adapter may be active.");
    if (!markerType) {
        markerType = SDL_RegisterEvents(1);
        expect(markerType != static_cast<Uint32>(-1), "Could not register native markers.");
    }
    impl_->active = true;
    impl_->windowID = SDL_GetWindowID(window);
    adapter = impl_.get();
}
void WaylandWorkspacePointer::stop() {
    discardPendingEvents();
    impl_->records.clear();
    impl_->active = false;
    impl_->windowID = 0;
    if (adapter == impl_.get()) adapter = nullptr;
}
bool WaylandWorkspacePointer::active() const noexcept { return impl_->active; }
void WaylandWorkspacePointer::discardPendingEvents() { ++impl_->generation; }
std::optional<WorkspacePointerEvent> WaylandWorkspacePointer::decodeEvent(const SDL_Event& event) const {
    if (!impl_->active || event.type != markerType || event.user.data1 != impl_.get() ||
        event.user.windowID != impl_->windowID) return std::nullopt;
    const auto record = impl_->records.extract(event.user.code);
    if (record.empty() || record.mapped().generation != impl_->generation) return std::nullopt;
    return record.mapped().event;
}
std::vector<WorkspacePointerEvent> WaylandWorkspacePointer::takeEvents() { return {}; }
}

extern "C" int LiSendKeyboardEvent2(short code, char action, char, char) {
    sent.push_back({'K', action, static_cast<unsigned short>(code)});
    return 0;
}
extern "C" int LiSendMouseButtonEvent(char action, int button) {
    sent.push_back({'B', action, button});
    return 0;
}
extern "C" int LiSendMouseMoveEvent(short x, short y) {
    sent.push_back({'M', x, y});
    return 0;
}
extern "C" int LiSendMousePositionEvent(short x, short y, short width, short height) {
    lastMousePosition = {x, y, width, height};
    sent.push_back({'P', 0, 0, lastMousePosition});
    return 0;
}
extern "C" int LiSendHighResScrollEvent(short) { return 0; }
extern "C" int LiSendHighResHScrollEvent(short) { return LI_ERR_UNSUPPORTED; }
extern "C" std::uint32_t LiGetHostFeatureFlags() { return 0; }
extern "C" int LiSendTouchEvent(std::uint8_t, std::uint32_t, float, float, float,
                                float, float, std::uint16_t) { return 0; }
extern "C" int LiSendPenEvent(std::uint8_t, std::uint8_t, std::uint8_t, float, float,
                              float, float, float, std::uint16_t, std::uint8_t) { return 0; }
extern "C" int LiSendControllerTouchEvent2(std::uint8_t, std::uint8_t, std::uint8_t,
                                          std::uint32_t, float, float, float) { return 0; }
extern "C" int LiSendControllerMotionEvent(std::uint8_t, std::uint8_t, float, float, float) { return 0; }
extern "C" int LiSendControllerBatteryEvent(std::uint8_t, std::uint8_t, std::uint8_t) { return 0; }
extern "C" int LiSendUtf8TextEvent(const char*, unsigned int) { return 0; }
extern "C" int LiSendControllerArrivalEvent(std::uint8_t, std::uint16_t, std::uint8_t,
                                           std::uint32_t, std::uint16_t) { return 0; }
extern "C" int LiSendMultiControllerEvent(short, short, int, unsigned char, unsigned char,
                                         short, short, short, short) { return 0; }

int main() {
    SDL_Window* window = nullptr;
    try {
        expect(SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) == 0, "Could not select dummy driver.");
        expect(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL initialization failed.");
        SDL_Rect display{};
        expect(SDL_GetDisplayBounds(0, &display) == 0 && display.w > 1 && display.h > 1,
               "No usable dummy display bounds.");
        window = SDL_CreateWindow("wayland-input-test", display.x, display.y,
                                  display.w, display.h, SDL_WINDOW_SHOWN);
        expect(window != nullptr, "Could not create shown dummy window.");
        expect((SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0,
               "Dummy window must be visible to pass native input guards.");
        const auto windowID = SDL_GetWindowID(window);
        const int x = display.w / 2, y = display.h / 2, siblingX = -display.w / 2;
        terra::InputSettings settings;
        settings.absoluteMouseMode = true;
        settings.fullscreen = true;
        settings.controllersEnabled = false;
        settings.workspaceMouse = {
            {{display.x, display.y, display.w, display.h}, {0, 0, display.w, display.h}},
            {{display.x - display.w, display.y, display.w, display.h}, {-display.w, 0, display.w, display.h}},
        };
        std::uint64_t overlayRevision = 0;
        bool overlayVisible = false;
        terra::InputForwarder input;
        advertiseWayland = true;
        input.start(window, settings, display.w, display.h, 60, {}, {},
                    [&](std::uint64_t revision, bool visible) {
                        overlayRevision = revision;
                        overlayVisible = visible;
                    });
        advertiseWayland = false;
        input.setEnabled(true);
        drain(input);
        expect(adapter && adapter->active, "Forwarder did not select native adapter.");
        const auto source = position(settings, x, y), sibling = position(settings, siblingX, y);
        expect(source.position != sibling.position, "Sibling mapping must differ from source.");
        windowEvent(input, windowID, SDL_WINDOWEVENT_FOCUS_GAINED);
        const auto drag = [&] {
            sent.clear();
            postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
            postNative(Kind::position, siblingX, y);
            postNative(Kind::button, siblingX, y, SDL_BUTTON_LEFT, false);
            drain(input);
            expect(sent == std::vector<Sent>{source, down, sibling, sibling, up},
                   "Drag did not forward exact source-to-sibling position/button sequence.");
            expect(lastMousePosition == sibling.position, "Native motion clamped to source.");
            return sent;
        };
        const auto focusedDrag = drag();
        windowEvent(input, windowID, SDL_WINDOWEVENT_FOCUS_LOST);
        windowEvent(input, windowID, SDL_WINDOWEVENT_FOCUS_GAINED);
        expect(drag() == focusedDrag, "Focus-gained drag differs from already-focused drag.");

        sent.clear();
        input.handleEvent(key(windowID, SDL_SCANCODE_A, true));
        postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
        drain(input);
        expect(sent == std::vector<Sent>{{'K', KEY_ACTION_DOWN, 0x8041}, source, down},
               "Held key and pointer were not forwarded.");
        sent.clear();
        postNative(Kind::position, siblingX, y);
        const auto generation = adapter->generation;
        windowEvent(input, windowID, SDL_WINDOWEVENT_FOCUS_LOST);
        expect(sent == std::vector<Sent>{{'K', KEY_ACTION_UP, 0x8041}} &&
                   adapter->generation == generation, "Keyboard focus loss released native pointer or discarded motion.");
        drain(input);
        expect(sent == std::vector<Sent>{{'K', KEY_ACTION_UP, 0x8041}, sibling},
               "Held native drag stopped on keyboard focus loss.");
        sent.clear();
        postNative(Kind::cancel);
        drain(input);
        postNative(Kind::cancel);
        drain(input);
        expect(sent == std::vector<Sent>{up}, "Pointer cancellation must release exactly once.");

        postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
        drain(input);
        sent.clear();
        postNative(Kind::cancel);
        postNative(Kind::position, x, y);
        postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
        drain(input);
        expect(sent == std::vector<Sent>{up, source, source, down},
               "Ordered cancellation discarded a newer pointer gesture in the same pump.");
        sent.clear();
        postNative(Kind::button, siblingX, y, SDL_BUTTON_LEFT, false);
        drain(input);
        expect(sent == std::vector<Sent>{sibling, up}, "New gesture lost its subsequent release.");

        sent.clear();
        SDL_Event duplicate{};
        duplicate.type = SDL_MOUSEMOTION;
        duplicate.motion.windowID = windowID;
        duplicate.motion.x = x;
        duplicate.motion.y = y;
        expect(SDL_PushEvent(&duplicate) == 1, "Could not queue SDL motion duplicate.");
        for (const bool pressed : {true, false}) {
            duplicate = {};
            duplicate.type = pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            duplicate.button.windowID = windowID;
            duplicate.button.button = SDL_BUTTON_LEFT;
            duplicate.button.state = pressed ? SDL_PRESSED : SDL_RELEASED;
            duplicate.button.x = x;
            duplicate.button.y = y;
            expect(SDL_PushEvent(&duplicate) == 1, "Could not queue SDL button duplicate.");
        }
        drain(input);
        expect(sent.empty(), "SDL mouse duplicates escaped native suppression.");

        // Keep actual window shown: only generation invalidation can reject these
        // pending downs when the queue drains after the explicit window handler.
        for (const Uint8 state : {SDL_WINDOWEVENT_HIDDEN, SDL_WINDOWEVENT_MINIMIZED}) {
            const auto before = adapter->generation;
            postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
            windowEvent(input, windowID, state);
            expect(adapter->generation > before && !adapter->records.empty(),
                   "Window suspension did not invalidate queued native down.");
            windowEvent(input, windowID, SDL_WINDOWEVENT_FOCUS_GAINED);
            drain(input);
            expect(sent.empty(), "Hidden/minimized pending down resurrected after focus gain.");
        }
        const auto shortcut = static_cast<Uint16>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
        for (const auto code : {SDL_SCANCODE_O, SDL_SCANCODE_Z}) {
            postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
            const auto before = adapter->generation;
            input.handleEvent(key(windowID, code, true, shortcut));
            input.handleEvent(key(windowID, code, false));
            expect(adapter->generation > before, "Capture suspension did not invalidate native down.");
            if (code == SDL_SCANCODE_O) {
                expect(overlayVisible, "Overlay shortcut did not suspend capture.");
                input.closeOverlay(overlayRevision);
                expect(!overlayVisible && input.acknowledgeOverlayHidden(overlayRevision),
                       "Overlay capture did not resume.");
            } else {
                input.handleEvent(key(windowID, code, true, shortcut));
                input.handleEvent(key(windowID, code, false));
            }
            drain(input);
            expect(sent.empty(), "Suspended pending down resurrected after capture restoration.");
        }

        auto shiftDown = key(windowID, SDL_SCANCODE_LSHIFT, true, KMOD_LSHIFT);
        auto shiftUp = key(windowID, SDL_SCANCODE_LSHIFT, false);
        expect(SDL_PushEvent(&shiftDown) == 1, "Could not queue modifier down.");
        postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
        expect(SDL_PushEvent(&shiftUp) == 1, "Could not queue modifier up.");
        drain(input);
        expect(sent == std::vector<Sent>{{'K', KEY_ACTION_DOWN, 0x80A0}, source, down,
                                         {'K', KEY_ACTION_UP, 0x80A0}},
               "Native button was reordered past modifier release.");
        sent.clear();
        input.setEnabled(false);
        input.setEnabled(false);
        expect(sent == std::vector<Sent>{up}, "Disable did not release held pointer exactly once.");
        input.setEnabled(true);
        sent.clear();
        postNative(Kind::button, x, y, SDL_BUTTON_LEFT, true);
        drain(input);
        expect(sent == std::vector<Sent>{source, down}, "Restored capture did not accept fresh native down.");
        sent.clear();
        input.stop();
        input.stop();
        expect(!adapter && sent == std::vector<Sent>{up}, "Stop did not reset adapter and release pointer once.");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        advertiseWayland = false;
        std::fprintf(stderr, "%s (SDL: %s)\n", error.what(), SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
}
