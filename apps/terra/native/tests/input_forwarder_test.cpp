#include "input_forwarder.h"
#include "video_renderer.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" {
#include <Limelight.h>
}

namespace {
int arrivals = 0;
int controllerEvents = 0;
int nextControllerResult = 0;
std::uint8_t lastArrivalController = 0;
short lastController = 0;
short lastMask = 0;
int rumbleEvents = 0;
int keyboardEvents = 0;
int mouseMoves = 0;
int mouseButtonEvents = 0;
int touchEvents = 0;
int penEvents = 0;
int controllerTouchEvents = 0;
int controllerMotionEvents = 0;
int controllerBatteryEvents = 0;
int ledEvents = 0;
std::uint16_t lastControllerCapabilities = 0;
std::uint8_t lastBatteryState = 0;
std::uint8_t lastBatteryPercentage = 0;
std::uint32_t hostFeatureFlags = LI_FF_PEN_TOUCH_EVENTS | LI_FF_CONTROLLER_TOUCH_EVENTS;
std::string clipboardText;
std::vector<std::uint8_t> touchTypes;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

#if SDL_VERSION_ATLEAST(2, 24, 0)
int SDLCALL virtualRumble(void*, Uint16, Uint16) {
    ++rumbleEvents;
    return 0;
}

int SDLCALL virtualLed(void*, Uint8, Uint8, Uint8) {
    ++ledEvents;
    return 0;
}
#endif
}

extern "C" int LiSendKeyboardEvent2(short, char, char, char) {
    ++keyboardEvents;
    return 0;
}
extern "C" int LiSendMouseButtonEvent(char, int) {
    ++mouseButtonEvents;
    return 0;
}
extern "C" int LiSendMouseMoveEvent(short, short) {
    ++mouseMoves;
    return 0;
}
extern "C" int LiSendMousePositionEvent(short, short, short, short) { return 0; }
extern "C" int LiSendHighResScrollEvent(short) { return 0; }
extern "C" int LiSendHighResHScrollEvent(short) { return LI_ERR_UNSUPPORTED; }
extern "C" std::uint32_t LiGetHostFeatureFlags() { return hostFeatureFlags; }
extern "C" bool LiGetHdrMetadata(PSS_HDR_METADATA) { return false; }
extern "C" bool LiGetCurrentHostDisplayHdrMode() { return false; }
extern "C" int LiSendTouchEvent(std::uint8_t eventType, std::uint32_t, float, float, float,
                                  float, float, std::uint16_t) {
    ++touchEvents;
    touchTypes.push_back(eventType);
    return 0;
}
extern "C" int LiSendPenEvent(std::uint8_t, std::uint8_t, std::uint8_t, float, float, float,
                                float, float, std::uint16_t, std::uint8_t) {
    ++penEvents;
    return 0;
}
extern "C" int LiSendControllerTouchEvent2(std::uint8_t, std::uint8_t, std::uint8_t,
                                              std::uint32_t, float, float, float) {
    ++controllerTouchEvents;
    return 0;
}
extern "C" int LiSendControllerMotionEvent(std::uint8_t, std::uint8_t, float, float, float) {
    ++controllerMotionEvents;
    return 0;
}
extern "C" int LiSendControllerBatteryEvent(std::uint8_t, std::uint8_t state,
                                               std::uint8_t percentage) {
    ++controllerBatteryEvents;
    lastBatteryState = state;
    lastBatteryPercentage = percentage;
    return 0;
}
extern "C" int LiSendUtf8TextEvent(const char* text, unsigned int length) {
    clipboardText.assign(text, length);
    return 0;
}
extern "C" int LiSendControllerArrivalEvent(std::uint8_t controllerNumber, std::uint16_t mask,
                                               std::uint8_t, std::uint32_t,
                                               std::uint16_t capabilities) {
    ++arrivals;
    lastArrivalController = controllerNumber;
    lastMask = static_cast<short>(mask);
    lastControllerCapabilities = capabilities;
    return 0;
}
extern "C" int LiSendMultiControllerEvent(short controllerNumber, short mask, int, unsigned char,
                                             unsigned char, short, short, short, short) {
    ++controllerEvents;
    lastController = controllerNumber;
    lastMask = mask;
    const int result = nextControllerResult;
    nextControllerResult = 0;
    return result;
}

