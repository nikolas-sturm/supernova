#include "input_forwarder.h"

#include <utility>

namespace terra {
bool gamepadTransportAvailable() noexcept { return true; }

std::optional<std::string> prepareClipboardText(std::string_view text) {
    constexpr std::size_t maximumBytes = 16 * 1024;
    if (text.empty() || text.size() > maximumBytes || text.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        std::size_t length = 0;
        if (first <= 0x7F) {
            length = 1;
        } else if (first >= 0xC2 && first <= 0xDF) {
            length = 2;
        } else if (first >= 0xE0 && first <= 0xEF) {
            length = 3;
        } else if (first >= 0xF0 && first <= 0xF4) {
            length = 4;
        } else {
            return std::nullopt;
        }
        if (index + length > text.size()) return std::nullopt;
        for (std::size_t continuation = 1; continuation < length; ++continuation) {
            const auto value = static_cast<unsigned char>(text[index + continuation]);
            if ((value & 0xC0U) != 0x80U) return std::nullopt;
        }
        if (length == 3) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xE0 && second < 0xA0) || (first == 0xED && second >= 0xA0)) {
                return std::nullopt;
            }
        } else if (length == 4) {
            const auto second = static_cast<unsigned char>(text[index + 1]);
            if ((first == 0xF0 && second < 0x90) || (first == 0xF4 && second >= 0x90)) {
                return std::nullopt;
            }
        }
        index += length;
    }

    std::string normalized;
    normalized.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\r' && index + 1 < text.size() && text[index + 1] == '\n') continue;
        normalized.push_back(text[index]);
    }
    return normalized.empty() ? std::nullopt
                              : std::optional<std::string>{std::move(normalized)};
}
}  // namespace terra

#ifdef _WIN32

#include <Limelight.h>
#include <Xinput.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace terra {
namespace {

constexpr UINT_PTR kGamepadTimer = 1;
constexpr UINT kGamepadPollMilliseconds = 8;
constexpr UINT kHookKeyboardMessage = WM_APP + 20;
constexpr std::uint32_t kSupportedGamepadButtons =
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
    RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;
using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using XInputGetCapabilitiesFunction = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);

XInputGetStateFunction loadXInputGetState(HMODULE module) {
    const FARPROC procedure = GetProcAddress(module, "XInputGetState");
    XInputGetStateFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(procedure));
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
}

XInputSetStateFunction loadXInputSetState(HMODULE module) {
    const FARPROC procedure = GetProcAddress(module, "XInputSetState");
    XInputSetStateFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(procedure));
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
}

XInputGetCapabilitiesFunction loadXInputGetCapabilities(HMODULE module) {
    const FARPROC procedure = GetProcAddress(module, "XInputGetCapabilities");
    XInputGetCapabilitiesFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(procedure));
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
}

short invertStickAxis(SHORT value) {
    return static_cast<short>(std::clamp(-static_cast<int>(value), -32768, 32767));
}

int terraButtons(WORD buttons, bool swapFaceButtons) {
    int result = 0;
    result |= (buttons & XINPUT_GAMEPAD_A) != 0 ? (swapFaceButtons ? B_FLAG : A_FLAG) : 0;
    result |= (buttons & XINPUT_GAMEPAD_B) != 0 ? (swapFaceButtons ? A_FLAG : B_FLAG) : 0;
    result |= (buttons & XINPUT_GAMEPAD_X) != 0 ? (swapFaceButtons ? Y_FLAG : X_FLAG) : 0;
    result |= (buttons & XINPUT_GAMEPAD_Y) != 0 ? (swapFaceButtons ? X_FLAG : Y_FLAG) : 0;
    result |= (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0 ? UP_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0 ? DOWN_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0 ? LEFT_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0 ? RIGHT_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0 ? LB_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0 ? RB_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_START) != 0 ? PLAY_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_BACK) != 0 ? BACK_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0 ? LS_CLK_FLAG : 0;
    result |= (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0 ? RS_CLK_FLAG : 0;
    return result;
}

short normalizedVirtualKey(WPARAM wparam, LPARAM lparam) {
    short key = static_cast<short>(wparam);
    const bool extended = (lparam & (1LL << 24)) != 0;
    if (key == VK_SHIFT) {
        const UINT scanCode = static_cast<UINT>((lparam >> 16) & 0xff);
        key = static_cast<short>(MapVirtualKeyW(scanCode, MAPVK_VSC_TO_VK_EX));
    } else if (key == VK_CONTROL) {
        key = extended ? VK_RCONTROL : VK_LCONTROL;
    } else if (key == VK_MENU) {
        key = extended ? VK_RMENU : VK_LMENU;
    }
    return key;
}

}  // namespace

std::uint16_t connectedGamepadMask() {
    if (!gamepadTransportAvailable()) return 0;
    constexpr std::array<const wchar_t*, 3> libraries = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* library : libraries) {
        const HMODULE module = LoadLibraryW(library);
        if (!module) continue;
        const auto getState = loadXInputGetState(module);
        std::uint16_t mask = 0;
        if (getState) {
            for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
                XINPUT_STATE state{};
                if (getState(index, &state) == ERROR_SUCCESS) {
                    mask |= static_cast<std::uint16_t>(1U << index);
                }
            }
        }
        FreeLibrary(module);
        if (getState) return mask;
    }
    return 0;
}

struct InputForwarder::Impl {
    struct PointerState {
        float lastX = 0;
        float lastY = 0;
    };

    inline static Impl* keyboardHookOwner = nullptr;
    HWND window = nullptr;
    InputSettings settings;
    std::wstring streamLabel;
    bool enabled = false;
    bool mouseCaptured = false;
    bool cursorHidden = false;
    int streamWidth = 1;
    int streamHeight = 1;
    std::unordered_set<short> keysDown;
    std::unordered_set<short> consumedShortcutKeys;
    std::unordered_set<int> mouseButtonsDown;
    std::unordered_map<UINT32, PointerState> pointers;
    std::optional<UINT32> primaryPointer;
    std::optional<UINT32> fallbackMousePointer;
    DWORD gestureStartedAt = 0;
    std::size_t gestureMaximumPointers = 0;
    bool gestureMoved = false;
    float gestureDistance = 0;
    bool penContactActive = false;
    HHOOK keyboardHook = nullptr;
    HMODULE xinputModule = nullptr;
    XInputGetStateFunction xinputGetState = nullptr;
    XInputSetStateFunction xinputSetState = nullptr;
    XInputGetCapabilitiesFunction xinputGetCapabilities = nullptr;
    std::array<bool, XUSER_MAX_COUNT> gamepadConnected{};
    std::array<XINPUT_STATE, XUSER_MAX_COUNT> gamepadStates{};
    bool gamepadSuppressed = false;
    std::optional<POINT> absoluteMousePosition;
    std::optional<int> inputResult;
    std::function<void()> toggleStatistics;
    std::function<bool()> toggleFullscreen;
    OverlayListener overlayListener;
    OverlayCaptureListener overlayCaptureListener;
    bool localCursorVisible = true;
    bool pointerRegionLocked = false;
    bool absoluteInputCaptured = true;
    bool overlayCaptureSuspended = false;
    bool overlayDesiredVisible = false;
    bool overlayShortcutArmed = false;
    bool overlayResumePending = false;
    std::uint64_t overlayRevision = 0;
    bool restoreRelativeCapture = false;
    bool restoreAbsoluteCapture = false;

    void updateWindowTitle() const {
        if (!window) return;
        std::wstring title = L"Terra Stream [" + streamLabel + L"] - ";
        if (inputResult && *inputResult != 0) {
            title += L"Input queue failed (" + std::to_wstring(*inputResult) + L")";
        } else if (!enabled) {
            title += L"Input waiting for connection";
        } else if (settings.absoluteMouseMode) {
            title += absoluteInputCaptured ? L"Absolute pointer active" : L"Click to capture input";
        } else {
            title += mouseCaptured ? L"Input captured" : L"Click to capture input";
        }
        if (mouseCaptured) title += L" (Ctrl+Alt+Shift+Z to release)";
        SetWindowTextW(window, title.c_str());
    }

    void noteInputResult(int result) {
        if (!inputResult || (*inputResult == 0 && result != 0)) {
            inputResult = result;
            updateWindowTitle();
        }
    }

    void noteControllerResult(int result) {
        noteInputResult(result);
    }

    std::uint16_t gamepadCapabilities(DWORD index, bool physical) const {
        std::uint16_t capabilities = LI_CCAP_ANALOG_TRIGGERS;
        XINPUT_CAPABILITIES reported{};
        if (physical && xinputSetState && xinputGetCapabilities &&
            xinputGetCapabilities(index, XINPUT_FLAG_GAMEPAD, &reported) == ERROR_SUCCESS &&
            (reported.Flags & XINPUT_CAPS_FFB_SUPPORTED) != 0) {
            capabilities |= LI_CCAP_RUMBLE;
        }
        return capabilities;
    }

