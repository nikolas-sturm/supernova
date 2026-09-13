#include "../src/sdl_video_policy.h"

#include <cstdio>

int main() {
    struct TestCase {
        const char* name;
        bool videoInitialized;
        const char* configuredDriver;
        const char* sessionType;
        const char* waylandDisplay;
        bool expected;
    };

    const TestCase cases[] = {
        {"Wayland session", false, nullptr, "wayland", "wayland-0", true},
        {"Wayland session without display", false, nullptr, "wayland", nullptr, true},
        {"Wayland session with empty display", false, nullptr, "wayland", "", true},
        {"Explicit X11 driver", false, "x11", "wayland", "wayland-0", false},
        {"Explicit Wayland driver", false, "wayland", "wayland", "wayland-0", false},
        {"Explicit driver list", false, "wayland,x11", "wayland", "wayland-0", false},
        {"Explicit automatic choice", false, "", "wayland", "wayland-0", false},
        {"X11 session with Wayland display", false, nullptr, "x11", "wayland-0", false},
        {"Other session with Wayland display", false, nullptr, "tty", "wayland-0", false},
        {"Session match is exact", false, nullptr, "Wayland", "wayland-0", false},
        {"Absent session display fallback", false, nullptr, nullptr, "wayland-0", true},
        {"Empty session display fallback", false, nullptr, "", "wayland-0", true},
        {"Absent session absent display", false, nullptr, nullptr, nullptr, false},
        {"Absent session empty display", false, nullptr, nullptr, "", false},
        {"Empty session absent display", false, nullptr, "", nullptr, false},
        {"Empty session empty display", false, nullptr, "", "", false},
        {"Initialized video in Wayland session", true, nullptr, "wayland", "wayland-0", false},
        {"Initialized video with display fallback", true, nullptr, nullptr, "wayland-0", false},
        {"Explicit driver blocks display fallback", false, "x11", nullptr, "wayland-0", false},
        {"Empty driver blocks display fallback", false, "", "", "wayland-0", false},
    };

    int failures = 0;
    for (const auto& test : cases) {
        if (terra::preferNativeWayland(test.videoInitialized, test.configuredDriver,
                                test.sessionType, test.waylandDisplay) != test.expected) {
            std::fprintf(stderr, "FAIL: %s\n", test.name);
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