int main() {
#if defined(TERRA_HAS_LIBPLACEBO)
    const int formatCapabilities = SCM_H264 | SCM_H264_HIGH8_444 | SCM_HEVC |
                                   SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 |
                                   SCM_HEVC_REXT10_444 | SCM_AV1_MAIN8 | SCM_AV1_MAIN10 |
                                   SCM_AV1_HIGH8_444 | SCM_AV1_HIGH10_444;
    expect(terra::selectVideoFormat(terra::VideoCodec::automatic, formatCapabilities, false,
                                      true) == VIDEO_FORMAT_H264_HIGH8_444,
           "Automatic Linux 4:4:4 selection did not prefer H.264.");
    expect(terra::selectVideoFormat(terra::VideoCodec::automatic, formatCapabilities, true,
                                      false) == VIDEO_FORMAT_H265_MAIN10,
           "Automatic Linux HDR selection did not choose HEVC Main10.");
    expect(terra::selectVideoFormat(terra::VideoCodec::automatic, formatCapabilities, true,
                                      true) == VIDEO_FORMAT_H265_REXT10_444,
           "Combined Linux HDR 4:4:4 selection did not choose HEVC RExt10.");
    expect(terra::selectVideoFormat(terra::VideoCodec::av1, formatCapabilities, true, true) ==
               VIDEO_FORMAT_AV1_HIGH10_444,
           "Explicit AV1 HDR 4:4:4 selection used wrong profile.");
    bool rejectedH264Hdr = false;
    try {
        static_cast<void>(terra::selectVideoFormat(terra::VideoCodec::h264,
                                                      formatCapabilities, true, false));
    } catch (const std::runtime_error&) {
        rejectedH264Hdr = true;
    }
    expect(rejectedH264Hdr, "H.264 HDR selection was not rejected.");
    bool rejectedMissingProfile = false;
    try {
        static_cast<void>(terra::selectVideoFormat(terra::VideoCodec::hevc, SCM_HEVC,
                                                      false, true));
    } catch (const std::runtime_error&) {
        rejectedMissingProfile = true;
    }
    expect(rejectedMissingProfile, "Missing HEVC 4:4:4 host profile was not rejected.");
#endif

    expect(terra::prepareClipboardText("line 1\r\nline 2") == "line 1\nline 2",
           "Clipboard newline normalization failed.");
    expect(terra::prepareClipboardText("\xF0\x9F\x8C\x91").has_value(),
           "Valid non-ASCII clipboard text was rejected.");
    expect(!terra::prepareClipboardText("\xF0\x28\x8C\x28"),
           "Malformed UTF-8 clipboard text was accepted.");
    expect(!terra::prepareClipboardText(std::string(16 * 1024 + 1, 'x')),
           "Oversized clipboard text was accepted.");

    SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
    expect(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) == 0, "SDL initialization failed.");

#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_VirtualJoystickDesc description{};
    description.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    description.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    description.naxes = SDL_CONTROLLER_AXIS_MAX;
    description.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    description.button_mask = (1U << SDL_CONTROLLER_BUTTON_MAX) - 1;
    description.axis_mask = (1U << SDL_CONTROLLER_AXIS_MAX) - 1;
    description.name = "Terra test controller";
    description.Rumble = virtualRumble;
    description.SetLED = virtualLed;

    std::array<int, 2> devices{
        SDL_JoystickAttachVirtualEx(&description),
        SDL_JoystickAttachVirtualEx(&description),
    };
    expect(devices[0] >= 0 && devices[1] >= 0, "Could not attach virtual controllers.");
    expect(terra::connectedGamepadMask() == 3, "Launch mask did not include both controllers.");

    std::array<SDL_JoystickID, 2> instanceIds{
        SDL_JoystickGetDeviceInstanceID(devices[0]),
        SDL_JoystickGetDeviceInstanceID(devices[1]),
    };