    std::uint16_t gamepadMask() const {
        std::uint16_t mask = settings.forceGamepad ? 1 : 0;
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            if (gamepadConnected[index]) mask |= static_cast<std::uint16_t>(1U << index);
        }
        return mask;
    }

    void stopGamepadRumble(DWORD index) const {
        if (!xinputSetState || index >= XUSER_MAX_COUNT) return;
        XINPUT_VIBRATION vibration{};
        xinputSetState(index, &vibration);
    }

    void stopGamepadRumble() const {
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) stopGamepadRumble(index);
    }

    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                           std::uint16_t highFrequency) {
        if (!enabled || controllerNumber >= XUSER_MAX_COUNT ||
            !gamepadConnected[controllerNumber] || !xinputSetState) {
            return;
        }
        XINPUT_VIBRATION vibration{
            .wLeftMotorSpeed = lowFrequency,
            .wRightMotorSpeed = highFrequency,
        };
        xinputSetState(controllerNumber, &vibration);
    }

    char modifiers() const {
        char result = 0;
        if (keysDown.contains(VK_LSHIFT) || keysDown.contains(VK_RSHIFT)) {
            result |= MODIFIER_SHIFT;
        }
        if (keysDown.contains(VK_LCONTROL) || keysDown.contains(VK_RCONTROL)) {
            result |= MODIFIER_CTRL;
        }
        if (keysDown.contains(VK_LMENU) || keysDown.contains(VK_RMENU)) {
            result |= MODIFIER_ALT;
        }
        if (systemKeyCaptureActive() &&
            (keysDown.contains(VK_LWIN) || keysDown.contains(VK_RWIN))) {
            result |= MODIFIER_META;
        }
        return result;
    }

    bool systemKeyCaptureActive() const {
        if (!enabled || !window || GetForegroundWindow() != window ||
            (settings.absoluteMouseMode ? !absoluteInputCaptured : !mouseCaptured) ||
            settings.captureSystemKeys == SystemKeyCapture::off) {
            return false;
        }
        return settings.captureSystemKeys == SystemKeyCapture::always || settings.fullscreen;
    }

    bool overlayShortcutActive() const {
        return enabled && window && overlayListener &&
               (overlayCaptureSuspended ||
                (GetForegroundWindow() == window &&
                 (overlayShortcutArmed ||
                  (settings.absoluteMouseMode ? absoluteInputCaptured : mouseCaptured))));
    }

    bool overlayChordActive() const {
        return overlayShortcutActive() && (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
               (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 &&
               (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    }

    void updateCursorClip() const {
        if (!window || (!mouseCaptured && !(settings.absoluteMouseMode && pointerRegionLocked))) {
            return;
        }
        RECT rect = settings.absoluteMouseMode ? videoBounds() : RECT{};
        if (!settings.absoluteMouseMode && !GetClientRect(window, &rect)) return;
        POINT topLeft{rect.left, rect.top};
        POINT bottomRight{rect.right, rect.bottom};
        if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return;
        rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ClipCursor(&rect);
    }

    RECT videoBounds() const {
        RECT client{};
        if (!window || !GetClientRect(window, &client)) return {0, 0, 1, 1};
        const int clientWidth = std::max<int>(client.right - client.left, 1);
        const int clientHeight = std::max<int>(client.bottom - client.top, 1);
        const double inputAspect = static_cast<double>(streamWidth) / streamHeight;
        const double outputAspect = static_cast<double>(clientWidth) / clientHeight;
        if (inputAspect > outputAspect) {
            const int height = std::max(static_cast<int>(clientWidth / inputAspect), 1);
            client.top = (clientHeight - height) / 2;
            client.bottom = client.top + height;
        } else {
            const int width = std::max(static_cast<int>(clientHeight * inputAspect), 1);
            client.left = (clientWidth - width) / 2;
            client.right = client.left + width;
        }
        return client;
    }

    std::pair<float, float> normalizedPointer(POINT point) const {
        const auto bounds = videoBounds();
        return {
            std::clamp(static_cast<float>(point.x - bounds.left) /
                           std::max<LONG>(bounds.right - bounds.left, 1),
                       0.0F, 1.0F),
            std::clamp(static_cast<float>(point.y - bounds.top) /
                           std::max<LONG>(bounds.bottom - bounds.top, 1),
                       0.0F, 1.0F),
        };
    }

    void releaseRemoteState() {
        for (int attempt = 0; attempt < 20 && (!keysDown.empty() || !mouseButtonsDown.empty());
             ++attempt) {
            for (auto key = keysDown.begin(); key != keysDown.end();) {
                const int input = LiSendKeyboardEvent(
                    static_cast<short>(0x8000 | *key), KEY_ACTION_UP, 0);
                if (input == 0) {
                    key = keysDown.erase(key);
                } else {
                    noteInputResult(input);
                    ++key;
                }
            }
            for (auto button = mouseButtonsDown.begin(); button != mouseButtonsDown.end();) {
                const int input = LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, *button);
                if (input == 0) {
                    button = mouseButtonsDown.erase(button);
                } else {
                    noteInputResult(input);
                    ++button;
                }
            }
            if (!keysDown.empty() || !mouseButtonsDown.empty()) Sleep(1);
        }
    }

    void cancelPointerState() {
        if (!pointers.empty() && !settings.touchscreenTrackpad &&
            (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            const int input = LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0, 0, 0, 0, 0,
                                               LI_ROT_UNKNOWN);
            if (input != LI_ERR_UNSUPPORTED) noteInputResult(input);
        }
        if (fallbackMousePointer) {
            sendMouseButton(BUTTON_LEFT, false);
        }
        if (penContactActive && (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            const int input = LiSendPenEvent(LI_TOUCH_EVENT_CANCEL, LI_TOOL_TYPE_UNKNOWN, 0, 0, 0,
                                             0, 0, 0, LI_ROT_UNKNOWN, LI_TILT_UNKNOWN);
            if (input != LI_ERR_UNSUPPORTED) noteInputResult(input);
        }
        pointers.clear();
        primaryPointer.reset();
        fallbackMousePointer.reset();
        gestureMaximumPointers = 0;
        gestureMoved = false;
        gestureDistance = 0;
        penContactActive = false;
    }

    void setMouseCaptured(bool capture) {
        if (mouseCaptured == capture || !window) return;
        mouseCaptured = capture;
        absoluteMousePosition.reset();
        if (capture) {
            SetFocus(window);
            SetCapture(window);
            updateCursorClip();
            if (!cursorHidden) {
                ShowCursor(FALSE);
                cursorHidden = true;
            }
        } else {
            releaseRemoteState();
            ClipCursor(nullptr);
            if (GetCapture() == window) ReleaseCapture();
            if (cursorHidden) {
                ShowCursor(TRUE);
                cursorHidden = false;
            }
        }
        updateWindowTitle();
    }

    void toggleMouseMode() {
        if (!settings.absoluteMouseMode) setMouseCaptured(false);
        settings.absoluteMouseMode = !settings.absoluteMouseMode;
        if (settings.absoluteMouseMode) absoluteInputCaptured = true;
        if (!settings.absoluteMouseMode) setMouseCaptured(true);
        updateCursorClip();
        updateWindowTitle();
    }

    void toggleInputCapture() {
        if (!settings.absoluteMouseMode) {
            setMouseCaptured(!mouseCaptured);
            return;
        }
        absoluteInputCaptured = !absoluteInputCaptured;
        if (absoluteInputCaptured) {
            SetFocus(window);
            updateCursorClip();
        } else {
            releaseRemoteState();
            ClipCursor(nullptr);
        }
        updateWindowTitle();
    }

    void suspendForOverlay() {
        if (!overlayCaptureSuspended) {
            restoreRelativeCapture = !settings.absoluteMouseMode && mouseCaptured;
            restoreAbsoluteCapture = settings.absoluteMouseMode && absoluteInputCaptured;
        }
        overlayCaptureSuspended = true;
        overlayResumePending = false;
        overlayShortcutArmed = true;
        cancelPointerState();
        gamepadSuppressed = false;
        if (restoreRelativeCapture) {
            setMouseCaptured(false);
        } else if (restoreAbsoluteCapture) {
            releaseRemoteState();
            absoluteInputCaptured = false;
            ClipCursor(nullptr);
            updateWindowTitle();
        }
    }

    void resumeAfterOverlay() {
        if (!overlayCaptureSuspended) return;
        if (consumedShortcutKeys.contains('O')) {
            overlayResumePending = true;
            return;
        }
        overlayResumePending = false;
        if (!enabled || !window) {
            overlayCaptureSuspended = false;
            restoreRelativeCapture = false;
            restoreAbsoluteCapture = false;
            if (overlayCaptureListener) overlayCaptureListener(overlayRevision, false);
            return;
        }
        if (GetForegroundWindow() != window) {
            overlayResumePending = true;
            return;
        }
        const bool relative = std::exchange(restoreRelativeCapture, false);
        const bool absolute = std::exchange(restoreAbsoluteCapture, false);
        overlayCaptureSuspended = false;
        if (relative && !settings.absoluteMouseMode) {
            setMouseCaptured(true);
        } else if (absolute && settings.absoluteMouseMode) {
            absoluteInputCaptured = true;
            updateCursorClip();
            updateWindowTitle();
        }
        if (overlayCaptureListener) overlayCaptureListener(overlayRevision, false);
    }

    void publishOverlayState() {
        if (overlayListener) overlayListener(overlayRevision, overlayDesiredVisible);
    }

    void toggleOverlay() {
        const bool previous = overlayDesiredVisible;
        if (!previous) suspendForOverlay();
        overlayDesiredVisible = !previous;
        ++overlayRevision;
        try {
            publishOverlayState();
        } catch (...) {
            overlayDesiredVisible = previous;
            if (!previous) resumeAfterOverlay();
            throw;
        }
    }

    void closeOverlay(std::uint64_t revision) {
        if (!overlayDesiredVisible || revision != overlayRevision) return;
        overlayDesiredVisible = false;
        ++overlayRevision;
        publishOverlayState();
    }

    bool acknowledgeOverlayHidden(std::uint64_t revision) {
        if (overlayDesiredVisible || revision != overlayRevision) return false;
        resumeAfterOverlay();
        return true;
    }

    void updateOverlay() {
        if (!overlayDesiredVisible) return;
        ++overlayRevision;
        publishOverlayState();
    }

    void toggleCursorDisplay() {
        if (!settings.absoluteMouseMode) return;
        localCursorVisible = !localCursorVisible;
        SetCursor(localCursorVisible ? LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)) : nullptr);
    }

    void togglePointerRegionLock() {
        if (!settings.absoluteMouseMode) return;
        pointerRegionLocked = !pointerRegionLocked;
        if (pointerRegionLocked) {
            updateCursorClip();
        } else {
            ClipCursor(nullptr);
        }
    }

    void sendClipboardText() {
        if (!window || !OpenClipboard(window)) return;
        std::optional<std::string> prepared;
        const HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle));
            const auto bytes = GlobalSize(handle);
            if (wide && bytes >= sizeof(wchar_t)) {
                const auto capacity = bytes / sizeof(wchar_t);
                std::size_t length = 0;
                while (length < capacity && wide[length] != L'\0') ++length;
                if (length < capacity && length <= 16 * 1024) {
                    const int utf8Length = WideCharToMultiByte(
                        CP_UTF8, WC_ERR_INVALID_CHARS, wide, static_cast<int>(length), nullptr, 0,
                        nullptr, nullptr);
                    if (utf8Length > 0 && utf8Length <= 16 * 1024) {
                        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
                        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide,
                                                static_cast<int>(length), utf8.data(), utf8Length,
                                                nullptr, nullptr) == utf8Length) {
                            prepared = prepareClipboardText(utf8);
                        }
                    }
                }
            }
            if (wide) GlobalUnlock(handle);
        }
        CloseClipboard();
        if (prepared) {
            noteInputResult(LiSendUtf8TextEvent(prepared->data(),
                                                static_cast<unsigned int>(prepared->size())));
        }
    }

    void handleKeyboardKey(short key, bool pressed, bool repeated) {
        if (!pressed && consumedShortcutKeys.erase(key) != 0) {
            if (key == 'O' && overlayResumePending) resumeAfterOverlay();
            return;
        }
        if (pressed && consumedShortcutKeys.contains(key)) return;
        const bool shortcutModifiers =
            ((GetKeyState(VK_CONTROL) & 0x8000) != 0 ||
             (modifiers() & MODIFIER_CTRL) != 0) &&
            ((GetKeyState(VK_MENU) & 0x8000) != 0 || (modifiers() & MODIFIER_ALT) != 0) &&
            ((GetKeyState(VK_SHIFT) & 0x8000) != 0 || (modifiers() & MODIFIER_SHIFT) != 0);
        const bool overlayShortcut = key == 'O' && overlayShortcutActive();
        if (pressed && !repeated && shortcutModifiers &&
            (key == 'Q' || key == 'Z' || key == 'X' || key == 'S' || key == 'M' ||
             key == 'V' || key == 'D' || key == 'C' || key == 'L' || overlayShortcut)) {
            consumedShortcutKeys.insert(key);
            releaseRemoteState();
            if (key == 'Q') {
                PostMessageW(window, WM_CLOSE, 0, 0);
            } else if (key == 'X') {
                if (toggleFullscreen) settings.fullscreen = toggleFullscreen();
            } else if (key == 'S') {
                if (toggleStatistics) toggleStatistics();
            } else if (key == 'Z') {
                toggleInputCapture();
            } else if (key == 'M') {
                toggleMouseMode();
            } else if (key == 'V') {
                sendClipboardText();
            } else if (key == 'D') {
                ShowWindow(window, SW_MINIMIZE);
            } else if (key == 'C') {
                toggleCursorDisplay();
            } else if (key == 'L') {
                togglePointerRegionLock();
            } else if (overlayShortcut) {
                try {
                    toggleOverlay();
                } catch (...) {
                }
            }
            return;
        }
        if (pressed && repeated) return;
        if (overlayCaptureSuspended) return;
        if (settings.absoluteMouseMode && !absoluteInputCaptured) return;
        if (!pressed && !keysDown.contains(key)) return;
        const int input = LiSendKeyboardEvent(static_cast<short>(0x8000 | key),
                                              pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                              modifiers());
        noteInputResult(input);
        if (input != 0) return;
        if (pressed) keysDown.insert(key);
        else keysDown.erase(key);
    }

    void handleKeyboard(WPARAM wparam, LPARAM lparam, bool pressed) {
        handleKeyboardKey(normalizedVirtualKey(wparam, lparam), pressed,
                          pressed && (lparam & (1LL << 30)) != 0);
    }

    static LRESULT CALLBACK keyboardHookProcedure(int code, WPARAM wparam, LPARAM lparam) noexcept {
        auto* self = keyboardHookOwner;
        if (code < 0 || !self) {
            return CallNextHookEx(self ? self->keyboardHook : nullptr, code, wparam, lparam);
        }
        const auto* input = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        if ((input->flags & LLKHF_INJECTED) != 0) {
            return CallNextHookEx(self->keyboardHook, code, wparam, lparam);
        }
        const bool pressed = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool released = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
        if (!pressed && !released) return CallNextHookEx(self->keyboardHook, code, wparam, lparam);
        short key = static_cast<short>(input->vkCode);
        if (key == VK_SHIFT) {
            key = static_cast<short>(MapVirtualKeyW(input->scanCode, MAPVK_VSC_TO_VK_EX));
        } else if (key == VK_CONTROL) {
            key = (input->flags & LLKHF_EXTENDED) != 0 ? VK_RCONTROL : VK_LCONTROL;
        } else if (key == VK_MENU) {
            key = (input->flags & LLKHF_EXTENDED) != 0 ? VK_RMENU : VK_LMENU;
        }
        const bool overlayKey = key == 'O' &&
                                (self->consumedShortcutKeys.contains('O') ||
                                 (pressed && self->overlayChordActive()));
        if (!self->systemKeyCaptureActive() && !overlayKey) {
            return CallNextHookEx(self->keyboardHook, code, wparam, lparam);
        }
        const LPARAM state = pressed ? 1 : 0;
        return PostMessageW(self->window, kHookKeyboardMessage, static_cast<WPARAM>(key), state)
                   ? 1
                   : CallNextHookEx(self->keyboardHook, code, wparam, lparam);
    }

    void sendMouseButton(int button, bool pressed) {
        if (settings.swapMouseButtons) {
            if (button == BUTTON_LEFT) {
                button = BUTTON_RIGHT;
            } else if (button == BUTTON_RIGHT) {
                button = BUTTON_LEFT;
            }
        }
        if (!pressed && !mouseButtonsDown.contains(button)) return;
        const int input =
            LiSendMouseButtonEvent(pressed ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button);
        noteInputResult(input);
        if (input != 0) return;
        if (pressed) mouseButtonsDown.insert(button);
        else mouseButtonsDown.erase(button);
    }

    void handleRawInput(HRAWINPUT handle) {
        if (!mouseCaptured || settings.absoluteMouseMode) return;
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 ||
            size == 0) {
            return;
        }
        std::vector<std::uint8_t> buffer(size);
        if (GetRawInputData(handle, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) !=
            size) {
            return;
        }
        const auto* input = reinterpret_cast<const RAWINPUT*>(buffer.data());
        if (input->header.dwType != RIM_TYPEMOUSE) return;
        const RAWMOUSE& mouse = input->data.mouse;
        if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
            const bool virtualDesktop = (mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
            const int originX = virtualDesktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
            const int originY = virtualDesktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
            const int width = GetSystemMetrics(virtualDesktop ? SM_CXVIRTUALSCREEN : SM_CXSCREEN);
            const int height = GetSystemMetrics(virtualDesktop ? SM_CYVIRTUALSCREEN : SM_CYSCREEN);
            const POINT position{
                originX + MulDiv(mouse.lLastX, width - 1, 65535),
                originY + MulDiv(mouse.lLastY, height - 1, 65535),
            };
            if (absoluteMousePosition) {
                const LONG deltaX = position.x - absoluteMousePosition->x;
                const LONG deltaY = position.y - absoluteMousePosition->y;
                if (deltaX != 0 || deltaY != 0) {
                    noteInputResult(LiSendMouseMoveEvent(
                        static_cast<short>(std::clamp(deltaX, -32768L, 32767L)),
                        static_cast<short>(std::clamp(deltaY, -32768L, 32767L))));
                }
            }
            absoluteMousePosition = position;
        } else if (mouse.lLastX != 0 || mouse.lLastY != 0) {
            noteInputResult(
                LiSendMouseMoveEvent(static_cast<short>(std::clamp(mouse.lLastX, -32768L, 32767L)),
                                     static_cast<short>(
                                         std::clamp(mouse.lLastY, -32768L, 32767L))));
        }
        const USHORT flags = mouse.usButtonFlags;
        if (flags & RI_MOUSE_LEFT_BUTTON_DOWN) sendMouseButton(BUTTON_LEFT, true);
        if (flags & RI_MOUSE_LEFT_BUTTON_UP) sendMouseButton(BUTTON_LEFT, false);
        if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN) sendMouseButton(BUTTON_RIGHT, true);
        if (flags & RI_MOUSE_RIGHT_BUTTON_UP) sendMouseButton(BUTTON_RIGHT, false);
        if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) sendMouseButton(BUTTON_MIDDLE, true);
        if (flags & RI_MOUSE_MIDDLE_BUTTON_UP) sendMouseButton(BUTTON_MIDDLE, false);
        if (flags & RI_MOUSE_BUTTON_4_DOWN) sendMouseButton(BUTTON_X1, true);
        if (flags & RI_MOUSE_BUTTON_4_UP) sendMouseButton(BUTTON_X1, false);
        if (flags & RI_MOUSE_BUTTON_5_DOWN) sendMouseButton(BUTTON_X2, true);
        if (flags & RI_MOUSE_BUTTON_5_UP) sendMouseButton(BUTTON_X2, false);
        if (flags & RI_MOUSE_WHEEL) {
            const auto amount = static_cast<short>(mouse.usButtonData);
            noteInputResult(LiSendHighResScrollEvent(settings.reverseScrollDirection
                                                         ? static_cast<short>(-static_cast<int>(amount))
                                                         : amount));
        }
        if (flags & RI_MOUSE_HWHEEL) {
            const auto amount = static_cast<short>(mouse.usButtonData);
            noteInputResult(LiSendHighResHScrollEvent(settings.reverseScrollDirection
                                                          ? static_cast<short>(-static_cast<int>(amount))
                                                          : amount));
        }
    }

    void sendAbsoluteMousePosition(LPARAM value) {
        if (!settings.absoluteMouseMode || !window) return;
        const auto bounds = videoBounds();
        const int width = std::min<int>(std::max<LONG>(bounds.right - bounds.left, 1), 32767);
        const int height = std::min<int>(std::max<LONG>(bounds.bottom - bounds.top, 1), 32767);
        const auto x = static_cast<short>(
            std::clamp(GET_X_LPARAM(value) - bounds.left, 0L, bounds.right - bounds.left) * width /
            std::max<LONG>(bounds.right - bounds.left, 1));
        const auto y = static_cast<short>(
            std::clamp(GET_Y_LPARAM(value) - bounds.top, 0L, bounds.bottom - bounds.top) * height /
            std::max<LONG>(bounds.bottom - bounds.top, 1));
        noteInputResult(LiSendMousePositionEvent(
            x, y, static_cast<short>(width), static_cast<short>(height)));
    }

    void emulateDirectPointer(UINT32 pointerId, std::uint8_t eventType, float x, float y) {
        if (eventType == LI_TOUCH_EVENT_DOWN && !fallbackMousePointer) {
            fallbackMousePointer = pointerId;
        }
        if (!fallbackMousePointer || *fallbackMousePointer != pointerId) return;
        const short width = static_cast<short>(std::min(streamWidth, 32767));
        const short height = static_cast<short>(std::min(streamHeight, 32767));
        noteInputResult(LiSendMousePositionEvent(
            static_cast<short>(std::lround(x * width)),
            static_cast<short>(std::lround(y * height)), width, height));
        if (eventType == LI_TOUCH_EVENT_DOWN) {
            sendMouseButton(BUTTON_LEFT, true);
        } else if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
            sendMouseButton(BUTTON_LEFT, false);
            if (!mouseButtonsDown.contains(BUTTON_LEFT)) fallbackMousePointer.reset();
        }
    }

    void handleTrackpadPointer(UINT32 pointerId, std::uint8_t eventType, float x, float y,
                               DWORD timestamp) {
        if (eventType == LI_TOUCH_EVENT_DOWN) {
            if (pointers.empty()) {
                gestureStartedAt = timestamp;
                gestureMaximumPointers = 0;
                gestureMoved = false;
                gestureDistance = 0;
            }
            pointers[pointerId] = {x, y};
            if (!primaryPointer) primaryPointer = pointerId;
            gestureMaximumPointers = std::max(gestureMaximumPointers, pointers.size());
            return;
        }
        const auto pointer = pointers.find(pointerId);
        if (pointer == pointers.end()) return;
        if (eventType == LI_TOUCH_EVENT_MOVE) {
            const float deltaX = x - pointer->second.lastX;
            const float deltaY = y - pointer->second.lastY;
            pointer->second = {x, y};
            gestureDistance += std::abs(deltaX) + std::abs(deltaY);
            if (gestureDistance > 0.003F) gestureMoved = true;
            if (primaryPointer && *primaryPointer == pointerId &&
                (deltaX != 0 || deltaY != 0)) {
                noteInputResult(LiSendMouseMoveEvent(
                    static_cast<short>(std::clamp(
                        static_cast<int>(std::lround(deltaX * streamWidth * 1.5F)), -32768,
                        32767)),
                    static_cast<short>(std::clamp(
                        static_cast<int>(std::lround(deltaY * streamHeight * 1.5F)), -32768,
                        32767))));
            }
            return;
        }
        pointers.erase(pointer);
        if (eventType == LI_TOUCH_EVENT_CANCEL) gestureMoved = true;
        if (primaryPointer && *primaryPointer == pointerId) {
            primaryPointer = pointers.empty() ? std::nullopt
                                              : std::optional<UINT32>{pointers.begin()->first};
        }
        if (!pointers.empty()) return;
        if (!gestureMoved && timestamp - gestureStartedAt <= 300) {
            const int button = gestureMaximumPointers >= 2 ? BUTTON_RIGHT : BUTTON_LEFT;
            sendMouseButton(button, true);
            sendMouseButton(button, false);
        }
        gestureMaximumPointers = 0;
        gestureMoved = false;
        gestureDistance = 0;
    }

    bool handlePointer(UINT message, WPARAM wparam) {
        if (overlayCaptureSuspended) return true;
        const UINT32 pointerId = GET_POINTERID_WPARAM(wparam);
        POINTER_INPUT_TYPE pointerType = PT_POINTER;
        if (!GetPointerType(pointerId, &pointerType)) return false;
        std::uint8_t eventType = message == WM_POINTERDOWN ? LI_TOUCH_EVENT_DOWN
                                 : message == WM_POINTERUP ? LI_TOUCH_EVENT_UP
                                                            : LI_TOUCH_EVENT_MOVE;
        if (pointerType == PT_TOUCH) {
            POINTER_TOUCH_INFO touch{};
            if (!GetPointerTouchInfo(pointerId, &touch)) return false;
            if ((touch.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0) {
                eventType = LI_TOUCH_EVENT_CANCEL;
            }
            POINT point = touch.pointerInfo.ptPixelLocation;
            if (!ScreenToClient(window, &point)) return false;
            const auto [x, y] = normalizedPointer(point);
            if (settings.touchscreenTrackpad) {
                handleTrackpadPointer(pointerId, eventType, x, y, touch.pointerInfo.dwTime);
                return true;
            }
            const auto bounds = videoBounds();
            const float contactWidth =
                (touch.touchMask & TOUCH_MASK_CONTACTAREA) != 0
                    ? static_cast<float>(touch.rcContact.right - touch.rcContact.left) /
                          std::max<LONG>(bounds.right - bounds.left, 1)
                    : 0.0F;
            const float contactHeight =
                (touch.touchMask & TOUCH_MASK_CONTACTAREA) != 0
                    ? static_cast<float>(touch.rcContact.bottom - touch.rcContact.top) /
                          std::max<LONG>(bounds.bottom - bounds.top, 1)
                    : 0.0F;
            const float pressure = (touch.touchMask & TOUCH_MASK_PRESSURE) != 0
                                       ? std::clamp(touch.pressure / 1024.0F, 0.0F, 1.0F)
                                       : 0.0F;
            const auto rotation = (touch.touchMask & TOUCH_MASK_ORIENTATION) != 0
                                      ? static_cast<std::uint16_t>((touch.orientation + 270) % 360)
                                      : static_cast<std::uint16_t>(LI_ROT_UNKNOWN);
            int input = LI_ERR_UNSUPPORTED;
            if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
                input = LiSendTouchEvent(eventType, pointerId, x, y, pressure,
                                         std::max(contactWidth, contactHeight),
                                         std::min(contactWidth, contactHeight), rotation);
            }
            if (input == LI_ERR_UNSUPPORTED) {
                emulateDirectPointer(pointerId, eventType, x, y);
            } else {
                noteInputResult(input);
            }
            if (eventType == LI_TOUCH_EVENT_DOWN) pointers[pointerId] = {x, y};
            if (eventType == LI_TOUCH_EVENT_UP || eventType == LI_TOUCH_EVENT_CANCEL) {
                pointers.erase(pointerId);
            }
            return true;
        }
        if (pointerType != PT_PEN) return false;
        POINTER_PEN_INFO pen{};
        if (!GetPointerPenInfo(pointerId, &pen)) return false;
        POINT point = pen.pointerInfo.ptPixelLocation;
        if (!ScreenToClient(window, &point)) return false;
        const auto [x, y] = normalizedPointer(point);
        const bool inContact = (pen.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        const std::uint8_t penEventType =
            (pen.pointerInfo.pointerFlags & POINTER_FLAG_CANCELED) != 0
                ? LI_TOUCH_EVENT_CANCEL
            : message == WM_POINTERUP   ? LI_TOUCH_EVENT_UP
            : message == WM_POINTERDOWN ? LI_TOUCH_EVENT_DOWN
            : inContact                 ? LI_TOUCH_EVENT_MOVE
                                        : LI_TOUCH_EVENT_HOVER;
        const std::uint8_t tool = (pen.penFlags & PEN_FLAG_ERASER) != 0 ? LI_TOOL_TYPE_ERASER
                                                                        : LI_TOOL_TYPE_PEN;
        const std::uint8_t buttons = (pen.penFlags & PEN_FLAG_BARREL) != 0
                                         ? LI_PEN_BUTTON_SECONDARY
                                         : 0;
        const float pressure = (pen.penMask & PEN_MASK_PRESSURE) != 0
                                   ? std::clamp(pen.pressure / 1024.0F, 0.0F, 1.0F)
                                   : 0.0F;
        const auto rotation = (pen.penMask & PEN_MASK_ROTATION) != 0
                                   ? static_cast<std::uint16_t>((pen.rotation + 270) % 360)
                                   : static_cast<std::uint16_t>(LI_ROT_UNKNOWN);
        const auto tilt = (pen.penMask & (PEN_MASK_TILT_X | PEN_MASK_TILT_Y)) != 0
                              ? static_cast<std::uint8_t>(std::clamp(
                                    static_cast<int>(std::lround(std::hypot(
                                        static_cast<double>(pen.tiltX),
                                        static_cast<double>(pen.tiltY)))),
                                    0, 90))
                              : static_cast<std::uint8_t>(LI_TILT_UNKNOWN);
        int input = LI_ERR_UNSUPPORTED;
        if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            input = LiSendPenEvent(penEventType, tool, buttons, x, y, pressure, 0, 0, rotation,
                                   tilt);
        }
        if (input == LI_ERR_UNSUPPORTED) {
            emulateDirectPointer(pointerId, penEventType, x, y);
        } else {
            noteInputResult(input);
        }
        if (input == 0 && (penEventType == LI_TOUCH_EVENT_DOWN ||
                           (penEventType == LI_TOUCH_EVENT_MOVE && inContact))) {
            penContactActive = true;
        } else if (penEventType == LI_TOUCH_EVENT_UP || penEventType == LI_TOUCH_EVENT_CANCEL) {
            penContactActive = false;
        }
        return true;
    }

    void sendWindowScroll(WPARAM value, bool horizontal) {
        auto amount = GET_WHEEL_DELTA_WPARAM(value);
        if (settings.reverseScrollDirection) amount = -amount;
        noteInputResult(horizontal ? LiSendHighResHScrollEvent(static_cast<short>(amount))
                                   : LiSendHighResScrollEvent(static_cast<short>(amount)));
    }

    void pollGamepad() {
        if (!enabled || !xinputGetState || !gamepadTransportAvailable()) return;
        if (overlayCaptureSuspended ||
            (!settings.backgroundGamepad && GetForegroundWindow() != window)) {
            if (!gamepadSuppressed) {
                const auto mask = gamepadMask();
                bool sent = true;
                for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
                    if ((mask & (1U << index)) == 0) continue;
                    const int result = LiSendMultiControllerEvent(
                        static_cast<short>(index), static_cast<short>(mask), 0, 0, 0, 0, 0, 0,
                        0);
                    noteControllerResult(result);
                    sent = result == 0 && sent;
                }
                gamepadSuppressed = sent;
            }
            return;
        }
        const bool wasSuppressed = gamepadSuppressed;
        gamepadSuppressed = false;
        std::array<bool, XUSER_MAX_COUNT> connected{};
        std::array<XINPUT_STATE, XUSER_MAX_COUNT> states{};
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            connected[index] = xinputGetState(index, &states[index]) == ERROR_SUCCESS;
        }
        std::uint16_t nextMask = settings.forceGamepad ? 1 : 0;
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            if (connected[index]) nextMask |= static_cast<std::uint16_t>(1U << index);
        }

        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            if (gamepadConnected[index] && !connected[index]) {
                stopGamepadRumble(index);
                const int result = LiSendMultiControllerEvent(
                    static_cast<short>(index), static_cast<short>(nextMask), 0, 0, 0, 0, 0, 0,
                    0);
                noteControllerResult(result);
                if (result == 0) {
                    gamepadConnected[index] = false;
                    gamepadStates[index] = {};
                }
                continue;
            }
            if (!gamepadConnected[index] && connected[index]) {
                const int result = LiSendControllerArrivalEvent(
                    static_cast<std::uint8_t>(index), nextMask, LI_CTYPE_XBOX,
                    kSupportedGamepadButtons, gamepadCapabilities(index, true));
                noteControllerResult(result);
                if (result != 0) continue;
                gamepadConnected[index] = true;
                gamepadStates[index].dwPacketNumber = states[index].dwPacketNumber - 1;
            }
            if (!connected[index] || (!wasSuppressed && states[index].dwPacketNumber ==
                                                            gamepadStates[index].dwPacketNumber)) {
                continue;
            }
            const auto& pad = states[index].Gamepad;
            const int result = LiSendMultiControllerEvent(
                static_cast<short>(index), static_cast<short>(nextMask),
                terraButtons(pad.wButtons, settings.swapFaceButtons), pad.bLeftTrigger,
                pad.bRightTrigger, pad.sThumbLX, invertStickAxis(pad.sThumbLY), pad.sThumbRX,
                invertStickAxis(pad.sThumbRY));
            noteControllerResult(result);
            if (result == 0) gamepadStates[index] = states[index];
        }
    }

    void loadXInput() {
        constexpr std::array<const wchar_t*, 3> libraries = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* library : libraries) {
            xinputModule = LoadLibraryW(library);
            if (!xinputModule) continue;
            xinputGetState = loadXInputGetState(xinputModule);
            xinputSetState = loadXInputSetState(xinputModule);
            xinputGetCapabilities = loadXInputGetCapabilities(xinputModule);
            if (xinputGetState) return;
            FreeLibrary(xinputModule);
            xinputModule = nullptr;
            xinputSetState = nullptr;
        }
    }
};

