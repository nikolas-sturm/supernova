#pragma once

#include <string_view>

namespace terra {

// An explicit driver (even empty) or initialized video owns backend selection.
inline bool preferNativeWayland(bool videoInitialized,
                                const char* configuredDriver,
                                const char* sessionType,
                                const char* waylandDisplay) {
    if (videoInitialized || configuredDriver != nullptr) {
        return false;
    }
    if (sessionType != nullptr && sessionType[0] != '\0') {
        return std::string_view(sessionType) == "wayland";
    }
    return waylandDisplay != nullptr && waylandDisplay[0] != '\0';
}

}  // namespace terra