#endif
    auto* window = SDL_CreateWindow("input-test", 0, 0, 1280, 720, SDL_WINDOW_HIDDEN);
    expect(window != nullptr, "Could not create test window.");

    terra::InputSettings settings;
    settings.backgroundGamepad = true;
    terra::InputForwarder input;
    int statisticsToggles = 0;
    int fullscreenToggles = 0;
    std::vector<std::pair<std::uint64_t, bool>> overlayStates;
    std::vector<std::pair<std::uint64_t, bool>> overlayCaptureStates;
    input.start(window, settings, 1920, 1080, 60, [&] { ++statisticsToggles; }, [&] {
        ++fullscreenToggles;
        return fullscreenToggles % 2 != 0;
    }, [&](std::uint64_t revision, bool visible) {
        overlayStates.emplace_back(revision, visible);
    }, [&](std::uint64_t revision, bool suspended) {
        overlayCaptureStates.emplace_back(revision, suspended);
    });
    input.setEnabled(true);
    SDL_Event event{};
#if SDL_VERSION_ATLEAST(2, 24, 0)
    expect(arrivals == 2, "Both controllers were not announced.");
    expect(lastArrivalController == 1 && lastMask == 3,
           "Second controller arrival used wrong slot or mask.");
    expect((lastControllerCapabilities & LI_CCAP_RGB_LED) != 0,
           "Virtual controller LED capability was not announced.");

    input.setGamepadLed(1, 10, 20, 30);
    expect(ledEvents == 1, "Controller LED command was not routed by slot.");

    event = {};
    event.type = SDL_CONTROLLERTOUCHPADDOWN;
    event.ctouchpad.which = instanceIds[1];
    event.ctouchpad.touchpad = 0;
    event.ctouchpad.finger = 2;
    event.ctouchpad.x = 0.25F;
    event.ctouchpad.y = 0.75F;
    event.ctouchpad.pressure = 0.5F;
    input.handleEvent(event);
    event.type = SDL_CONTROLLERTOUCHPADUP;
    input.handleEvent(event);
    expect(controllerTouchEvents == 2, "Controller touchpad events were not forwarded.");

    event = {};
    event.type = SDL_JOYBATTERYUPDATED;
    event.jbattery.which = instanceIds[1];
    event.jbattery.level = SDL_JOYSTICK_POWER_LOW;
    input.handleEvent(event);
    expect(controllerBatteryEvents == 1 && lastBatteryState == LI_BATTERY_STATE_DISCHARGING &&
               lastBatteryPercentage == 20,
           "Controller battery event was not mapped correctly.");

    event = {};
    event.type = SDL_CONTROLLERTOUCHPADDOWN;
    event.ctouchpad.which = instanceIds[1];
    event.ctouchpad.touchpad = 0;
    event.ctouchpad.finger = 3;
    input.handleEvent(event);
    input.setEnabled(false);
    expect(controllerTouchEvents == 4,
           "Disabling input did not cancel active controller touchpad contact.");
    input.setEnabled(true);

    event = {};
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.which = instanceIds[1];
    event.cbutton.state = SDL_PRESSED;
    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    input.handleEvent(event);
    expect(lastController == 1 && lastMask == 3,
            "Second controller input used wrong slot or mask.");

    nextControllerResult = -1;
    event.type = SDL_CONTROLLERBUTTONUP;
    event.cbutton.state = SDL_RELEASED;
    input.handleEvent(event);
    const int eventsAfterFailure = controllerEvents;
    input.updateGamepads();
    expect(controllerEvents == eventsAfterFailure + 1 && lastController == 1,
           "Failed controller update was not retried.");
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.which = instanceIds[0];
    event.cbutton.state = SDL_PRESSED;
    input.handleEvent(event);
    expect(lastController == 0 && lastMask == 3 && terra::gamepadTransportAvailable(),
           "One failed controller update disabled remaining controller transport.");

    input.setGamepadRumble(1, 100, 200);
    expect(rumbleEvents == 1, "Second controller did not receive rumble.");

    nextControllerResult = -1;
    expect(SDL_JoystickDetachVirtual(devices[1]) == 0, "Could not detach second controller.");
    event = {};
    event.type = SDL_CONTROLLERDEVICEREMOVED;
    event.cdevice.which = instanceIds[1];
    input.handleEvent(event);
    const int eventsAfterRemovalFailure = controllerEvents;
    input.updateGamepads();
    expect(controllerEvents == eventsAfterRemovalFailure + 1,
           "Failed controller removal was not retried.");
    expect(lastController == 1 && lastMask == 1,
           "Controller removal did not publish updated mask.");
