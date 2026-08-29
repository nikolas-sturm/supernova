#include "input_forwarder.h"

#ifdef _WIN32

#include <Limelight.h>
#include <Xinput.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace eclipse {
namespace {

constexpr UINT_PTR kGamepadTimer = 1;
constexpr UINT kGamepadPollMilliseconds = 8;
constexpr std::uint32_t kSupportedGamepadButtons =
    A_FLAG | B_FLAG | X_FLAG | Y_FLAG | UP_FLAG | DOWN_FLAG | LEFT_FLAG | RIGHT_FLAG | LB_FLAG |
    RB_FLAG | PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;
using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFunction loadXInputGetState(HMODULE module) {
    const FARPROC procedure = GetProcAddress(module, "XInputGetState");
    XInputGetStateFunction function = nullptr;
    static_assert(sizeof(function) == sizeof(procedure));
    std::memcpy(&function, &procedure, sizeof(function));
    return function;
}

bool firstConnectedController(XInputGetStateFunction getState, DWORD& index, XINPUT_STATE& state) {
    if (!getState) return false;
    for (DWORD candidate = 0; candidate < XUSER_MAX_COUNT; ++candidate) {
        state = {};
        if (getState(candidate, &state) == ERROR_SUCCESS) {
            index = candidate;
            return true;
        }
    }
    return false;
}

short invertStickAxis(SHORT value) {
    return static_cast<short>(std::clamp(-static_cast<int>(value), -32768, 32767));
}

int moonlightButtons(WORD buttons, bool swapFaceButtons) {
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
    constexpr std::array<const wchar_t*, 3> libraries = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
    for (const wchar_t* library : libraries) {
        const HMODULE module = LoadLibraryW(library);
        if (!module) continue;
        const auto getState = loadXInputGetState(module);
        DWORD index = 0;
        XINPUT_STATE state{};
        const bool connected = firstConnectedController(getState, index, state);
        FreeLibrary(module);
        if (getState) return connected ? 1 : 0;
    }
    return 0;
}

struct InputForwarder::Impl {
    HWND window = nullptr;
    InputSettings settings;
    std::wstring streamLabel;
    bool mouseCaptured = false;
    bool cursorHidden = false;
    std::unordered_set<short> keysDown;
    std::unordered_set<int> mouseButtonsDown;
    HMODULE xinputModule = nullptr;
    XInputGetStateFunction xinputGetState = nullptr;
    bool gamepadConnected = false;
    DWORD gamepadIndex = XUSER_MAX_COUNT;
    XINPUT_STATE gamepadState{};
    bool gamepadSuppressed = false;
    std::optional<POINT> absoluteMousePosition;
    std::optional<int> inputResult;

    void updateWindowTitle() const {
        if (!window) return;
        std::wstring title = L"Eclipse Stream [" + streamLabel + L"] - ";
        if (inputResult && *inputResult != 0) {
            title += L"Input queue failed (" + std::to_wstring(*inputResult) + L")";
        } else if (settings.absoluteMouseMode) {
            title += L"Absolute pointer active";
        } else {
            title += mouseCaptured ? L"Input captured" : L"Click to capture input";
        }
        if (mouseCaptured) title += L" (F8 to release)";
        SetWindowTextW(window, title.c_str());
    }

    void noteInputResult(int result) {
        if (!inputResult || (*inputResult == 0 && result != 0)) {
            inputResult = result;
            updateWindowTitle();
        }
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
        if (keysDown.contains(VK_LWIN) || keysDown.contains(VK_RWIN)) {
            result |= MODIFIER_META;
        }
        return result;
    }

