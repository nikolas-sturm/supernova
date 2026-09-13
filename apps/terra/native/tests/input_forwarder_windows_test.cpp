#include "input_forwarder.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include <vector>

extern "C" {
#include <Limelight.h>
}

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Call {
    enum class Kind { position, button } kind;
    short x = 0, y = 0, width = 0, height = 0;
    char action = 0;
    int button = 0;
    int result = 0;
    bool operator==(const Call&) const = default;
};

std::vector<Call> calls;
int nextPositionResult = 0;
int nextReleaseResult = 0;
int unexpectedCalls = 0;

Call position(short x, short y, int result = 0) {
    return {Call::Kind::position, x, y, 32767, 32767, 0, 0, result};
}

Call button(char action, int value, int result = 0) {
    return {Call::Kind::button, 0, 0, 0, 0, action, value, result};
}

void expectCalls(std::initializer_list<Call> expected) {
    expect(calls == std::vector<Call>(expected), "Unexpected transport call order or payload.");
    expect(unexpectedCalls == 0, "Non-mouse transport was called.");
}

// No message pump and no forwarding window procedure: only explicit handleMessage
// calls reach InputForwarder. Hidden SetCapture tests ownership, not OS routing
// across processes or physical monitors. Neither window is shown or activated.
struct Window {
    HWND handle = nullptr;

    Window(int x, int y) {
        handle = CreateWindowExW(WS_EX_NOACTIVATE, L"STATIC", L"Terra input test",
                                 WS_POPUP, x, y, 400, 200, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), nullptr);
        expect(handle != nullptr, "Could not create hidden test HWND.");
    }
    ~Window() {
        if (GetCapture() == handle) ReleaseCapture();
        DestroyWindow(handle);
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
};

// stop() unhooks but does not unregister raw input. This guard also handles
// start() throwing after raw registration and before hook installation succeeds.
// This executable owns its process's raw mouse registration; run it standalone.
struct RawMouseRegistration {
    ~RawMouseRegistration() {
        RAWINPUTDEVICE mouse{};
        mouse.usUsagePage = 0x01;
        mouse.usUsage = 0x02;
        mouse.dwFlags = RIDEV_REMOVE;
        RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
};

struct Fixture {
    Window source{500, 300};
    Window sibling{100, 300};
    RawMouseRegistration rawRegistration;
    terra::InputForwarder input; // Destroyed before raw registration and HWNDs.

    explicit Fixture(bool swapped = false) {
        calls.clear();
        nextPositionResult = nextReleaseResult = unexpectedCalls = 0;
        expect(GetCapture() == nullptr, "Test thread already owns mouse capture.");
        expect(!IsWindowVisible(source.handle) && !IsWindowVisible(sibling.handle),
               "Test windows must remain hidden.");
        RECT client{};
        POINT origin{};
        expect(GetClientRect(source.handle, &client) &&
                   ClientToScreen(source.handle, &origin),
               "Could not query source geometry.");
        expect(client.left == 0 && client.top == 0 && client.right == 400 &&
                   client.bottom == 200,
               "Unexpected popup client dimensions.");

        terra::InputSettings settings;
        settings.absoluteMouseMode = true;
        settings.fullscreen = true;
        settings.controllersEnabled = false;
        settings.captureSystemKeys = terra::SystemKeyCapture::off;
        settings.swapMouseButtons = swapped;
        // Synthetic left sibling: local and remote geometry need no real monitor.
        // Source remote origin is nonzero to exercise source-relative translation.
        const int sourceX = static_cast<int>(origin.x);
        const int sourceY = static_cast<int>(origin.y);
        settings.workspaceMouse = {
            {{sourceX, sourceY, 400, 200}, {800, 200, 800, 400}},
            {{sourceX - 400, sourceY, 400, 200}, {0, 200, 800, 400}},
        };
        input.start(source.handle, settings, 800, 400, 60);
        input.setEnabled(true);
        expectCalls({});
    }

    void mouse(UINT message, int x, int y, WPARAM buttons = 0) {
        LRESULT result = -1;
        const LPARAM point = MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
        expect(input.handleMessage(message, buttons, point, result) && result == 0,
               "Mouse message was not handled.");
    }

    void dispatch(UINT message, WPARAM wparam = 0, LPARAM lparam = 0) {
        LRESULT result = 0;
        // Lifecycle messages intentionally fall through to the window procedure.
        input.handleMessage(message, wparam, lparam, result);
    }

    void captured(bool owned) const {
        expect((GetCapture() == source.handle) == owned,
               "Unexpected source HWND capture ownership.");
    }

    void finish() {
        const auto before = calls;
        input.stop();
        expect(calls == before, "Stop retried an already released remote button.");
        expect(unexpectedCalls == 0, "Unexpected transport during stop.");
        captured(false);
    }
};

void drag(bool swapped) {
    Fixture test(swapped);
    const int mapped = swapped ? BUTTON_RIGHT : BUTTON_LEFT;
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.captured(true);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, mapped)});
    test.mouse(WM_MOUSEMOVE, -200, 100, MK_LBUTTON);
    test.captured(true);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, mapped),
                 position(-16383, 16383)});
    test.mouse(WM_LBUTTONUP, -200, 100);
    test.captured(false);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, mapped),
                 position(-16383, 16383), position(-16383, 16383),
                 button(BUTTON_ACTION_RELEASE, mapped)});
    test.finish();
}