#endif

    SDL_FlushEvent(SDL_QUIT);
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_Q;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(keyboardEvents == 0, "Local disconnect shortcut leaked to remote keyboard.");
    bool quitRequested = false;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) quitRequested = true;
    }
    expect(quitRequested, "Local disconnect shortcut did not request window close.");

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_S;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(statisticsToggles == 1, "Local statistics shortcut did not toggle the overlay.");

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_X;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(fullscreenToggles == 1, "Local fullscreen shortcut did not toggle window mode.");

    const int keyboardEventsBeforeRelease = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_Z;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeRelease,
           "Local mouse-release shortcut leaked to remote keyboard.");

    const int keyboardEventsBeforeMouseMode = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_M;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeMouseMode,
           "Local mouse-mode shortcut leaked to remote keyboard.");

    const int cursorState = SDL_ShowCursor(SDL_QUERY);
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_C;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(SDL_ShowCursor(SDL_QUERY) != cursorState,
           "Local cursor shortcut did not toggle cursor visibility.");

    const int keyboardEventsBeforePointerLock = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_L;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforePointerLock,
            "Local pointer-lock shortcut leaked to remote keyboard.");

    event = {};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    input.handleEvent(event);
    const int keyboardEventsBeforeOverlay = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    event.key.repeat = 1;
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    event.key.repeat = 0;
    input.handleEvent(event);
    expect(overlayStates == std::vector<std::pair<std::uint64_t, bool>>{{1, true}},
           "Local stream-overlay shortcut did not publish visible revision 1 once.");
    expect(keyboardEvents == keyboardEventsBeforeOverlay,
           "Local stream-overlay shortcut leaked to remote keyboard.");

    const int mouseButtonsBeforeOverlayInput = mouseButtonEvents;
    event = {};
    event.type = SDL_MOUSEBUTTONUP;
    event.button.windowID = SDL_GetWindowID(window);
    event.button.state = SDL_RELEASED;
    event.button.button = SDL_BUTTON_LEFT;
    input.handleEvent(event);
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.state = SDL_PRESSED;
    input.handleEvent(event);
    expect(mouseButtonEvents == mouseButtonsBeforeOverlayInput,
           "Mouse click recaptured input or leaked while stream overlay was open.");

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_A;
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeOverlay,
           "Keyboard input leaked to the host while stream overlay was open.");

    const std::array overlayModifiers{
        SDL_SCANCODE_LCTRL,
        SDL_SCANCODE_LALT,
        SDL_SCANCODE_LSHIFT,
    };
    for (const auto modifier : overlayModifiers) {
        event = {};
        event.type = SDL_KEYDOWN;
        event.key.windowID = SDL_GetWindowID(window);
        event.key.state = SDL_PRESSED;
        event.key.keysym.scancode = modifier;
        input.handleEvent(event);
    }
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(overlayStates.back() == std::pair<std::uint64_t, bool>{2, false},
           "Stream-overlay shortcut did not publish hidden revision 2 on key press.");
    expect(input.acknowledgeOverlayHidden(2),
           "Matching hidden revision was not acknowledged.");
    expect(overlayCaptureStates.empty(),
           "Capture suspension cleared before overlay toggle key was released.");
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_B;
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeOverlay,
           "Stream input resumed before overlay toggle key was released.");
    event = {};
    event.type = SDL_KEYUP;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_RELEASED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    input.handleEvent(event);
    expect(overlayCaptureStates ==
               std::vector<std::pair<std::uint64_t, bool>>{{2, false}},
           "Capture restoration was not reported after overlay toggle key release.");

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(overlayStates.back() == std::pair<std::uint64_t, bool>{3, true},
           "Held overlay modifiers did not publish visible revision 3.");
    expect(!input.acknowledgeOverlayHidden(2),
           "Stale hidden revision was accepted after reopening overlay.");
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(overlayStates.back() == std::pair<std::uint64_t, bool>{4, false},
           "Held overlay modifiers did not publish hidden revision 4.");
    expect(input.acknowledgeOverlayHidden(4),
           "Latest hidden revision was not acknowledged.");
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(overlayCaptureStates ==
               std::vector<std::pair<std::uint64_t, bool>>{{2, false}, {4, false}},
           "Latest overlay capture restoration was not reported.");

    for (const auto modifier : overlayModifiers) {
        event = {};
        event.type = SDL_KEYUP;
        event.key.windowID = SDL_GetWindowID(window);
        event.key.state = SDL_RELEASED;
        event.key.keysym.scancode = modifier;
        input.handleEvent(event);
    }

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_Z;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(overlayStates.back() == std::pair<std::uint64_t, bool>{5, true},
           "Overlay shortcut did not remain armed after automatic capture was lost.");
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    event.type = SDL_KEYDOWN;
    event.key.state = SDL_PRESSED;
    input.handleEvent(event);
    expect(overlayStates.back() == std::pair<std::uint64_t, bool>{6, false},
           "Armed overlay shortcut did not close without mouse recapture.");
    expect(input.acknowledgeOverlayHidden(6),
           "Armed overlay close did not accept matching hidden revision.");
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);

    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeOverlay + 2,
           "Plain O was incorrectly handled as a local shortcut.");

    const int keyboardEventsBeforeMinimize = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_D;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeMinimize,
           "Local minimize shortcut leaked to remote keyboard.");

    const int keyboardEventsBeforeF8 = keyboardEvents;
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_F8;
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(keyboardEvents == keyboardEventsBeforeF8 + 2,
           "Removed F8 placeholder was still handled locally.");

    expect(SDL_SetClipboardText("hello\r\nworld") == 0, "Could not seed test clipboard.");
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_V;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    expect(clipboardText == "hello\nworld", "Clipboard shortcut did not send normalized text.");

    input.stop();
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const int arrivalsBeforeDisabledControllers = arrivals;
    const int controllerEventsBeforeDisabledControllers = controllerEvents;
    const int rumbleEventsBeforeDisabledControllers = rumbleEvents;
    settings.controllersEnabled = false;
    input.start(window, settings, 1920, 1080, 60);
    input.setEnabled(true);
    event = {};
    event.type = SDL_CONTROLLERBUTTONDOWN;
    event.cbutton.which = instanceIds[0];
    event.cbutton.state = SDL_PRESSED;
    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
    input.handleEvent(event);
    input.updateGamepads();
    input.setGamepadRumble(0, 100, 200);
    expect(arrivals == arrivalsBeforeDisabledControllers &&
               controllerEvents == controllerEventsBeforeDisabledControllers &&
               rumbleEvents == rumbleEventsBeforeDisabledControllers,
           "Disabled controller role still emitted controller traffic.");
    input.stop();
    settings.controllersEnabled = true;