InputForwarder::InputForwarder() : impl_(std::make_unique<Impl>()) {}

InputForwarder::~InputForwarder() { stop(); }

void InputForwarder::start(HWND window, InputSettings settings, int width, int height, int fps,
                           std::function<void()> toggleStatistics,
                           std::function<bool()> toggleFullscreen,
                           OverlayListener overlayListener,
                           OverlayCaptureListener overlayCaptureListener,
                           InputOverlayState overlayState) {
    if (impl_->window) throw std::runtime_error("Input forwarding is already active");
    impl_->window = window;
    impl_->settings = settings;
    impl_->toggleStatistics = std::move(toggleStatistics);
    impl_->toggleFullscreen = std::move(toggleFullscreen);
    impl_->overlayListener = std::move(overlayListener);
    impl_->overlayCaptureListener = std::move(overlayCaptureListener);
    impl_->overlayRevision = overlayState.revision;
    impl_->overlayDesiredVisible = overlayState.visible;
    impl_->overlayCaptureSuspended = overlayState.captureSuspended;
    impl_->overlayShortcutArmed = overlayState.visible || overlayState.captureSuspended;
    impl_->restoreRelativeCapture = overlayState.captureSuspended && !settings.absoluteMouseMode;
    impl_->restoreAbsoluteCapture = overlayState.captureSuspended && settings.absoluteMouseMode;
    impl_->streamWidth = std::max(width, 1);
    impl_->streamHeight = std::max(height, 1);
    impl_->streamLabel = std::to_wstring(width) + L"x" + std::to_wstring(height) + L" @ " +
                         std::to_wstring(fps) + L" FPS";
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01;
    mouse.usUsage = 0x02;
    mouse.hwndTarget = window;
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse))) {
        impl_->window = nullptr;
        throw std::runtime_error("Could not register raw mouse input");
    }
    Impl::keyboardHookOwner = impl_.get();
    impl_->keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, Impl::keyboardHookProcedure,
                                            GetModuleHandleW(nullptr), 0);
    if (!impl_->keyboardHook) {
        Impl::keyboardHookOwner = nullptr;
        impl_->window = nullptr;
        throw std::runtime_error("Could not install local shortcut hook");
    }
    if (settings.controllersEnabled) {
        impl_->loadXInput();
        SetTimer(window, kGamepadTimer, kGamepadPollMilliseconds, nullptr);
    }
    impl_->updateWindowTitle();
}

void InputForwarder::closeOverlay(std::uint64_t revision) { impl_->closeOverlay(revision); }

bool InputForwarder::acknowledgeOverlayHidden(std::uint64_t revision) {
    return impl_->acknowledgeOverlayHidden(revision);
}

void InputForwarder::updateOverlay() { impl_->updateOverlay(); }

void InputForwarder::setEnabled(bool enabled) {
    if (!impl_->window || impl_->enabled == enabled) return;
    if (enabled) {
        impl_->enabled = true;
        if (impl_->settings.controllersEnabled) impl_->pollGamepad();
        if (impl_->settings.controllersEnabled && impl_->settings.forceGamepad &&
            !impl_->gamepadConnected[0] &&
            gamepadTransportAvailable()) {
            impl_->noteControllerResult(LiSendControllerArrivalEvent(
                0, 1, LI_CTYPE_XBOX, kSupportedGamepadButtons,
                impl_->gamepadCapabilities(0, false)));
        }
    } else {
        impl_->cancelPointerState();
        impl_->stopGamepadRumble();
        impl_->setMouseCaptured(false);
        impl_->releaseRemoteState();
        const auto mask = impl_->gamepadMask();
        if (impl_->settings.controllersEnabled && gamepadTransportAvailable()) {
            for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
                if ((mask & (1U << index)) == 0) continue;
                impl_->noteControllerResult(LiSendMultiControllerEvent(
                    static_cast<short>(index), 0, 0, 0, 0, 0, 0, 0, 0));
            }
        }
        impl_->gamepadConnected = {};
        impl_->gamepadStates = {};
        impl_->gamepadSuppressed = false;
        impl_->enabled = false;
    }
    impl_->updateWindowTitle();
}

void InputForwarder::setGamepadRumble(std::uint16_t controllerNumber,
                                      std::uint16_t lowFrequency,
                                      std::uint16_t highFrequency) {
    impl_->setGamepadRumble(controllerNumber, lowFrequency, highFrequency);
}

void InputForwarder::setGamepadTriggerRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}

void InputForwarder::setGamepadMotionEventState(std::uint16_t, std::uint8_t, std::uint16_t) {}

void InputForwarder::setGamepadLed(std::uint16_t, std::uint8_t, std::uint8_t, std::uint8_t) {}

void InputForwarder::updateGamepads() {}

void InputForwarder::stop() {
    if (!impl_->window) return;
    KillTimer(impl_->window, kGamepadTimer);
    setEnabled(false);
    if (impl_->keyboardHook) {
        UnhookWindowsHookEx(impl_->keyboardHook);
        impl_->keyboardHook = nullptr;
        if (Impl::keyboardHookOwner == impl_.get()) Impl::keyboardHookOwner = nullptr;
    }
    impl_->window = nullptr;
    impl_->toggleStatistics = {};
    impl_->toggleFullscreen = {};
    impl_->overlayListener = {};
    impl_->overlayCaptureListener = {};
    impl_->consumedShortcutKeys.clear();
    impl_->overlayCaptureSuspended = false;
    impl_->overlayDesiredVisible = false;
    impl_->overlayShortcutArmed = false;
    impl_->overlayResumePending = false;
    impl_->overlayRevision = 0;
    impl_->restoreRelativeCapture = false;
    impl_->restoreAbsoluteCapture = false;
    if (impl_->xinputModule) {
        FreeLibrary(impl_->xinputModule);
        impl_->xinputModule = nullptr;
        impl_->xinputGetState = nullptr;
        impl_->xinputSetState = nullptr;
        impl_->xinputGetCapabilities = nullptr;
    }
}