void multipleButtons(bool swapped) {
    Fixture test(swapped);
    const int left = swapped ? BUTTON_RIGHT : BUTTON_LEFT;
    const int right = swapped ? BUTTON_LEFT : BUTTON_RIGHT;
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.mouse(WM_RBUTTONDOWN, -200, 100, MK_LBUTTON | MK_RBUTTON);
    test.captured(true);
    test.mouse(WM_LBUTTONUP, -200, 100, MK_RBUTTON);
    test.captured(true);
    test.mouse(WM_MOUSEMOVE, -200, 100, MK_RBUTTON);
    test.captured(true);
    test.mouse(WM_RBUTTONUP, -200, 100);
    test.captured(false);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, left),
                 position(-16383, 16383), button(BUTTON_ACTION_PRESS, right),
                 position(-16383, 16383), button(BUTTON_ACTION_RELEASE, left),
                 position(-16383, 16383), position(-16383, 16383),
                 button(BUTTON_ACTION_RELEASE, right)});
    test.finish();
}

void interruptedDrag(UINT message) {
    Fixture test(true);
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.captured(true);
    if (message == WM_CAPTURECHANGED) {
        // Model the native precondition: capture has already changed. Automatic
        // HWND notifications are not forwarded by our window procedure.
        expect(SetCapture(test.sibling.handle) == test.source.handle,
               "Could not transfer capture to same-process sibling.");
        test.dispatch(message, 0, reinterpret_cast<LPARAM>(test.sibling.handle));
        expect(GetCapture() == test.sibling.handle,
               "Capture-loss cleanup released the new owner's capture.");
    } else {
        test.dispatch(message, FALSE);
        expect(GetCapture() == nullptr, "Interrupted drag retained native capture.");
    }
    test.captured(false);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, BUTTON_RIGHT),
                 button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT)});
    // Repeated cancellation and late physical up must not release twice.
    test.dispatch(message, FALSE);
    test.mouse(WM_LBUTTONUP, -200, 100);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, BUTTON_RIGHT),
                 button(BUTTON_ACTION_RELEASE, BUTTON_RIGHT)});
    test.finish();
}

void failedPositionSuppressesPress() {
    Fixture test;
    nextPositionResult = -1;
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.captured(false);
    expectCalls({position(8192, 8192, -1)});
    test.mouse(WM_LBUTTONUP, 100, 50);
    expectCalls({position(8192, 8192, -1)});
    // The error is one-shot; the next gesture must still work.
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.captured(true);
    test.mouse(WM_LBUTTONUP, -200, 100);
    test.captured(false);
    expectCalls({position(8192, 8192, -1), position(8192, 8192),
                 button(BUTTON_ACTION_PRESS, BUTTON_LEFT), position(-16383, 16383),
                 button(BUTTON_ACTION_RELEASE, BUTTON_LEFT)});
    test.finish();
}

void failedReleaseRetries() {
    Fixture test;
    test.mouse(WM_LBUTTONDOWN, 100, 50, MK_LBUTTON);
    test.captured(true);
    nextReleaseResult = -1;
    test.mouse(WM_LBUTTONUP, -200, 100);
    test.captured(false);
    expectCalls({position(8192, 8192), button(BUTTON_ACTION_PRESS, BUTTON_LEFT),
                 position(-16383, 16383), button(BUTTON_ACTION_RELEASE, BUTTON_LEFT, -1),
                 button(BUTTON_ACTION_RELEASE, BUTTON_LEFT)});
    test.finish();
}
} // namespace

// All Limelight references from the Windows input_forwarder.cpp translation unit.
// Link these stubs instead of moonlight-common-c; no renderer or host connection.
extern "C" int LiSendMousePositionEvent(short x, short y, short width, short height) {
    const int result = std::exchange(nextPositionResult, 0);
    calls.push_back({Call::Kind::position, x, y, width, height, 0, 0, result});
    return result;
}
extern "C" int LiSendMouseButtonEvent(char action, int value) {
    const int result = action == BUTTON_ACTION_RELEASE ? std::exchange(nextReleaseResult, 0) : 0;
    calls.push_back(button(action, value, result));
    return result;
}
extern "C" int LiSendKeyboardEvent(short, char, char) { ++unexpectedCalls; return 0; }
extern "C" int LiSendMouseMoveEvent(short, short) { ++unexpectedCalls; return 0; }
extern "C" int LiSendHighResScrollEvent(short) { ++unexpectedCalls; return 0; }
extern "C" int LiSendHighResHScrollEvent(short) { ++unexpectedCalls; return 0; }
extern "C" std::uint32_t LiGetHostFeatureFlags() { ++unexpectedCalls; return 0; }
extern "C" int LiSendTouchEvent(std::uint8_t, std::uint32_t, float, float, float,
                                float, float, std::uint16_t) {
    ++unexpectedCalls; return 0;
}
extern "C" int LiSendPenEvent(std::uint8_t, std::uint8_t, std::uint8_t, float, float, float,
                              float, float, std::uint16_t, std::uint8_t) {
    ++unexpectedCalls; return 0;
}
extern "C" int LiSendUtf8TextEvent(const char*, unsigned int) { ++unexpectedCalls; return 0; }
extern "C" int LiSendControllerArrivalEvent(std::uint8_t, std::uint16_t, std::uint8_t,
                                            std::uint32_t, std::uint16_t) {
    ++unexpectedCalls; return 0;
}
extern "C" int LiSendMultiControllerEvent(short, short, int, unsigned char, unsigned char,
                                          short, short, short, short) {
    ++unexpectedCalls; return 0;
}

int main() {
    try {
        drag(false);
        drag(true);
        multipleButtons(false);
        multipleButtons(true);
        interruptedDrag(WM_ACTIVATEAPP);
        interruptedDrag(WM_CAPTURECHANGED);
        interruptedDrag(WM_CANCELMODE);
        failedPositionSuppressesPress();
        failedReleaseRetries();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "input_forwarder_windows_test: %s\n", error.what());
        return 1;
    }
}