#endif
    settings.touchscreenTrackpad = false;
    input.start(window, settings, 1920, 1080, 60);
    input.setEnabled(true);
    event = {};
    event.type = SDL_FINGERDOWN;
    event.tfinger.touchId = 42;
    event.tfinger.fingerId = 7;
    event.tfinger.x = 0.5F;
    event.tfinger.y = 0.5F;
    event.tfinger.pressure = 0.75F;
    input.handleEvent(event);
    event.type = SDL_FINGERMOTION;
    event.tfinger.x = 0.6F;
    input.handleEvent(event);
    event.type = SDL_FINGERUP;
    input.handleEvent(event);
    expect(touchEvents == 3 && touchTypes.front() == LI_TOUCH_EVENT_DOWN &&
               touchTypes.back() == LI_TOUCH_EVENT_UP,
           "Direct touch events were not forwarded.");

    event = {};
    event.type = SDL_FINGERDOWN;
    event.tfinger.touchId = 42;
    event.tfinger.fingerId = 8;
    event.tfinger.x = 0.5F;
    event.tfinger.y = 0.5F;
    input.handleEvent(event);
    event = {};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    input.handleEvent(event);
    expect(touchTypes.back() == LI_TOUCH_EVENT_CANCEL_ALL,
           "Focus loss did not cancel active direct touches.");

    input.stop();
    settings.touchscreenTrackpad = true;
    input.start(window, settings, 1920, 1080, 60);
    input.setEnabled(true);
    event = {};
    event.type = SDL_FINGERDOWN;
    event.tfinger.timestamp = 100;
    event.tfinger.touchId = 42;
    event.tfinger.fingerId = 9;
    event.tfinger.x = 0.4F;
    event.tfinger.y = 0.4F;
    input.handleEvent(event);
    event.type = SDL_FINGERMOTION;
    event.tfinger.timestamp = 150;
    event.tfinger.x = 0.5F;
    input.handleEvent(event);
    expect(mouseMoves > 0, "Virtual trackpad motion was not forwarded.");
    event.type = SDL_FINGERUP;
    event.tfinger.timestamp = 200;
    input.handleEvent(event);
    expect(mouseButtonEvents == 0, "Moved virtual-trackpad gesture produced a tap.");

    event = {};
    event.type = SDL_FINGERDOWN;
    event.tfinger.timestamp = 300;
    event.tfinger.touchId = 42;
    event.tfinger.fingerId = 10;
    event.tfinger.x = 0.5F;
    event.tfinger.y = 0.5F;
    input.handleEvent(event);
    event.type = SDL_FINGERUP;
    event.tfinger.timestamp = 400;
    input.handleEvent(event);
    expect(mouseButtonEvents == 2, "Virtual-trackpad tap was not forwarded.");

    input.stop();
    settings.absoluteMouseMode = true;
    int ignoredOverlayOpens = 0;
    input.start(window, settings, 1920, 1080, 60, {}, {},
                [&](std::uint64_t, bool) { ++ignoredOverlayOpens; });
    input.setEnabled(true);
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(ignoredOverlayOpens == 0, "Stream overlay opened without window focus.");

    event = {};
    event.type = SDL_WINDOWEVENT;
    event.window.windowID = SDL_GetWindowID(window);
    event.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    input.handleEvent(event);
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_Z;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.windowID = SDL_GetWindowID(window);
    event.key.state = SDL_PRESSED;
    event.key.keysym.scancode = SDL_SCANCODE_O;
    event.key.keysym.mod = static_cast<SDL_Keymod>(KMOD_LCTRL | KMOD_LALT | KMOD_LSHIFT);
    input.handleEvent(event);
    event.type = SDL_KEYUP;
    event.key.state = SDL_RELEASED;
    input.handleEvent(event);
    expect(ignoredOverlayOpens == 0, "Stream overlay opened without captured input.");
    input.stop();
    SDL_DestroyWindow(window);
#if SDL_VERSION_ATLEAST(2, 24, 0)
    expect(SDL_JoystickDetachVirtual(devices[0]) == 0, "Could not detach first controller.");
#endif
    SDL_Quit();
}