    void updateCursorClip() const {
        if (!mouseCaptured || !window) return;
        RECT rect{};
        if (!GetClientRect(window, &rect)) return;
        POINT topLeft{rect.left, rect.top};
        POINT bottomRight{rect.right, rect.bottom};
        if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight)) return;
        rect = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
        ClipCursor(&rect);
    }

    void releaseRemoteState() {
        for (const short key : keysDown) {
            LiSendKeyboardEvent(static_cast<short>(0x8000 | key), KEY_ACTION_UP, 0);
        }
        keysDown.clear();
        for (const int button : mouseButtonsDown) {
            LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, button);
        }
        mouseButtonsDown.clear();
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

    void handleKeyboard(WPARAM wparam, LPARAM lparam, bool pressed) {
        const short key = normalizedVirtualKey(wparam, lparam);
        if (key == VK_F8) {
            if (pressed && (lparam & (1LL << 30)) == 0) setMouseCaptured(!mouseCaptured);
            return;
        }
        if (pressed && (lparam & (1LL << 30)) != 0) return;
        if (pressed) {
            keysDown.insert(key);
        } else {
            keysDown.erase(key);
        }
        noteInputResult(LiSendKeyboardEvent(static_cast<short>(0x8000 | key),
                                            pressed ? KEY_ACTION_DOWN : KEY_ACTION_UP,
                                            modifiers()));
    }

    void sendMouseButton(int button, bool pressed) {
        if (settings.swapMouseButtons) {
            if (button == BUTTON_LEFT) {
                button = BUTTON_RIGHT;
            } else if (button == BUTTON_RIGHT) {
                button = BUTTON_LEFT;
            }
        }
        if (pressed) {
            mouseButtonsDown.insert(button);
        } else {
            mouseButtonsDown.erase(button);
        }
        noteInputResult(
            LiSendMouseButtonEvent(pressed ? BUTTON_ACTION_PRESS : BUTTON_ACTION_RELEASE, button));
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
        RECT bounds{};
        if (!GetClientRect(window, &bounds)) return;
        const int width = std::max(static_cast<int>(bounds.right - bounds.left), 1);
        const int height = std::max(static_cast<int>(bounds.bottom - bounds.top), 1);
        const auto x = static_cast<short>(std::clamp(GET_X_LPARAM(value), 0, width));
        const auto y = static_cast<short>(std::clamp(GET_Y_LPARAM(value), 0, height));
        noteInputResult(LiSendMousePositionEvent(
            x, y, static_cast<short>(std::min(width, 32767)),
            static_cast<short>(std::min(height, 32767))));
    }

    void sendWindowScroll(WPARAM value, bool horizontal) {
        auto amount = GET_WHEEL_DELTA_WPARAM(value);
        if (settings.reverseScrollDirection) amount = -amount;
        noteInputResult(horizontal ? LiSendHighResHScrollEvent(static_cast<short>(amount))
                                   : LiSendHighResScrollEvent(static_cast<short>(amount)));
    }

    void pollGamepad() {
        if (!xinputGetState) return;
        if (!settings.backgroundGamepad && GetForegroundWindow() != window) {
            if (!gamepadSuppressed && gamepadConnected) {
                LiSendMultiControllerEvent(0, settings.forceGamepad ? 1 : 0, 0, 0, 0, 0, 0, 0, 0);
                gamepadSuppressed = true;
            }
            return;
        }
        const bool wasSuppressed = gamepadSuppressed;
        gamepadSuppressed = false;
        XINPUT_STATE state{};
        DWORD connectedIndex = gamepadIndex;
        bool connected = gamepadIndex < XUSER_MAX_COUNT &&
                         xinputGetState(gamepadIndex, &state) == ERROR_SUCCESS;
        if (!connected) connected = firstConnectedController(xinputGetState, connectedIndex, state);
        if (!connected) {
            if (settings.forceGamepad) {
                if (gamepadIndex < XUSER_MAX_COUNT) {
                    LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0);
                    gamepadIndex = XUSER_MAX_COUNT;
                    gamepadState = {};
                }
                return;
            }
            if (gamepadConnected) {
                LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
                gamepadConnected = false;
                gamepadState = {};
                gamepadIndex = XUSER_MAX_COUNT;
            }
            return;
        }
        if (!gamepadConnected || connectedIndex != gamepadIndex) {
            if (gamepadConnected && !settings.forceGamepad) {
                LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
            }
            if (!settings.forceGamepad || !gamepadConnected) {
                noteInputResult(LiSendControllerArrivalEvent(
                    0, 1, LI_CTYPE_XBOX, kSupportedGamepadButtons, LI_CCAP_ANALOG_TRIGGERS));
            }
            gamepadConnected = true;
            gamepadIndex = connectedIndex;
        } else if (!wasSuppressed && state.dwPacketNumber == gamepadState.dwPacketNumber) {
            return;
        }
        gamepadState = state;
        const XINPUT_GAMEPAD& pad = state.Gamepad;
        noteInputResult(LiSendMultiControllerEvent(
            0, 1, moonlightButtons(pad.wButtons, settings.swapFaceButtons), pad.bLeftTrigger,
            pad.bRightTrigger, pad.sThumbLX, invertStickAxis(pad.sThumbLY), pad.sThumbRX,
            invertStickAxis(pad.sThumbRY)));
    }

    void loadXInput() {
        constexpr std::array<const wchar_t*, 3> libraries = {
            L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        for (const wchar_t* library : libraries) {
            xinputModule = LoadLibraryW(library);
            if (!xinputModule) continue;
            xinputGetState = loadXInputGetState(xinputModule);
            if (xinputGetState) return;
            FreeLibrary(xinputModule);
            xinputModule = nullptr;
        }
    }
};