bool InputForwarder::handleMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (!impl_->enabled) {
        if (message == WM_TIMER && wparam == kGamepadTimer) {
            result = 0;
            return true;
        }
        return false;
    }
    if (impl_->overlayCaptureSuspended &&
        (message == WM_INPUT || (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
         message == WM_POINTERDOWN || message == WM_POINTERUPDATE || message == WM_POINTERUP)) {
        result = 0;
        return true;
    }
    switch (message) {
        case kHookKeyboardMessage: {
            const auto key = static_cast<short>(wparam);
            const bool pressed = (lparam & 1) != 0;
            impl_->handleKeyboardKey(
                key, pressed,
                pressed && (impl_->keysDown.contains(key) ||
                            impl_->consumedShortcutKeys.contains(key)));
            result = 0;
            return true;
        }
        case WM_INPUT:
            impl_->handleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
            result = 0;
            return true;
        case WM_KEYDOWN:
            if ((wparam == VK_LWIN || wparam == VK_RWIN) &&
                !impl_->systemKeyCaptureActive()) {
                return false;
            }
            impl_->handleKeyboard(wparam, lparam, true);
            result = 0;
            return true;
        case WM_SYSKEYDOWN:
            if (!impl_->systemKeyCaptureActive()) return false;
            impl_->handleKeyboard(wparam, lparam, true);
            result = 0;
            return true;
        case WM_KEYUP:
            if ((wparam == VK_LWIN || wparam == VK_RWIN) &&
                !impl_->systemKeyCaptureActive()) {
                return false;
            }
            impl_->handleKeyboard(wparam, lparam, false);
            result = 0;
            return true;
        case WM_SYSKEYUP:
            if (!impl_->systemKeyCaptureActive()) return false;
            impl_->handleKeyboard(wparam, lparam, false);
            result = 0;
            return true;
        case WM_LBUTTONDOWN:
            if (impl_->settings.absoluteMouseMode) {
                if (!impl_->absoluteInputCaptured) {
                    impl_->absoluteInputCaptured = true;
                    impl_->updateCursorClip();
                    impl_->updateWindowTitle();
                    result = 0;
                    return true;
                }
                impl_->sendMouseButton(BUTTON_LEFT, true);
            } else if (!impl_->mouseCaptured) {
                impl_->setMouseCaptured(true);
            }
            result = 0;
            return true;
        case WM_LBUTTONUP:
            if (impl_->settings.absoluteMouseMode) impl_->sendMouseButton(BUTTON_LEFT, false);
            result = 0;
            return impl_->settings.absoluteMouseMode;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            if (impl_->settings.absoluteMouseMode && impl_->absoluteInputCaptured) {
                impl_->sendMouseButton(BUTTON_RIGHT, message == WM_RBUTTONDOWN);
                result = 0;
                return true;
            }
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (impl_->settings.absoluteMouseMode && impl_->absoluteInputCaptured) {
                impl_->sendMouseButton(BUTTON_MIDDLE, message == WM_MBUTTONDOWN);
                result = 0;
                return true;
            }
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if (impl_->settings.absoluteMouseMode && impl_->absoluteInputCaptured) {
                const auto button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? BUTTON_X1 : BUTTON_X2;
                impl_->sendMouseButton(button, message == WM_XBUTTONDOWN);
                result = TRUE;
                return true;
            }
            break;
        case WM_MOUSEMOVE:
            if (impl_->settings.absoluteMouseMode && impl_->absoluteInputCaptured) {
                impl_->sendAbsoluteMousePosition(lparam);
                result = 0;
                return true;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (impl_->settings.absoluteMouseMode && impl_->absoluteInputCaptured) {
                impl_->sendWindowScroll(wparam, message == WM_MOUSEHWHEEL);
                result = 0;
                return true;
            }
            break;
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE:
        case WM_POINTERUP:
            if (impl_->handlePointer(message, wparam)) {
                result = 0;
                return true;
            }
            break;
        case WM_POINTERCAPTURECHANGED:
            impl_->cancelPointerState();
            break;
        case WM_ACTIVATEAPP:
            if (wparam == FALSE) {
                impl_->cancelPointerState();
                if (impl_->settings.absoluteMouseMode) {
                    impl_->releaseRemoteState();
                } else {
                    impl_->setMouseCaptured(false);
                }
            } else if (impl_->overlayResumePending) {
                impl_->resumeAfterOverlay();
            }
            break;
        case WM_CAPTURECHANGED:
            if (impl_->mouseCaptured && reinterpret_cast<HWND>(lparam) != impl_->window) {
                impl_->setMouseCaptured(false);
            }
            break;
        case WM_MOVE:
        case WM_SIZE:
            impl_->updateCursorClip();
            break;
        case WM_SETCURSOR:
            if (impl_->mouseCaptured ||
                (impl_->settings.absoluteMouseMode && !impl_->localCursorVisible)) {
                SetCursor(nullptr);
                result = TRUE;
                return true;
            }
            break;
        case WM_TIMER:
            if (wparam == kGamepadTimer) {
                impl_->pollGamepad();
                result = 0;
                return true;
            }
            break;
        default:
            break;
    }
    return false;
}

}  // namespace terra

#elif defined(__linux__) && defined(TERRA_HAS_LINUX_VIDEO)

#include <Limelight.h>

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace terra {
namespace {

struct KeyMapping {
    short key;
    char flags = 0;
};

std::optional<KeyMapping> windowsVirtualKey(SDL_Scancode scanCode) {
    if (scanCode >= SDL_SCANCODE_1 && scanCode <= SDL_SCANCODE_9) {
        return KeyMapping{static_cast<short>(0x31 + scanCode - SDL_SCANCODE_1)};
    }
    if (scanCode >= SDL_SCANCODE_A && scanCode <= SDL_SCANCODE_Z) {
        return KeyMapping{static_cast<short>(0x41 + scanCode - SDL_SCANCODE_A)};
    }
    if (scanCode >= SDL_SCANCODE_F1 && scanCode <= SDL_SCANCODE_F12) {
        return KeyMapping{static_cast<short>(0x70 + scanCode - SDL_SCANCODE_F1)};
    }
    if (scanCode >= SDL_SCANCODE_F13 && scanCode <= SDL_SCANCODE_F24) {
        return KeyMapping{static_cast<short>(0x7C + scanCode - SDL_SCANCODE_F13)};
    }
    if (scanCode >= SDL_SCANCODE_KP_1 && scanCode <= SDL_SCANCODE_KP_9) {
        return KeyMapping{static_cast<short>(0x61 + scanCode - SDL_SCANCODE_KP_1)};
    }

    switch (scanCode) {
        case SDL_SCANCODE_BACKSPACE: return KeyMapping{0x08};
        case SDL_SCANCODE_TAB: return KeyMapping{0x09};
        case SDL_SCANCODE_CLEAR: return KeyMapping{0x0C};
        case SDL_SCANCODE_KP_ENTER:
        case SDL_SCANCODE_RETURN: return KeyMapping{0x0D};
        case SDL_SCANCODE_PAUSE: return KeyMapping{0x13};
        case SDL_SCANCODE_CAPSLOCK: return KeyMapping{0x14};
        case SDL_SCANCODE_ESCAPE: return KeyMapping{0x1B};
        case SDL_SCANCODE_SPACE: return KeyMapping{0x20};
        case SDL_SCANCODE_PAGEUP: return KeyMapping{0x21};
        case SDL_SCANCODE_PAGEDOWN: return KeyMapping{0x22};
        case SDL_SCANCODE_END: return KeyMapping{0x23};
        case SDL_SCANCODE_HOME: return KeyMapping{0x24};
        case SDL_SCANCODE_LEFT: return KeyMapping{0x25};
        case SDL_SCANCODE_UP: return KeyMapping{0x26};
        case SDL_SCANCODE_RIGHT: return KeyMapping{0x27};
        case SDL_SCANCODE_DOWN: return KeyMapping{0x28};
        case SDL_SCANCODE_SELECT: return KeyMapping{0x29};
        case SDL_SCANCODE_EXECUTE: return KeyMapping{0x2B};
        case SDL_SCANCODE_PRINTSCREEN: return KeyMapping{0x2C};
        case SDL_SCANCODE_INSERT: return KeyMapping{0x2D};
        case SDL_SCANCODE_DELETE: return KeyMapping{0x2E};
        case SDL_SCANCODE_HELP: return KeyMapping{0x2F};
        case SDL_SCANCODE_0: return KeyMapping{0x30};
        case SDL_SCANCODE_LGUI: return KeyMapping{0x5B};
        case SDL_SCANCODE_RGUI: return KeyMapping{0x5C};
        case SDL_SCANCODE_APPLICATION: return KeyMapping{0x5D};
        case SDL_SCANCODE_KP_0: return KeyMapping{0x60};
        case SDL_SCANCODE_KP_MULTIPLY: return KeyMapping{0x6A};
        case SDL_SCANCODE_KP_PLUS: return KeyMapping{0x6B};
        case SDL_SCANCODE_KP_COMMA: return KeyMapping{0x6C};
        case SDL_SCANCODE_KP_MINUS: return KeyMapping{0x6D};
        case SDL_SCANCODE_KP_PERIOD: return KeyMapping{0x6E};
        case SDL_SCANCODE_KP_DIVIDE: return KeyMapping{0x6F};
        case SDL_SCANCODE_NUMLOCKCLEAR: return KeyMapping{static_cast<short>(0x90)};
        case SDL_SCANCODE_SCROLLLOCK: return KeyMapping{static_cast<short>(0x91)};
        case SDL_SCANCODE_LSHIFT: return KeyMapping{static_cast<short>(0xA0)};
        case SDL_SCANCODE_RSHIFT: return KeyMapping{static_cast<short>(0xA1)};
        case SDL_SCANCODE_LCTRL: return KeyMapping{static_cast<short>(0xA2)};
        case SDL_SCANCODE_RCTRL: return KeyMapping{static_cast<short>(0xA3)};
        case SDL_SCANCODE_LALT: return KeyMapping{static_cast<short>(0xA4)};
        case SDL_SCANCODE_RALT: return KeyMapping{static_cast<short>(0xA5)};
        case SDL_SCANCODE_AC_BACK: return KeyMapping{static_cast<short>(0xA6)};
        case SDL_SCANCODE_AC_FORWARD: return KeyMapping{static_cast<short>(0xA7)};
        case SDL_SCANCODE_AC_REFRESH: return KeyMapping{static_cast<short>(0xA8)};
        case SDL_SCANCODE_AC_STOP: return KeyMapping{static_cast<short>(0xA9)};
        case SDL_SCANCODE_AC_SEARCH: return KeyMapping{static_cast<short>(0xAA)};
        case SDL_SCANCODE_AC_BOOKMARKS: return KeyMapping{static_cast<short>(0xAB)};
        case SDL_SCANCODE_AC_HOME: return KeyMapping{static_cast<short>(0xAC)};
        case SDL_SCANCODE_SEMICOLON: return KeyMapping{static_cast<short>(0xBA)};
        case SDL_SCANCODE_EQUALS: return KeyMapping{static_cast<short>(0xBB)};
        case SDL_SCANCODE_COMMA: return KeyMapping{static_cast<short>(0xBC)};
        case SDL_SCANCODE_MINUS: return KeyMapping{static_cast<short>(0xBD)};
        case SDL_SCANCODE_PERIOD: return KeyMapping{static_cast<short>(0xBE)};
        case SDL_SCANCODE_SLASH: return KeyMapping{static_cast<short>(0xBF)};
        case SDL_SCANCODE_GRAVE: return KeyMapping{static_cast<short>(0xC0)};
        case SDL_SCANCODE_LEFTBRACKET: return KeyMapping{static_cast<short>(0xDB)};
        case SDL_SCANCODE_BACKSLASH: return KeyMapping{static_cast<short>(0xDC)};
        case SDL_SCANCODE_INTERNATIONAL3:
            return KeyMapping{static_cast<short>(0xDC), SS_KBE_FLAG_NON_NORMALIZED};
        case SDL_SCANCODE_RIGHTBRACKET: return KeyMapping{static_cast<short>(0xDD)};
        case SDL_SCANCODE_APOSTROPHE: return KeyMapping{static_cast<short>(0xDE)};
        case SDL_SCANCODE_NONUSBACKSLASH: return KeyMapping{static_cast<short>(0xE2)};
        case SDL_SCANCODE_INTERNATIONAL1:
            return KeyMapping{static_cast<short>(0xE2), SS_KBE_FLAG_NON_NORMALIZED};
        case SDL_SCANCODE_LANG1: return KeyMapping{0x1C};
        case SDL_SCANCODE_LANG2: return KeyMapping{0x1D};
        default: return std::nullopt;
    }
}

std::optional<int> terraMouseButton(Uint8 button) {
    switch (button) {
        case SDL_BUTTON_LEFT: return BUTTON_LEFT;
        case SDL_BUTTON_MIDDLE: return BUTTON_MIDDLE;
        case SDL_BUTTON_RIGHT: return BUTTON_RIGHT;
        case SDL_BUTTON_X1: return BUTTON_X1;
        case SDL_BUTTON_X2: return BUTTON_X2;
        default: return std::nullopt;
    }
}

short clampedShort(int value) {
    return static_cast<short>(std::clamp(value, -32768, 32767));
}

char terraModifiers(SDL_Keymod modifiers, bool includeMeta) {
    char result = 0;
    if ((modifiers & KMOD_SHIFT) != 0) result |= MODIFIER_SHIFT;
    if ((modifiers & KMOD_CTRL) != 0) result |= MODIFIER_CTRL;
    if ((modifiers & KMOD_ALT) != 0) result |= MODIFIER_ALT;
    if (includeMeta && (modifiers & KMOD_GUI) != 0) result |= MODIFIER_META;
    return result;
}

constexpr std::uint32_t kStandardGamepadButtons =
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
    RB_FLAG | PLAY_FLAG | BACK_FLAG | SPECIAL_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;

std::optional<int> terraControllerButton(Uint8 button, bool swapFaceButtons) {
    if (swapFaceButtons) {
        switch (button) {
            case SDL_CONTROLLER_BUTTON_A: button = SDL_CONTROLLER_BUTTON_B; break;
            case SDL_CONTROLLER_BUTTON_B: button = SDL_CONTROLLER_BUTTON_A; break;
            case SDL_CONTROLLER_BUTTON_X: button = SDL_CONTROLLER_BUTTON_Y; break;
            case SDL_CONTROLLER_BUTTON_Y: button = SDL_CONTROLLER_BUTTON_X; break;
            default: break;
        }
    }
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return A_FLAG;
        case SDL_CONTROLLER_BUTTON_B: return B_FLAG;
        case SDL_CONTROLLER_BUTTON_X: return X_FLAG;
        case SDL_CONTROLLER_BUTTON_Y: return Y_FLAG;
        case SDL_CONTROLLER_BUTTON_BACK: return BACK_FLAG;
        case SDL_CONTROLLER_BUTTON_GUIDE: return SPECIAL_FLAG;
        case SDL_CONTROLLER_BUTTON_START: return PLAY_FLAG;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return LS_CLK_FLAG;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return RS_CLK_FLAG;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return LB_FLAG;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return RB_FLAG;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return UP_FLAG;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return DOWN_FLAG;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return LEFT_FLAG;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RIGHT_FLAG;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        case SDL_CONTROLLER_BUTTON_MISC1: return MISC_FLAG;
        case SDL_CONTROLLER_BUTTON_PADDLE1: return PADDLE1_FLAG;
        case SDL_CONTROLLER_BUTTON_PADDLE2: return PADDLE2_FLAG;
        case SDL_CONTROLLER_BUTTON_PADDLE3: return PADDLE3_FLAG;
        case SDL_CONTROLLER_BUTTON_PADDLE4: return PADDLE4_FLAG;
        case SDL_CONTROLLER_BUTTON_TOUCHPAD: return TOUCHPAD_FLAG;
#endif
        default: return std::nullopt;
    }
}

short invertedControllerAxis(Sint16 value) {
    return static_cast<short>(-std::max<int>(value, -32767));
}

unsigned char controllerTrigger(Sint16 value) {
    return static_cast<unsigned char>(std::clamp<int>(value, 0, 32767) * 255 / 32767);
}

bool isSteamControllerProduct(std::uint16_t product) {
    switch (product) {
        case 0x1101:
        case 0x1102:
        case 0x1105:
        case 0x1106:
        case 0x1142:
        case 0x1201:
        case 0x1202:
        case 0x1205:
        case 0x1302:
        case 0x1303:
        case 0x1304:
        case 0x1305: return true;
        default: return false;
    }
}

void loadGamepadMappings() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return;
    const auto database = executable.parent_path() / "gamecontrollerdb.txt";
    if (SDL_GameControllerAddMappingsFromFile(database.string().c_str()) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Could not load %s: %s", database.c_str(),
                    SDL_GetError());
    }
}

bool initializeGamepadSubsystems(bool& ownsJoystick, bool& ownsGameController) {
    ownsJoystick = false;
    ownsGameController = false;
    if (SDL_InitSubSystem(SDL_INIT_JOYSTICK) < 0) return false;
    ownsJoystick = true;
    loadGamepadMappings();
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
        SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        ownsJoystick = false;
        return false;
    }
    ownsGameController = true;
    return true;
}

}  // namespace

std::uint16_t connectedGamepadMask() {
    if (!gamepadTransportAvailable()) return 0;
    bool ownsJoystick = false;
    bool ownsGameController = false;
    if (!initializeGamepadSubsystems(ownsJoystick, ownsGameController)) return 0;
    std::uint16_t mask = 0;
    int controllerCount = 0;
    const int joystickCount = SDL_NumJoysticks();
    for (int index = 0; index < joystickCount && controllerCount < 16; ++index) {
        if (!SDL_IsGameController(index)) continue;
        SDL_GameController* controller = SDL_GameControllerOpen(index);
        if (!controller) continue;
        mask |= static_cast<std::uint16_t>(1U << controllerCount++);
        SDL_GameControllerClose(controller);
    }
    if (ownsGameController) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    if (ownsJoystick) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
    return mask;
}

struct InputForwarder::Impl {
    static constexpr std::size_t kMaximumControllers = 16;

    struct ControllerSlot {
        SDL_GameController* controller = nullptr;
        SDL_JoystickID instanceId = -1;
        int buttons = 0;
        unsigned char leftTrigger = 0;
        unsigned char rightTrigger = 0;
        short leftStickX = 0;
        short leftStickY = 0;
        short rightStickX = 0;
        short rightStickY = 0;
        Uint32 accelerometerPeriodMs = 0;
        Uint32 gyroscopePeriodMs = 0;
        Uint32 lastAccelerometerAt = 0;
        Uint32 lastGyroscopeAt = 0;
        std::array<float, 3> lastAccelerometer{};
        std::array<float, 3> lastGyroscope{};
        std::unordered_set<std::uint64_t> activeTouches;
        SDL_JoystickPowerLevel powerLevel = SDL_JOYSTICK_POWER_UNKNOWN;
        Uint32 nextBatteryPollAt = 0;
        std::uint16_t lowFrequencyRumble = 0;
        std::uint16_t highFrequencyRumble = 0;
        std::uint16_t leftTriggerRumble = 0;
        std::uint16_t rightTriggerRumble = 0;
        Uint32 nextRumbleRefreshAt = 0;
        Uint32 nextTriggerRumbleRefreshAt = 0;
        bool stateDirty = false;
    };

    struct FingerState {
        std::uint32_t pointerId = 0;
        float lastX = 0;
        float lastY = 0;
    };

    SDL_Window* window = nullptr;
    InputSettings settings;
    std::string streamLabel;
    std::string inputError;
    int streamWidth = 1;
    int streamHeight = 1;
    bool enabled = false;
    bool mouseCaptured = false;
    std::unordered_map<SDL_FingerID, FingerState> fingers;
    std::optional<SDL_FingerID> primaryFinger;
    std::optional<SDL_FingerID> fallbackMouseFinger;
    std::uint32_t nextPointerId = 1;
    Uint32 gestureStartedAt = 0;
    std::size_t gestureMaximumFingers = 0;
    bool gestureMoved = false;
    float gestureDistance = 0;
    std::unordered_map<short, char> remoteKeysDown;
    std::unordered_set<SDL_Scancode> consumedShortcutKeys;
    std::unordered_set<SDL_Scancode> localKeysDown;
    std::unordered_set<int> remoteMouseButtonsDown;
    std::array<ControllerSlot, kMaximumControllers> controllers{};
    std::array<std::optional<std::uint16_t>, kMaximumControllers> pendingNeutralMasks{};
    bool controllerSuppressed = false;
    bool backgroundControllerEvents = false;
    bool controllerInputAvailable = true;
    bool ownsJoystickSubsystem = false;
    bool ownsGameControllerSubsystem = false;
    int previousJoystickEventState = SDL_IGNORE;
    int previousControllerEventState = SDL_IGNORE;
    std::optional<std::string> previousBackgroundEventsHint;
    std::optional<std::string> previousAltTabHint;
    std::function<void()> toggleStatistics;
    std::function<bool()> toggleFullscreen;
    OverlayListener overlayListener;
    OverlayCaptureListener overlayCaptureListener;
    bool localCursorVisible = true;
    bool pointerRegionLocked = false;
    bool absoluteInputCaptured = true;
    bool windowFocused = false;
    bool overlayCaptureSuspended = false;
    bool overlayDesiredVisible = false;
    bool overlayShortcutArmed = false;
    bool overlayResumePending = false;
    Uint32 overlayResumeRetryAt = 0;
    std::uint64_t overlayRevision = 0;
    bool restoreRelativeCapture = false;
    bool restoreAbsoluteCapture = false;

    void updateWindowTitle() const {
        if (!window) return;
        std::string title = "Terra Stream [" + streamLabel + "] - ";
        if (!inputError.empty()) {
            title += inputError;
        } else if (!enabled) {
            title += "Input waiting for connection";
        } else if (settings.absoluteMouseMode) {
            title += absoluteInputCaptured ? "Absolute pointer active" : "Click to capture input";
        } else {
            title += mouseCaptured ? "Input captured (Ctrl+Alt+Shift+Z to release)"
                                   : "Click to capture input";
        }
        SDL_SetWindowTitle(window, title.c_str());
    }

    void noteInputResult(int result) {
        if (result == 0 || !inputError.empty()) return;
        inputError = "Input queue failed (" + std::to_string(result) + ")";
        updateWindowTitle();
    }

    void noteControllerResult(int result) {
        noteInputResult(result);
    }

    void noteOptionalControllerResult(int result) {
        if (result != LI_ERR_UNSUPPORTED) noteControllerResult(result);
    }

    std::uint16_t gamepadMask() const {
        std::uint16_t mask = settings.forceGamepad ? 1 : 0;
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            if (controllers[index].controller) mask |= static_cast<std::uint16_t>(1U << index);
        }
        return mask;
    }

    std::uint8_t gamepadType(SDL_GameController* controller) const {
        if (!controller) return LI_CTYPE_XBOX;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        switch (SDL_GameControllerGetType(controller)) {
            case SDL_CONTROLLER_TYPE_XBOX360:
            case SDL_CONTROLLER_TYPE_XBOXONE: return LI_CTYPE_XBOX;
            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
#if SDL_VERSION_ATLEAST(2, 0, 14)
            case SDL_CONTROLLER_TYPE_PS5:
#endif
                return LI_CTYPE_PS;
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
#if SDL_VERSION_ATLEAST(2, 24, 0)
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
#endif
                return LI_CTYPE_NINTENDO;
            default: break;
        }
#endif
        return SDL_GameControllerGetVendor(controller) == 0x28de &&
                       isSteamControllerProduct(SDL_GameControllerGetProduct(controller))
                   ? LI_CTYPE_STEAM
                   : LI_CTYPE_UNKNOWN;
    }

    std::uint32_t supportedGamepadButtons(SDL_GameController* controller) const {
        if (!controller) return kStandardGamepadButtons;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        std::uint32_t supported = 0;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            const auto mapped = terraControllerButton(static_cast<Uint8>(button),
                                                          settings.swapFaceButtons);
            if (mapped && SDL_GameControllerHasButton(
                              controller, static_cast<SDL_GameControllerButton>(button))) {
                supported |= static_cast<std::uint32_t>(*mapped);
            }
        }
        return supported;
#else
        return kStandardGamepadButtons;
#endif
    }

    std::uint32_t gamepadCapabilities(SDL_GameController* controller) const {
        if (!controller) return LI_CCAP_ANALOG_TRIGGERS;
        const bool analogTriggers =
            SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT)
                    .bindType == SDL_CONTROLLER_BINDTYPE_AXIS ||
            SDL_GameControllerGetBindForAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
                    .bindType == SDL_CONTROLLER_BINDTYPE_AXIS;
        std::uint32_t capabilities = analogTriggers ? LI_CCAP_ANALOG_TRIGGERS : 0;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (SDL_GameControllerHasRumble(controller)) capabilities |= LI_CCAP_RUMBLE;
        if (SDL_GameControllerHasRumbleTriggers(controller)) {
            capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        }
#elif SDL_VERSION_ATLEAST(2, 0, 9)
        if (SDL_GameControllerRumble(controller, 1, 1, 1) == 0) capabilities |= LI_CCAP_RUMBLE;
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (SDL_GameControllerRumbleTriggers(controller, 1, 1, 1) == 0) {
            capabilities |= LI_CCAP_TRIGGER_RUMBLE;
        }
#endif
#endif
        const int touchpads = SDL_GameControllerGetNumTouchpads(controller);
        if (touchpads > 0) capabilities |= LI_CCAP_TOUCHPAD;
        if (touchpads > 1) capabilities |= LI_CCAP_DUAL_TOUCHPAD;
        if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_ACCEL)) {
            capabilities |= LI_CCAP_ACCEL;
        }
        if (SDL_GameControllerHasSensor(controller, SDL_SENSOR_GYRO)) {
            capabilities |= LI_CCAP_GYRO;
        }
        const auto power = SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(controller));
        if (power != SDL_JOYSTICK_POWER_UNKNOWN || SDL_VERSION_ATLEAST(2, 24, 0)) {
            capabilities |= LI_CCAP_BATTERY_STATE;
        }
        if (SDL_GameControllerHasLED(controller)) capabilities |= LI_CCAP_RGB_LED;
        return capabilities;
    }

    void stopGamepadRumble(ControllerSlot& slot) {
        if (!slot.controller) return;
#if SDL_VERSION_ATLEAST(2, 0, 9)
        SDL_GameControllerRumble(slot.controller, 0, 0, 0);
        slot.lowFrequencyRumble = 0;
        slot.highFrequencyRumble = 0;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
        SDL_GameControllerRumbleTriggers(slot.controller, 0, 0, 0);
        slot.leftTriggerRumble = 0;
        slot.rightTriggerRumble = 0;
#endif
    }

    void stopGamepadRumble() {
        for (auto& slot : controllers) stopGamepadRumble(slot);
    }

    void setGamepadRumble(std::uint16_t controllerNumber, std::uint16_t lowFrequency,
                           std::uint16_t highFrequency) {
#if SDL_VERSION_ATLEAST(2, 0, 9)
        if (!enabled || controllerNumber >= controllers.size() ||
            !controllers[controllerNumber].controller) {
            return;
        }
        auto& slot = controllers[controllerNumber];
        slot.lowFrequencyRumble = lowFrequency;
        slot.highFrequencyRumble = highFrequency;
        slot.nextRumbleRefreshAt = SDL_GetTicks() + 30000;
        SDL_GameControllerRumble(slot.controller, lowFrequency, highFrequency,
                                 lowFrequency || highFrequency ? 60000 : 0);
#else
        static_cast<void>(controllerNumber);
        static_cast<void>(lowFrequency);
        static_cast<void>(highFrequency);
#endif
    }

    void setGamepadTriggerRumble(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                  std::uint16_t rightTrigger) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
        if (!enabled || controllerNumber >= controllers.size() ||
            !controllers[controllerNumber].controller) {
            return;
        }
        auto& slot = controllers[controllerNumber];
        slot.leftTriggerRumble = leftTrigger;
        slot.rightTriggerRumble = rightTrigger;
        slot.nextTriggerRumbleRefreshAt = SDL_GetTicks() + 30000;
        SDL_GameControllerRumbleTriggers(slot.controller, leftTrigger, rightTrigger,
                                         leftTrigger || rightTrigger ? 60000 : 0);
#else
        static_cast<void>(controllerNumber);
        static_cast<void>(leftTrigger);
        static_cast<void>(rightTrigger);
#endif
    }

    void setGamepadMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz) {
        if (controllerNumber >= controllers.size() || !controllers[controllerNumber].controller) {
            return;
        }
        auto& slot = controllers[controllerNumber];
        const auto sensor = motionType == LI_MOTION_TYPE_ACCEL ? SDL_SENSOR_ACCEL
                            : motionType == LI_MOTION_TYPE_GYRO ? SDL_SENSOR_GYRO
                                                               : SDL_SENSOR_INVALID;
        if (sensor == SDL_SENSOR_INVALID ||
            !SDL_GameControllerHasSensor(slot.controller, sensor)) {
            return;
        }
        auto& period = motionType == LI_MOTION_TYPE_ACCEL ? slot.accelerometerPeriodMs
                                                          : slot.gyroscopePeriodMs;
        if (SDL_GameControllerSetSensorEnabled(slot.controller, sensor,
                                               reportRateHz ? SDL_TRUE : SDL_FALSE) < 0) {
            period = 0;
            return;
        }
        period = reportRateHz ? std::max<Uint32>(1000U / reportRateHz, 1U) : 0;
    }

    void setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red, std::uint8_t green,
                       std::uint8_t blue) const {
        if (!enabled || controllerNumber >= controllers.size() ||
            !controllers[controllerNumber].controller ||
            !SDL_GameControllerHasLED(controllers[controllerNumber].controller)) {
            return;
        }
        SDL_GameControllerSetLED(controllers[controllerNumber].controller, red, green, blue);
    }

    void updateGamepads() {
        if (!enabled) return;
        const auto now = SDL_GetTicks();
        if (overlayResumePending && !overlayDesiredVisible && overlayToggleKeyReleased() &&
            SDL_TICKS_PASSED(now, overlayResumeRetryAt)) {
            resumeAfterOverlay();
        }
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            if (pendingNeutralMasks[index]) {
                sendNeutralControllerState(index, *pendingNeutralMasks[index]);
            }
            auto& slot = controllers[index];
            if (!slot.controller) continue;
            if (slot.stateDirty) sendControllerState(index);
            if ((slot.lowFrequencyRumble || slot.highFrequencyRumble) &&
                SDL_TICKS_PASSED(now, slot.nextRumbleRefreshAt)) {
                SDL_GameControllerRumble(slot.controller, slot.lowFrequencyRumble,
                                         slot.highFrequencyRumble, 60000);
                slot.nextRumbleRefreshAt = now + 30000;
            }
            if ((slot.leftTriggerRumble || slot.rightTriggerRumble) &&
                SDL_TICKS_PASSED(now, slot.nextTriggerRumbleRefreshAt)) {
                SDL_GameControllerRumbleTriggers(slot.controller, slot.leftTriggerRumble,
                                                 slot.rightTriggerRumble, 60000);
                slot.nextTriggerRumbleRefreshAt = now + 30000;
            }
            if (!SDL_TICKS_PASSED(now, slot.nextBatteryPollAt)) continue;
            slot.nextBatteryPollAt = now + 30000;
            const auto power =
                SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(slot.controller));
            if (power == slot.powerLevel) continue;
#if SDL_VERSION_ATLEAST(2, 24, 0)
            sendControllerBattery(index, power);
#else
            if (slot.powerLevel == SDL_JOYSTICK_POWER_UNKNOWN &&
                power != SDL_JOYSTICK_POWER_UNKNOWN) {
                announceController(index);
            } else {
                sendControllerBattery(index, power);
            }