InputForwarder::InputForwarder() : impl_(std::make_unique<Impl>()) {}

InputForwarder::~InputForwarder() { stop(); }

void InputForwarder::start(HWND window, InputSettings settings, int width, int height, int fps) {
    if (impl_->window) throw std::runtime_error("Input forwarding is already active");
    impl_->window = window;
    impl_->settings = settings;
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
    impl_->loadXInput();
    if (settings.forceGamepad) {
        impl_->noteInputResult(LiSendControllerArrivalEvent(
            0, 1, LI_CTYPE_XBOX, kSupportedGamepadButtons, LI_CCAP_ANALOG_TRIGGERS));
        impl_->gamepadConnected = true;
    }
    SetTimer(window, kGamepadTimer, kGamepadPollMilliseconds, nullptr);
    impl_->updateWindowTitle();
}

void InputForwarder::stop() {
    if (!impl_->window) return;
    KillTimer(impl_->window, kGamepadTimer);
    impl_->setMouseCaptured(false);
    impl_->releaseRemoteState();
    if (impl_->gamepadConnected) {
        LiSendMultiControllerEvent(0, 0, 0, 0, 0, 0, 0, 0, 0);
        impl_->gamepadConnected = false;
    }
    impl_->window = nullptr;
    if (impl_->xinputModule) {
        FreeLibrary(impl_->xinputModule);
        impl_->xinputModule = nullptr;
        impl_->xinputGetState = nullptr;
    }
}

bool InputForwarder::handleMessage(UINT message, WPARAM wparam, LPARAM lparam, LRESULT& result) {
    switch (message) {
        case WM_INPUT:
            impl_->handleRawInput(reinterpret_cast<HRAWINPUT>(lparam));
            result = 0;
            return true;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            impl_->handleKeyboard(wparam, lparam, true);
            result = 0;
            return true;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            impl_->handleKeyboard(wparam, lparam, false);
            result = 0;
            return true;
        case WM_LBUTTONDOWN:
            if (impl_->settings.absoluteMouseMode) {
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
            if (impl_->settings.absoluteMouseMode) {
                impl_->sendMouseButton(BUTTON_RIGHT, message == WM_RBUTTONDOWN);
                result = 0;
                return true;
            }
            break;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            if (impl_->settings.absoluteMouseMode) {
                impl_->sendMouseButton(BUTTON_MIDDLE, message == WM_MBUTTONDOWN);
                result = 0;
                return true;
            }
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            if (impl_->settings.absoluteMouseMode) {
                const auto button = GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? BUTTON_X1 : BUTTON_X2;
                impl_->sendMouseButton(button, message == WM_XBUTTONDOWN);
                result = TRUE;
                return true;
            }
            break;
        case WM_MOUSEMOVE:
            if (impl_->settings.absoluteMouseMode) {
                impl_->sendAbsoluteMousePosition(lparam);
                result = 0;
                return true;
            }
            break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            if (impl_->settings.absoluteMouseMode) {
                impl_->sendWindowScroll(wparam, message == WM_MOUSEHWHEEL);
                result = 0;
                return true;
            }
            break;
        case WM_ACTIVATEAPP:
            if (wparam == FALSE) {
                if (impl_->settings.absoluteMouseMode) {
                    impl_->releaseRemoteState();
                } else {
                    impl_->setMouseCaptured(false);
                }
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
            if (impl_->mouseCaptured) {
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

}  // namespace eclipse

#else

namespace eclipse {

std::uint16_t connectedGamepadMask() { return 0; }

}  // namespace eclipse

#endif