#endif
        }
    }

    static void resetControllerState(ControllerSlot& slot) {
        slot.buttons = 0;
        slot.leftTrigger = 0;
        slot.rightTrigger = 0;
        slot.leftStickX = 0;
        slot.leftStickY = 0;
        slot.rightStickX = 0;
        slot.rightStickY = 0;
    }

    void sendControllerBattery(std::size_t index, SDL_JoystickPowerLevel level) {
        if (!enabled || !controllerInputAvailable || !gamepadTransportAvailable() ||
            index >= controllers.size() || !controllers[index].controller) {
            return;
        }
        std::uint8_t state = LI_BATTERY_STATE_UNKNOWN;
        std::uint8_t percentage = LI_BATTERY_PERCENTAGE_UNKNOWN;
        controllers[index].powerLevel = level;
        controllers[index].nextBatteryPollAt = SDL_GetTicks() + 30000;
        switch (level) {
            case SDL_JOYSTICK_POWER_WIRED: state = LI_BATTERY_STATE_CHARGING; break;
            case SDL_JOYSTICK_POWER_EMPTY:
                state = LI_BATTERY_STATE_DISCHARGING;
                percentage = 5;
                break;
            case SDL_JOYSTICK_POWER_LOW:
                state = LI_BATTERY_STATE_DISCHARGING;
                percentage = 20;
                break;
            case SDL_JOYSTICK_POWER_MEDIUM:
                state = LI_BATTERY_STATE_DISCHARGING;
                percentage = 50;
                break;
            case SDL_JOYSTICK_POWER_FULL:
                state = LI_BATTERY_STATE_DISCHARGING;
                percentage = 90;
                break;
            case SDL_JOYSTICK_POWER_UNKNOWN: break;
            case SDL_JOYSTICK_POWER_MAX: return;
        }
        noteOptionalControllerResult(LiSendControllerBatteryEvent(
            static_cast<std::uint8_t>(index), state, percentage));
    }

    void cancelControllerTouches(std::size_t index) {
        if (index >= controllers.size()) return;
        auto& slot = controllers[index];
        for (const auto touch : slot.activeTouches) {
            const auto touchpad = static_cast<std::uint8_t>(touch >> 32U);
            const auto finger = static_cast<std::uint32_t>(touch);
            noteOptionalControllerResult(LiSendControllerTouchEvent2(
                static_cast<std::uint8_t>(index), LI_TOUCH_EVENT_CANCEL, touchpad, finger, 0, 0,
                0));
        }
        slot.activeTouches.clear();
    }

    void refreshControllerState(std::size_t index) {
        auto& slot = controllers[index];
        resetControllerState(slot);
        if (!slot.controller) return;
        for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button) {
            if (SDL_GameControllerGetButton(slot.controller,
                                            static_cast<SDL_GameControllerButton>(button)) == 0) {
                continue;
            }
            if (const auto mapped = terraControllerButton(
                    static_cast<Uint8>(button), settings.swapFaceButtons)) {
                slot.buttons |= *mapped;
            }
        }
        slot.leftStickX = SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_LEFTX);
        slot.leftStickY = invertedControllerAxis(
            SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_LEFTY));
        slot.rightStickX = SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_RIGHTX);
        slot.rightStickY = invertedControllerAxis(
            SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_RIGHTY));
        slot.leftTrigger = controllerTrigger(
            SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        slot.rightTrigger = controllerTrigger(
            SDL_GameControllerGetAxis(slot.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
    }

    void refreshControllerStates() {
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            refreshControllerState(index);
        }
    }

    void announceController(std::size_t index) {
        if (!enabled || !controllerInputAvailable || !gamepadTransportAvailable() ||
            gamepadMask() == 0 ||
            (!controllers[index].controller && !(index == 0 && settings.forceGamepad))) {
            return;
        }
        auto* controller = controllers[index].controller;
        const int result = LiSendControllerArrivalEvent(
            static_cast<std::uint8_t>(index), gamepadMask(), gamepadType(controller),
            supportedGamepadButtons(controller), gamepadCapabilities(controller));
        noteControllerResult(result);
        if (result == 0 && controller) {
            const auto power =
                SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(controller));
            if (power != SDL_JOYSTICK_POWER_UNKNOWN) sendControllerBattery(index, power);
        }
    }

    void announceControllers() {
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            announceController(index);
        }
    }

    void sendControllerState(std::size_t index) {
        if (!enabled || !controllerInputAvailable || !gamepadTransportAvailable() ||
            controllerSuppressed || overlayCaptureSuspended || gamepadMask() == 0 ||
            (!controllers[index].controller && !(index == 0 && settings.forceGamepad))) {
            return;
        }
        auto& slot = controllers[index];
        slot.stateDirty = true;
        const int result = LiSendMultiControllerEvent(
            static_cast<short>(index), static_cast<short>(gamepadMask()), slot.buttons,
            slot.leftTrigger, slot.rightTrigger, slot.leftStickX, slot.leftStickY,
            slot.rightStickX, slot.rightStickY);
        noteControllerResult(result);
        if (result == 0) slot.stateDirty = false;
    }

    void sendControllerStates() {
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            sendControllerState(index);
        }
    }

    void sendNeutralControllerState(std::size_t index, std::uint16_t mask) {
        if (!controllerInputAvailable || !gamepadTransportAvailable()) return;
        const int result = LiSendMultiControllerEvent(static_cast<short>(index),
                                                       static_cast<short>(mask), 0, 0, 0, 0, 0, 0,
                                                       0);
        noteControllerResult(result);
        if (result == 0) {
            pendingNeutralMasks[index].reset();
        } else {
            pendingNeutralMasks[index] = mask;
        }
    }

    void sendNeutralControllerStates(std::uint16_t mask) {
        bool sent = false;
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            if (!controllers[index].controller && !(index == 0 && settings.forceGamepad)) continue;
            sendNeutralControllerState(index, mask);
            sent = true;
        }
        if (!sent && mask == 0) sendNeutralControllerState(0, 0);
    }

    std::optional<std::size_t> controllerIndex(SDL_JoystickID instanceId) const {
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            if (controllers[index].instanceId == instanceId) return index;
        }
        return std::nullopt;
    }

    bool openController(int deviceIndex) {
        if (!SDL_IsGameController(deviceIndex)) return false;
        SDL_GameController* candidate = SDL_GameControllerOpen(deviceIndex);
        if (!candidate) return false;
        SDL_Joystick* joystick = SDL_GameControllerGetJoystick(candidate);
        const SDL_JoystickID instanceId = joystick ? SDL_JoystickInstanceID(joystick) : -1;
        if (instanceId < 0) {
            SDL_GameControllerClose(candidate);
            return false;
        }
        if (controllerIndex(instanceId)) {
            SDL_GameControllerClose(candidate);
            return false;
        }
        const auto freeSlot = std::find_if(controllers.begin(), controllers.end(),
                                           [](const auto& slot) { return !slot.controller; });
        if (freeSlot == controllers.end()) {
            SDL_GameControllerClose(candidate);
            return false;
        }
        const auto index = static_cast<std::size_t>(freeSlot - controllers.begin());
        pendingNeutralMasks[index].reset();
        freeSlot->controller = candidate;
        freeSlot->instanceId = instanceId;
        refreshControllerState(index);
        if (enabled) {
            announceController(index);
            if (controllerSuppressed) {
                sendNeutralControllerState(index, gamepadMask());
            } else {
                sendControllerState(index);
            }
        }
        return true;
    }

    void openControllers() {
        const int joystickCount = SDL_NumJoysticks();
        for (int index = 0; index < joystickCount; ++index) {
            static_cast<void>(openController(index));
        }
    }

    void closeController(std::size_t index) {
        auto& slot = controllers[index];
        if (enabled) cancelControllerTouches(index);
        stopGamepadRumble(slot);
        if (slot.controller) SDL_GameControllerClose(slot.controller);
        slot = {};
    }

    void handleControllerDevice(const SDL_ControllerDeviceEvent& event) {
        if (event.type == SDL_CONTROLLERDEVICEADDED) {
            static_cast<void>(openController(event.which));
            return;
        }
        if (event.type == SDL_CONTROLLERDEVICEREMAPPED) {
            const auto index = controllerIndex(event.which);
            if (!index) return;
            refreshControllerState(*index);
            announceController(*index);
            sendControllerState(*index);
            return;
        }
        if (event.type != SDL_CONTROLLERDEVICEREMOVED) return;
        const auto index = controllerIndex(event.which);
        if (!index) return;
        closeController(*index);
        if (!enabled) return;
        sendNeutralControllerState(*index, gamepadMask());
        if (*index == 0 && settings.forceGamepad) announceController(0);
    }

    void handleControllerAxis(const SDL_ControllerAxisEvent& event) {
        const auto index = controllerIndex(event.which);
        if (!index) return;
        auto& slot = controllers[*index];
        switch (event.axis) {
            case SDL_CONTROLLER_AXIS_LEFTX: slot.leftStickX = event.value; break;
            case SDL_CONTROLLER_AXIS_LEFTY:
                slot.leftStickY = invertedControllerAxis(event.value);
                break;
            case SDL_CONTROLLER_AXIS_RIGHTX: slot.rightStickX = event.value; break;
            case SDL_CONTROLLER_AXIS_RIGHTY:
                slot.rightStickY = invertedControllerAxis(event.value);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                slot.leftTrigger = controllerTrigger(event.value);
                break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
                slot.rightTrigger = controllerTrigger(event.value);
                break;
            default: return;
        }
        sendControllerState(*index);
    }

    void handleControllerButton(const SDL_ControllerButtonEvent& event) {
        const auto index = controllerIndex(event.which);
        if (!index) return;
        auto& slot = controllers[*index];
        const auto button = terraControllerButton(event.button, settings.swapFaceButtons);
        if (!button) return;
        if (event.state == SDL_PRESSED) {
            slot.buttons |= *button;
        } else {
            slot.buttons &= ~*button;
        }
        sendControllerState(*index);
    }

    void handleControllerSensor(const SDL_ControllerSensorEvent& event) {
        const auto index = controllerIndex(event.which);
        if (!index || !enabled || controllerSuppressed || overlayCaptureSuspended ||
            !controllerInputAvailable ||
            !gamepadTransportAvailable() ||
            (LiGetHostFeatureFlags() & LI_FF_CONTROLLER_TOUCH_EVENTS) == 0) {
            return;
        }
        auto& slot = controllers[*index];
        auto motionType = std::uint8_t{0};
        Uint32* period = nullptr;
        Uint32* lastAt = nullptr;
        std::array<float, 3>* lastData = nullptr;
        if (event.sensor == SDL_SENSOR_ACCEL) {
            motionType = LI_MOTION_TYPE_ACCEL;
            period = &slot.accelerometerPeriodMs;
            lastAt = &slot.lastAccelerometerAt;
            lastData = &slot.lastAccelerometer;
        } else if (event.sensor == SDL_SENSOR_GYRO) {
            motionType = LI_MOTION_TYPE_GYRO;
            period = &slot.gyroscopePeriodMs;
            lastAt = &slot.lastGyroscopeAt;
            lastData = &slot.lastGyroscope;
        } else {
            return;
        }
        if (*period == 0 || !SDL_TICKS_PASSED(event.timestamp, *lastAt + *period) ||
            std::equal(lastData->begin(), lastData->end(), event.data)) {
            return;
        }
        std::copy(std::begin(event.data), std::end(event.data), lastData->begin());
        *lastAt = event.timestamp;
        constexpr float radiansToDegrees = 57.2957795F;
        const float scale = motionType == LI_MOTION_TYPE_GYRO ? radiansToDegrees : 1.0F;
        noteOptionalControllerResult(LiSendControllerMotionEvent(
            static_cast<std::uint8_t>(*index), motionType, event.data[0] * scale,
            event.data[1] * scale, event.data[2] * scale));
    }

    void handleControllerTouchpad(const SDL_ControllerTouchpadEvent& event) {
        const auto index = controllerIndex(event.which);
        if (!index || !enabled || controllerSuppressed || overlayCaptureSuspended ||
            !controllerInputAvailable ||
            !gamepadTransportAvailable() || event.touchpad < 0 || event.touchpad > 1 ||
            event.finger < 0 ||
            (LiGetHostFeatureFlags() & LI_FF_CONTROLLER_TOUCH_EVENTS) == 0) {
            return;
        }
        auto& slot = controllers[*index];
        const auto type = event.type == SDL_CONTROLLERTOUCHPADDOWN ? LI_TOUCH_EVENT_DOWN
                          : event.type == SDL_CONTROLLERTOUCHPADUP ? LI_TOUCH_EVENT_UP
                                                                   : LI_TOUCH_EVENT_MOVE;
        const auto touchpad = static_cast<std::uint8_t>(event.touchpad);
        const auto finger = static_cast<std::uint32_t>(event.finger);
        const int result = LiSendControllerTouchEvent2(
            static_cast<std::uint8_t>(*index), type, touchpad, finger,
            std::clamp(event.x, 0.0F, 1.0F), std::clamp(event.y, 0.0F, 1.0F),
            std::clamp(event.pressure, 0.0F, 1.0F));
        noteOptionalControllerResult(result);
        if (result != 0) return;
        const auto touch = (static_cast<std::uint64_t>(touchpad) << 32U) | finger;
        if (type == LI_TOUCH_EVENT_DOWN) slot.activeTouches.insert(touch);
        else if (type == LI_TOUCH_EVENT_UP) slot.activeTouches.erase(touch);
    }

#if SDL_VERSION_ATLEAST(2, 24, 0)
    void handleControllerBattery(const SDL_JoyBatteryEvent& event) {
        const auto index = controllerIndex(event.which);
        if (!index || controllers[*index].powerLevel == event.level) return;
        sendControllerBattery(*index, event.level);
    }
#endif

    void setControllerFocus(bool focused) {
        if (backgroundControllerEvents || controllerSuppressed == !focused) return;
        controllerSuppressed = !focused;
        if (!enabled || gamepadMask() == 0) return;
        if (controllerSuppressed) {
            for (std::size_t index = 0; index < controllers.size(); ++index) {
                cancelControllerTouches(index);
            }
            sendNeutralControllerStates(gamepadMask());
        } else {
            refreshControllerStates();
            sendControllerStates();
        }
    }

    void startControllers() {
        if (const char* previous = SDL_GetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS)) {
            previousBackgroundEventsHint = previous;
        }
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                                settings.backgroundGamepad ? "1" : "0", SDL_HINT_OVERRIDE);
        backgroundControllerEvents =
            settings.backgroundGamepad &&
            SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_FALSE);
        if (!initializeGamepadSubsystems(ownsJoystickSubsystem,
                                         ownsGameControllerSubsystem)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_INPUT, "Could not initialize SDL gamepad input: %s",
                        SDL_GetError());
            return;
        }
        previousJoystickEventState = SDL_JoystickEventState(SDL_QUERY);
        previousControllerEventState = SDL_GameControllerEventState(SDL_QUERY);
        SDL_JoystickEventState(SDL_ENABLE);
        SDL_GameControllerEventState(SDL_ENABLE);
        controllerSuppressed = !backgroundControllerEvents &&
                               (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0;
        openControllers();
    }

    void stopControllers() {
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            closeController(index);
        }
        if (ownsGameControllerSubsystem) {
            SDL_GameControllerEventState(previousControllerEventState);
            SDL_JoystickEventState(previousJoystickEventState);
        }
        if (ownsGameControllerSubsystem) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        if (ownsJoystickSubsystem) SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
        ownsGameControllerSubsystem = false;
        ownsJoystickSubsystem = false;
        backgroundControllerEvents = false;
#if SDL_VERSION_ATLEAST(2, 24, 0)
        SDL_ResetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS);
        if (previousBackgroundEventsHint) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                        previousBackgroundEventsHint->c_str());
        }
#else
        SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                                previousBackgroundEventsHint
                                    ? previousBackgroundEventsHint->c_str()
                                    : "0",
                                SDL_HINT_OVERRIDE);
#endif
        previousBackgroundEventsHint.reset();
    }

    SDL_Rect videoBounds() const {
        int width = 1;
        int height = 1;
        SDL_GetWindowSize(window, &width, &height);
        SDL_Rect bounds{0, 0, std::max(width, 1), std::max(height, 1)};
        const double inputAspect = static_cast<double>(streamWidth) / streamHeight;
        const double outputAspect = static_cast<double>(bounds.w) / bounds.h;
        if (inputAspect > outputAspect) {
            bounds.h = std::max(static_cast<int>(bounds.w / inputAspect), 1);
            bounds.y = (height - bounds.h) / 2;
        } else {
            bounds.w = std::max(static_cast<int>(bounds.h * inputAspect), 1);
            bounds.x = (width - bounds.w) / 2;
        }
        return bounds;
    }

    bool systemKeyCaptureActive() const {
        if (!enabled || !window ||
            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) == 0 ||
            (settings.absoluteMouseMode ? !absoluteInputCaptured : !mouseCaptured) ||
            settings.captureSystemKeys == SystemKeyCapture::off) {
            return false;
        }
        return settings.captureSystemKeys == SystemKeyCapture::always || settings.fullscreen;
    }

    void updateKeyboardGrab() const {
        if (!window) return;
        const bool capture = systemKeyCaptureActive();
        SDL_SetWindowKeyboardGrab(window, capture ? SDL_TRUE : SDL_FALSE);
        SDL_SetHintWithPriority("SDL_ALLOW_ALT_TAB_WHILE_GRABBED",
                                capture ? "0"
                                        : previousAltTabHint ? previousAltTabHint->c_str() : "1",
                                SDL_HINT_OVERRIDE);
    }

    bool releaseRemoteState() {
        for (auto key = remoteKeysDown.begin(); key != remoteKeysDown.end();) {
            const int result = LiSendKeyboardEvent2(
                static_cast<short>(0x8000U | static_cast<unsigned short>(key->first)),
                KEY_ACTION_UP, 0, key->second);
            if (result == 0) {
                key = remoteKeysDown.erase(key);
            } else {
                noteInputResult(result);
                ++key;
            }
        }
        for (auto button = remoteMouseButtonsDown.begin();
             button != remoteMouseButtonsDown.end();) {
            const int result = LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, *button);
            if (result == 0) {
                button = remoteMouseButtonsDown.erase(button);
            } else {
                noteInputResult(result);
                ++button;
            }
        }
        return remoteKeysDown.empty() && remoteMouseButtonsDown.empty();
    }

    void releaseRemoteStateWithRetry() {
        for (int attempt = 0; attempt < 20 && !releaseRemoteState(); ++attempt) {
            SDL_Delay(1);
        }
    }

    void cancelTouchState() {
        if (!fingers.empty() && !settings.touchscreenTrackpad &&
            (LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            const int result = LiSendTouchEvent(LI_TOUCH_EVENT_CANCEL_ALL, 0, 0, 0, 0, 0, 0,
                                                LI_ROT_UNKNOWN);
            if (result != LI_ERR_UNSUPPORTED) noteInputResult(result);
        }
        if (fallbackMouseFinger) {
            sendMouseButton(SDL_BUTTON_LEFT, false);
        }
        fingers.clear();
        primaryFinger.reset();
        fallbackMouseFinger.reset();
        gestureMaximumFingers = 0;
        gestureMoved = false;
        gestureDistance = 0;
    }

    bool setMouseCaptured(bool capture) {
        if (!window || !enabled || settings.absoluteMouseMode) return false;
        if (mouseCaptured == capture) return true;
        if (capture) {
            SDL_SetWindowGrab(window, SDL_TRUE);
            if (SDL_SetRelativeMouseMode(SDL_TRUE) < 0) {
                SDL_SetWindowGrab(window, SDL_FALSE);
                inputError = "Mouse capture unavailable: " + std::string{SDL_GetError()};
                updateWindowTitle();
                return false;
            }
            mouseCaptured = true;
        } else {
            releaseRemoteStateWithRetry();
            SDL_SetRelativeMouseMode(SDL_FALSE);
            SDL_SetWindowGrab(window, SDL_FALSE);
            mouseCaptured = false;
        }
        updateKeyboardGrab();
        updateWindowTitle();
        return true;
    }

    void applyPointerRegionLock() {
        if (!window) return;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        if (settings.absoluteMouseMode && pointerRegionLocked) {
            const auto bounds = videoBounds();
            SDL_SetWindowMouseRect(window, &bounds);
        } else {
            SDL_SetWindowMouseRect(window, nullptr);
        }
#else
        SDL_SetWindowGrab(window, settings.absoluteMouseMode && pointerRegionLocked ? SDL_TRUE
                                                                                    : SDL_FALSE);
#endif
    }

    void toggleMouseMode() {
        if (!settings.absoluteMouseMode) setMouseCaptured(false);
        settings.absoluteMouseMode = !settings.absoluteMouseMode;
        if (settings.absoluteMouseMode) absoluteInputCaptured = true;
        if (!settings.absoluteMouseMode) setMouseCaptured(true);
        applyPointerRegionLock();
        updateKeyboardGrab();
        updateWindowTitle();
    }

    void toggleInputCapture() {
        if (!settings.absoluteMouseMode) {
            setMouseCaptured(!mouseCaptured);
            return;
        }
        absoluteInputCaptured = !absoluteInputCaptured;
        if (absoluteInputCaptured) {
            applyPointerRegionLock();
        } else {
            releaseRemoteStateWithRetry();
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_SetWindowMouseRect(window, nullptr);
#else
            SDL_SetWindowGrab(window, SDL_FALSE);
#endif
        }
        updateKeyboardGrab();
        updateWindowTitle();
    }

    bool overlayShortcutActive() const {
        return enabled && window && windowFocused && overlayListener &&
               (overlayShortcutArmed || overlayCaptureSuspended ||
                 (settings.absoluteMouseMode ? absoluteInputCaptured : mouseCaptured));
    }

    bool overlayToggleKeyReleased() const {
        return !localKeysDown.contains(SDL_SCANCODE_O);
    }

    void suspendForOverlay() {
        if (!overlayCaptureSuspended) {
            restoreRelativeCapture = !settings.absoluteMouseMode && mouseCaptured;
            restoreAbsoluteCapture = settings.absoluteMouseMode && absoluteInputCaptured;
        }
        overlayCaptureSuspended = true;
        overlayResumePending = false;
        overlayResumeRetryAt = 0;
        overlayShortcutArmed = true;
        cancelTouchState();
        for (std::size_t index = 0; index < controllers.size(); ++index) {
            cancelControllerTouches(index);
        }
        if (gamepadMask() != 0) sendNeutralControllerStates(gamepadMask());
        if (restoreRelativeCapture) {
            setMouseCaptured(false);
        } else if (restoreAbsoluteCapture) {
            releaseRemoteStateWithRetry();
            absoluteInputCaptured = false;
#if SDL_VERSION_ATLEAST(2, 0, 18)
            SDL_SetWindowMouseRect(window, nullptr);
#else
            SDL_SetWindowGrab(window, SDL_FALSE);
#endif
            updateKeyboardGrab();
            updateWindowTitle();
        }
    }

    void resumeAfterOverlay() {
        if (!overlayCaptureSuspended) return;
        if (!overlayToggleKeyReleased()) {
            overlayResumePending = true;
            return;
        }
        if (!enabled || !window) {
            overlayResumePending = false;
            overlayCaptureSuspended = false;
            restoreRelativeCapture = false;
            restoreAbsoluteCapture = false;
            if (overlayCaptureListener) overlayCaptureListener(overlayRevision, false);
            return;
        }
        if (!windowFocused) {
            overlayResumePending = true;
            return;
        }
        if (restoreRelativeCapture && !settings.absoluteMouseMode) {
            if (!setMouseCaptured(true)) {
                overlayResumePending = true;
                overlayResumeRetryAt = SDL_GetTicks() + 50;
                return;
            }
        } else if (restoreAbsoluteCapture && settings.absoluteMouseMode) {
            absoluteInputCaptured = true;
            applyPointerRegionLock();
            updateKeyboardGrab();
            updateWindowTitle();
        }
        overlayResumePending = false;
        overlayResumeRetryAt = 0;
        consumedShortcutKeys.erase(SDL_SCANCODE_O);
        restoreRelativeCapture = false;
        restoreAbsoluteCapture = false;
        overlayCaptureSuspended = false;
        if (overlayCaptureListener) overlayCaptureListener(overlayRevision, false);
    }

    void publishOverlayState() {
        if (overlayListener) overlayListener(overlayRevision, overlayDesiredVisible);
    }

    void toggleOverlay() {
        const bool previous = overlayDesiredVisible;
        if (!previous) suspendForOverlay();
        overlayDesiredVisible = !previous;
        ++overlayRevision;
        try {
            publishOverlayState();
        } catch (...) {
            overlayDesiredVisible = previous;
            if (!previous) resumeAfterOverlay();
            throw;
        }
    }

    void closeOverlay(std::uint64_t revision) {
        if (!overlayDesiredVisible || revision != overlayRevision) return;
        overlayDesiredVisible = false;
        ++overlayRevision;
        publishOverlayState();
    }

    bool acknowledgeOverlayHidden(std::uint64_t revision) {
        if (overlayDesiredVisible || revision != overlayRevision) return false;
        resumeAfterOverlay();
        return true;
    }

    void updateOverlay() {
        if (!overlayDesiredVisible) return;
        ++overlayRevision;
        publishOverlayState();
    }

    void toggleCursorDisplay() {
        if (!settings.absoluteMouseMode) return;
        localCursorVisible = !localCursorVisible;
        SDL_ShowCursor(localCursorVisible ? SDL_ENABLE : SDL_DISABLE);
    }

    void togglePointerRegionLock() {
        if (!settings.absoluteMouseMode) return;
        pointerRegionLocked = !pointerRegionLocked;
        applyPointerRegionLock();
    }

    void sendClipboardText() {
        char* clipboard = SDL_GetClipboardText();
        if (!clipboard) return;
        const auto text = prepareClipboardText(clipboard);
        SDL_free(clipboard);
        if (!text) return;
        noteInputResult(LiSendUtf8TextEvent(text->data(), static_cast<unsigned int>(text->size())));
    }

    void handleKeyboard(const SDL_KeyboardEvent& event) {
        const bool pressed = event.state == SDL_PRESSED;
        if (pressed) {
            localKeysDown.insert(event.keysym.scancode);
        } else {
            localKeysDown.erase(event.keysym.scancode);
        }
        if (!pressed) {
            const bool consumed = consumedShortcutKeys.erase(event.keysym.scancode) != 0;
            if (overlayResumePending && overlayToggleKeyReleased()) resumeAfterOverlay();
            if (consumed) return;
        }
        if (pressed && consumedShortcutKeys.contains(event.keysym.scancode)) return;
        const auto modifiers = static_cast<SDL_Keymod>(event.keysym.mod);
        const bool overlayShortcut = event.keysym.scancode == SDL_SCANCODE_O &&
                                     overlayShortcutActive();
        if (pressed && event.repeat == 0 && (modifiers & KMOD_CTRL) != 0 &&
            (modifiers & KMOD_ALT) != 0 && (modifiers & KMOD_SHIFT) != 0 &&
             (event.keysym.scancode == SDL_SCANCODE_Q ||
               event.keysym.scancode == SDL_SCANCODE_Z ||
               event.keysym.scancode == SDL_SCANCODE_X ||
               event.keysym.scancode == SDL_SCANCODE_S ||
               event.keysym.scancode == SDL_SCANCODE_M ||
               event.keysym.scancode == SDL_SCANCODE_V ||
               event.keysym.scancode == SDL_SCANCODE_D ||
               event.keysym.scancode == SDL_SCANCODE_C ||
                event.keysym.scancode == SDL_SCANCODE_L || overlayShortcut)) {
            consumedShortcutKeys.insert(event.keysym.scancode);
            releaseRemoteStateWithRetry();
            if (event.keysym.scancode == SDL_SCANCODE_Q) {
                SDL_Event quit{};
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
            } else if (event.keysym.scancode == SDL_SCANCODE_X) {
                if (toggleFullscreen) settings.fullscreen = toggleFullscreen();
            } else if (event.keysym.scancode == SDL_SCANCODE_S) {
                if (toggleStatistics) toggleStatistics();
            } else if (event.keysym.scancode == SDL_SCANCODE_Z) {
                toggleInputCapture();
            } else if (event.keysym.scancode == SDL_SCANCODE_M) {
                toggleMouseMode();
            } else if (event.keysym.scancode == SDL_SCANCODE_V) {
                sendClipboardText();
            } else if (event.keysym.scancode == SDL_SCANCODE_D) {
                SDL_MinimizeWindow(window);
            } else if (event.keysym.scancode == SDL_SCANCODE_C) {
                toggleCursorDisplay();
            } else if (event.keysym.scancode == SDL_SCANCODE_L) {
                togglePointerRegionLock();
            } else if (overlayShortcut) {
                try {
                    toggleOverlay();
                } catch (...) {
                }
            }
            return;
        }
        if (event.repeat != 0) return;
        if (overlayCaptureSuspended) return;
        if (settings.absoluteMouseMode && !absoluteInputCaptured) return;
        const auto mapping = windowsVirtualKey(event.keysym.scancode);
        if (!mapping) return;
        const bool captureSystemKeys = systemKeyCaptureActive();
        if ((event.keysym.scancode == SDL_SCANCODE_LGUI ||
             event.keysym.scancode == SDL_SCANCODE_RGUI) &&
            !captureSystemKeys) {
            return;
        }
        if (!pressed && !remoteKeysDown.contains(mapping->key)) return;
        const int result = LiSendKeyboardEvent2(
            static_cast<short>(0x8000U | static_cast<unsigned short>(mapping->key)),
            pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP,
            terraModifiers(static_cast<SDL_Keymod>(event.keysym.mod), captureSystemKeys),
            mapping->flags);
        noteInputResult(result);
        if (result != 0) return;
        if (pressed) {
            remoteKeysDown[mapping->key] = mapping->flags;
        } else {
            remoteKeysDown.erase(mapping->key);
        }
    }

    void sendMouseButton(Uint8 sdlButton, bool pressed) {
        auto button = terraMouseButton(sdlButton);
        if (!button) return;
        if (settings.swapMouseButtons) {
            if (*button == BUTTON_LEFT) {
                *button = BUTTON_RIGHT;
            } else if (*button == BUTTON_RIGHT) {
                *button = BUTTON_LEFT;
            }
        }
        if (!pressed && !remoteMouseButtonsDown.contains(*button)) return;
        const int result = LiSendMouseButtonEvent(
            pressed ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, *button);
        noteInputResult(result);
        if (result != 0) return;
        if (pressed) {
            remoteMouseButtonsDown.insert(*button);
        } else {
            remoteMouseButtonsDown.erase(*button);
        }
    }

    void handleMouseButton(const SDL_MouseButtonEvent& event) {
        if (event.which == SDL_TOUCH_MOUSEID || overlayCaptureSuspended) return;
        const bool pressed = event.state == SDL_PRESSED;
        if (settings.absoluteMouseMode && !absoluteInputCaptured) {
            if (!pressed && event.button == SDL_BUTTON_LEFT) {
                absoluteInputCaptured = true;
                applyPointerRegionLock();
                updateKeyboardGrab();
                updateWindowTitle();
            }
            return;
        }
        if (!settings.absoluteMouseMode && !mouseCaptured) {
            if (!pressed && event.button == SDL_BUTTON_LEFT) setMouseCaptured(true);
            return;
        }
        if (settings.absoluteMouseMode && pressed) {
            const auto bounds = videoBounds();
            if (event.x < bounds.x || event.x >= bounds.x + bounds.w || event.y < bounds.y ||
                event.y >= bounds.y + bounds.h) {
                return;
            }
        }
        sendMouseButton(event.button, pressed);
    }

    void handleMouseMotion(const SDL_MouseMotionEvent& event) {
        if (event.which == SDL_TOUCH_MOUSEID || overlayCaptureSuspended) return;
        if (settings.absoluteMouseMode && !absoluteInputCaptured) return;
        if (settings.absoluteMouseMode) {
            const auto bounds = videoBounds();
            const int referenceWidth = std::min(bounds.w, 32767);
            const int referenceHeight = std::min(bounds.h, 32767);
            const int x = std::clamp(event.x - bounds.x, 0, bounds.w) * referenceWidth / bounds.w;
            const int y =
                std::clamp(event.y - bounds.y, 0, bounds.h) * referenceHeight / bounds.h;
            noteInputResult(LiSendMousePositionEvent(
                static_cast<short>(x), static_cast<short>(y),
                static_cast<short>(referenceWidth), static_cast<short>(referenceHeight)));
        } else if (mouseCaptured && (event.xrel != 0 || event.yrel != 0)) {
            noteInputResult(LiSendMouseMoveEvent(clampedShort(event.xrel), clampedShort(event.yrel)));
        }
    }

    void handleMouseWheel(const SDL_MouseWheelEvent& event) {
        if (event.which == SDL_TOUCH_MOUSEID || overlayCaptureSuspended ||
            (settings.absoluteMouseMode ? !absoluteInputCaptured : !mouseCaptured)) {
            return;
        }
        if (settings.absoluteMouseMode) {
            int mouseX = 0;
            int mouseY = 0;
            SDL_GetMouseState(&mouseX, &mouseY);
            const auto bounds = videoBounds();
            if (mouseX < bounds.x || mouseX >= bounds.x + bounds.w || mouseY < bounds.y ||
                mouseY >= bounds.y + bounds.h) {
                return;
            }
        }
        float x = static_cast<float>(event.x);
        float y = static_cast<float>(event.y);
#if SDL_VERSION_ATLEAST(2, 0, 18)
        x = event.preciseX;
        y = event.preciseY;
#endif
        const float direction = settings.reverseScrollDirection ? -1.0F : 1.0F;
        if (y != 0.0F) {
            noteInputResult(LiSendHighResScrollEvent(
                clampedShort(static_cast<int>(std::lround(y * direction * 120.0F)))));
        }
        if (x != 0.0F) {
            const int result = LiSendHighResHScrollEvent(
                clampedShort(static_cast<int>(std::lround(x * direction * 120.0F))));
            if (result != LI_ERR_UNSUPPORTED) noteInputResult(result);
        }
    }

    std::pair<float, float> normalizedTouch(float x, float y) const {
        int windowWidth = 1;
        int windowHeight = 1;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        const auto bounds = videoBounds();
        const auto pixelX = x * std::max(windowWidth, 1);
        const auto pixelY = y * std::max(windowHeight, 1);
        return {
            std::clamp((pixelX - bounds.x) / bounds.w, 0.0F, 1.0F),
            std::clamp((pixelY - bounds.y) / bounds.h, 0.0F, 1.0F),
        };
    }

    void emulateDirectTouch(const SDL_TouchFingerEvent& event, float x, float y) {
        if (event.type == SDL_FINGERDOWN && !fallbackMouseFinger) {
            fallbackMouseFinger = event.fingerId;
        }
        if (!fallbackMouseFinger || *fallbackMouseFinger != event.fingerId) return;
        noteInputResult(LiSendMousePositionEvent(
            static_cast<short>(std::lround(x * std::min(streamWidth, 32767))),
            static_cast<short>(std::lround(y * std::min(streamHeight, 32767))),
            static_cast<short>(std::min(streamWidth, 32767)),
            static_cast<short>(std::min(streamHeight, 32767))));
        if (event.type == SDL_FINGERDOWN) {
            sendMouseButton(SDL_BUTTON_LEFT, true);
        } else if (event.type == SDL_FINGERUP) {
            sendMouseButton(SDL_BUTTON_LEFT, false);
            if (!remoteMouseButtonsDown.contains(BUTTON_LEFT)) fallbackMouseFinger.reset();
        }
    }

    bool touchDeviceAccepted(SDL_TouchID touchId) const {
#if SDL_VERSION_ATLEAST(2, 0, 10)
        const auto type = SDL_GetTouchDeviceType(touchId);
        return type == SDL_TOUCH_DEVICE_DIRECT || type == SDL_TOUCH_DEVICE_INVALID;
#else
        static_cast<void>(touchId);
        return true;
#endif
    }

    void handleDirectTouch(const SDL_TouchFingerEvent& event) {
        auto finger = fingers.find(event.fingerId);
        if (event.type == SDL_FINGERDOWN) {
            finger = fingers.emplace(event.fingerId,
                                     FingerState{nextPointerId++, event.x, event.y})
                         .first;
        } else if (finger == fingers.end()) {
            return;
        }
        const auto [x, y] = normalizedTouch(event.x, event.y);
        const std::uint8_t eventType = event.type == SDL_FINGERDOWN ? LI_TOUCH_EVENT_DOWN
                                       : event.type == SDL_FINGERUP ? LI_TOUCH_EVENT_UP
                                                                    : LI_TOUCH_EVENT_MOVE;
        int result = LI_ERR_UNSUPPORTED;
        if ((LiGetHostFeatureFlags() & LI_FF_PEN_TOUCH_EVENTS) != 0) {
            result = LiSendTouchEvent(eventType, finger->second.pointerId, x, y, event.pressure,
                                      0, 0, LI_ROT_UNKNOWN);
        }
        if (result == LI_ERR_UNSUPPORTED) {
            emulateDirectTouch(event, x, y);
        } else {
            noteInputResult(result);
        }
        if (event.type == SDL_FINGERUP) fingers.erase(finger);
    }

    void handleTrackpadTouch(const SDL_TouchFingerEvent& event) {
        if (event.type == SDL_FINGERDOWN) {
            if (fingers.empty()) {
                gestureStartedAt = event.timestamp;
                gestureMaximumFingers = 0;
                gestureMoved = false;
                gestureDistance = 0;
            }
            fingers[event.fingerId] = {nextPointerId++, event.x, event.y};
            if (!primaryFinger) primaryFinger = event.fingerId;
            gestureMaximumFingers = std::max(gestureMaximumFingers, fingers.size());
            return;
        }
        const auto finger = fingers.find(event.fingerId);
        if (finger == fingers.end()) return;
        if (event.type == SDL_FINGERMOTION) {
            const auto deltaX = event.x - finger->second.lastX;
            const auto deltaY = event.y - finger->second.lastY;
            finger->second.lastX = event.x;
            finger->second.lastY = event.y;
            gestureDistance += std::abs(deltaX) + std::abs(deltaY);
            if (gestureDistance > 0.003F) gestureMoved = true;
            if (primaryFinger && *primaryFinger == event.fingerId &&
                (deltaX != 0 || deltaY != 0)) {
                noteInputResult(LiSendMouseMoveEvent(
                    clampedShort(static_cast<int>(std::lround(deltaX * streamWidth * 1.5F))),
                    clampedShort(static_cast<int>(std::lround(deltaY * streamHeight * 1.5F)))));
            }
            return;
        }
        fingers.erase(finger);
        if (primaryFinger && *primaryFinger == event.fingerId) {
            primaryFinger = fingers.empty() ? std::nullopt
                                            : std::optional<SDL_FingerID>{fingers.begin()->first};
        }
        if (!fingers.empty()) return;
        if (!gestureMoved && event.timestamp - gestureStartedAt <= 300) {
            const int button = gestureMaximumFingers >= 2 ? BUTTON_RIGHT : BUTTON_LEFT;
            sendMouseButton(button == BUTTON_RIGHT ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, true);
            sendMouseButton(button == BUTTON_RIGHT ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT, false);
        }
        gestureMaximumFingers = 0;
        gestureMoved = false;
        gestureDistance = 0;
    }

    void handleTouch(const SDL_TouchFingerEvent& event) {
        if (overlayCaptureSuspended) return;
        if (!touchDeviceAccepted(event.touchId)) return;
        if (settings.touchscreenTrackpad) {
            handleTrackpadTouch(event);
        } else {
            handleDirectTouch(event);
        }
    }
};

InputForwarder::InputForwarder() : impl_(std::make_unique<Impl>()) {}

InputForwarder::~InputForwarder() { stop(); }

void InputForwarder::start(SDL_Window* window, InputSettings settings, int width, int height,
                           int fps, std::function<void()> toggleStatistics,
                           std::function<bool()> toggleFullscreen,
                           OverlayListener overlayListener,
                           OverlayCaptureListener overlayCaptureListener,
                           InputOverlayState overlayState) {
    if (impl_->window) throw std::runtime_error("Input forwarding is already active");
    impl_->window = window;
    impl_->settings = settings;
    impl_->toggleStatistics = std::move(toggleStatistics);
    impl_->toggleFullscreen = std::move(toggleFullscreen);
    impl_->overlayListener = std::move(overlayListener);
    impl_->overlayCaptureListener = std::move(overlayCaptureListener);
    impl_->overlayRevision = overlayState.revision;
    impl_->overlayDesiredVisible = overlayState.visible;
    impl_->overlayCaptureSuspended = overlayState.captureSuspended;
    impl_->overlayShortcutArmed = overlayState.visible || overlayState.captureSuspended;
    impl_->restoreRelativeCapture = overlayState.captureSuspended && !settings.absoluteMouseMode;
    impl_->restoreAbsoluteCapture = overlayState.captureSuspended && settings.absoluteMouseMode;
    impl_->windowFocused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    impl_->streamWidth = std::max(width, 1);
    impl_->streamHeight = std::max(height, 1);
    impl_->streamLabel = std::to_string(width) + "x" + std::to_string(height) + " @ " +
                          std::to_string(fps) + " FPS";
    if (const char* previous = SDL_GetHint("SDL_ALLOW_ALT_TAB_WHILE_GRABBED")) {
        impl_->previousAltTabHint = previous;
    }
    if (settings.controllersEnabled) impl_->startControllers();
    impl_->updateWindowTitle();
}

void InputForwarder::closeOverlay(std::uint64_t revision) { impl_->closeOverlay(revision); }

bool InputForwarder::acknowledgeOverlayHidden(std::uint64_t revision) {
    return impl_->acknowledgeOverlayHidden(revision);
}

void InputForwarder::updateOverlay() { impl_->updateOverlay(); }

void InputForwarder::setEnabled(bool enabled) {
    if (!impl_->window || impl_->enabled == enabled) return;
    if (!enabled) {
        impl_->cancelTouchState();
        impl_->stopGamepadRumble();
        if (impl_->mouseCaptured) {
            impl_->setMouseCaptured(false);
        } else {
            impl_->releaseRemoteStateWithRetry();
        }
        if (impl_->settings.controllersEnabled) {
            for (std::size_t index = 0; index < impl_->controllers.size(); ++index) {
                impl_->cancelControllerTouches(index);
            }
            if (impl_->gamepadMask() != 0) impl_->sendNeutralControllerStates(0);
        }
    } else {
        impl_->enabled = true;
        impl_->controllerSuppressed = !impl_->backgroundControllerEvents &&
                                      (SDL_GetWindowFlags(impl_->window) &
                                       SDL_WINDOW_INPUT_FOCUS) == 0;
        if (impl_->settings.controllersEnabled) {
            impl_->refreshControllerStates();
            impl_->announceControllers();
            if (impl_->gamepadMask() == 0) {
                impl_->sendNeutralControllerState(0, 0);
            } else if (impl_->controllerSuppressed) {
                impl_->sendNeutralControllerStates(impl_->gamepadMask());
            } else {
                impl_->sendControllerStates();
            }
        }
    }
    impl_->enabled = enabled;
    impl_->updateKeyboardGrab();
    impl_->updateWindowTitle();
}

void InputForwarder::setGamepadRumble(std::uint16_t controllerNumber,
                                      std::uint16_t lowFrequency,
                                      std::uint16_t highFrequency) {
    impl_->setGamepadRumble(controllerNumber, lowFrequency, highFrequency);
}

void InputForwarder::setGamepadTriggerRumble(std::uint16_t controllerNumber,
                                              std::uint16_t leftTrigger,
                                              std::uint16_t rightTrigger) {
    impl_->setGamepadTriggerRumble(controllerNumber, leftTrigger, rightTrigger);
}

void InputForwarder::setGamepadMotionEventState(std::uint16_t controllerNumber,
                                                 std::uint8_t motionType,
                                                 std::uint16_t reportRateHz) {
    impl_->setGamepadMotionEventState(controllerNumber, motionType, reportRateHz);
}

void InputForwarder::setGamepadLed(std::uint16_t controllerNumber, std::uint8_t red,
                                    std::uint8_t green, std::uint8_t blue) {
    impl_->setGamepadLed(controllerNumber, red, green, blue);
}

void InputForwarder::updateGamepads() {
    if (impl_->settings.controllersEnabled) impl_->updateGamepads();
}

void InputForwarder::stop() {
    if (!impl_->window) return;
    setEnabled(false);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetWindowGrab(impl_->window, SDL_FALSE);
    SDL_SetWindowKeyboardGrab(impl_->window, SDL_FALSE);
    SDL_ResetHint("SDL_ALLOW_ALT_TAB_WHILE_GRABBED");
    if (impl_->previousAltTabHint) {
        SDL_SetHint("SDL_ALLOW_ALT_TAB_WHILE_GRABBED", impl_->previousAltTabHint->c_str());
    }
    impl_->previousAltTabHint.reset();
    impl_->stopControllers();
    impl_->window = nullptr;
    impl_->toggleStatistics = {};
    impl_->toggleFullscreen = {};
    impl_->overlayListener = {};
    impl_->overlayCaptureListener = {};
    impl_->consumedShortcutKeys.clear();
    impl_->localKeysDown.clear();
    impl_->windowFocused = false;
    impl_->overlayCaptureSuspended = false;
    impl_->overlayDesiredVisible = false;
    impl_->overlayShortcutArmed = false;
    impl_->overlayResumePending = false;
    impl_->overlayResumeRetryAt = 0;
    impl_->overlayRevision = 0;
    impl_->restoreRelativeCapture = false;
    impl_->restoreAbsoluteCapture = false;
}

void InputForwarder::handleEvent(const SDL_Event& event) {
    if (!impl_->window) return;
    if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED ||
        event.type == SDL_CONTROLLERDEVICEREMAPPED) {
        impl_->handleControllerDevice(event.cdevice);
        return;
    }
    if (event.type == SDL_CONTROLLERAXISMOTION) {
        impl_->handleControllerAxis(event.caxis);
        return;
    }
    if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERBUTTONUP) {
        impl_->handleControllerButton(event.cbutton);
        return;
    }
    if (event.type == SDL_CONTROLLERSENSORUPDATE) {
        impl_->handleControllerSensor(event.csensor);
        return;
    }
    if (event.type == SDL_CONTROLLERTOUCHPADDOWN ||
        event.type == SDL_CONTROLLERTOUCHPADMOTION || event.type == SDL_CONTROLLERTOUCHPADUP) {
        impl_->handleControllerTouchpad(event.ctouchpad);
        return;
    }
#if SDL_VERSION_ATLEAST(2, 24, 0)
    if (event.type == SDL_JOYBATTERYUPDATED) {
        impl_->handleControllerBattery(event.jbattery);
        return;
    }
#endif
    if (!impl_->enabled) return;
    const auto windowId = SDL_GetWindowID(impl_->window);
    switch (event.type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            if (event.key.windowID == windowId) impl_->handleKeyboard(event.key);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (event.button.windowID == windowId) impl_->handleMouseButton(event.button);
            break;
        case SDL_MOUSEMOTION:
            if (event.motion.windowID == windowId) impl_->handleMouseMotion(event.motion);
            break;
        case SDL_MOUSEWHEEL:
            if (event.wheel.windowID == windowId) impl_->handleMouseWheel(event.wheel);
            break;
        case SDL_FINGERDOWN:
        case SDL_FINGERMOTION:
        case SDL_FINGERUP: impl_->handleTouch(event.tfinger); break;
        case SDL_WINDOWEVENT:
            if (event.window.windowID != windowId) break;
            if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                impl_->windowFocused = false;
                if (impl_->mouseCaptured) {
                    impl_->setMouseCaptured(false);
                } else {
                    impl_->releaseRemoteStateWithRetry();
                }
                impl_->setControllerFocus(false);
                impl_->cancelTouchState();
                impl_->updateKeyboardGrab();
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                impl_->windowFocused = true;
                impl_->setControllerFocus(true);
                impl_->updateKeyboardGrab();
                if (impl_->overlayResumePending) impl_->resumeAfterOverlay();
            }
            break;
        default: break;
    }
}

}  // namespace terra

#else

namespace terra {

std::uint16_t connectedGamepadMask() { return 0; }

}  // namespace terra

#endif
