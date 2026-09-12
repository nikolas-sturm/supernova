#include "client_displays.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <tuple>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cwchar>

namespace terra {
namespace {

struct MonitorContext {
    std::vector<ClientDisplayOutput>* outputs;
};

std::string toUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const auto size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& outputs = *reinterpret_cast<MonitorContext*>(context)->outputs;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;

    ClientDisplayOutput output;
    output.id = toUtf8(info.szDevice);
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    output.name = EnumDisplayDevicesW(info.szDevice, 0, &device, 0) ? toUtf8(device.DeviceString) : std::string {};
    if (output.name.empty()) output.name = output.id;
    output.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    output.x = info.rcMonitor.left;
    output.y = info.rcMonitor.top;
    output.width = info.rcMonitor.right - info.rcMonitor.left;
    output.height = info.rcMonitor.bottom - info.rcMonitor.top;

    std::set<std::tuple<int, int, int>> seen;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    for (DWORD index = 0; EnumDisplaySettingsExW(info.szDevice, index, &mode, 0); ++index) {
        if (mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0) continue;
        const auto refreshRate = mode.dmDisplayFrequency <= 1 ? 60 : static_cast<int>(mode.dmDisplayFrequency);
        if (!seen.emplace(static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight), refreshRate).second) {
            continue;
        }
        output.modes.push_back({static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight), refreshRate});
    }
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &current, 0)) {
        output.width = static_cast<int>(current.dmPelsWidth);
        output.height = static_cast<int>(current.dmPelsHeight);
        output.refreshRate = current.dmDisplayFrequency <= 1 ? 60 : static_cast<int>(current.dmDisplayFrequency);
    }
    if (output.width <= 0 || output.height <= 0) {
        output.width = info.rcMonitor.right - info.rcMonitor.left;
        output.height = info.rcMonitor.bottom - info.rcMonitor.top;
    }
    if (output.refreshRate <= 0) output.refreshRate = 60;
    if (output.modes.empty()) {
        output.modes.push_back({output.width, output.height, output.refreshRate});
    }
    std::ranges::sort(output.modes, [](const auto& left, const auto& right) {
        return std::tie(left.width, left.height, left.refreshRate) <
               std::tie(right.width, right.height, right.refreshRate);
    });
    if (output.name.empty()) output.name = "Display " + std::to_string(outputs.size() + 1);
    if (output.id.empty()) output.id = output.name;
    outputs.push_back(std::move(output));
    return TRUE;
}

}  // namespace

std::vector<ClientDisplayOutput> enumerateClientDisplays() {
    std::vector<ClientDisplayOutput> outputs;
    MonitorContext context{&outputs};
    EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&context));
    std::ranges::sort(outputs, [](const auto& left, const auto& right) {
        if (left.primary != right.primary) return left.primary;
        return std::tie(left.x, left.y) < std::tie(right.x, right.y);
    });
    // ponytail: duplicate device ids keep the trailing index in the platform name,
    // which is stable for a fixed port; revisit if users hot-swap identical monitors.
    return outputs;
}

}  // namespace terra

#elif defined(__linux__) && defined(TERRA_HAS_LINUX_VIDEO)

#include <SDL.h>

namespace terra {
namespace {

std::string uniqueId(std::string name, int index, const std::vector<ClientDisplayOutput>& outputs) {
    if (name.empty()) name = "Display " + std::to_string(index + 1);
    if (std::ranges::none_of(outputs, [&](const auto& output) { return output.id == name; })) {
        return name;
    }
    return name + " #" + std::to_string(index + 1);
}

}  // namespace

// ponytail: reads SDL display state without a lock; enumeration happens before
// a stream starts or on explicit rescan, so contention with the renderer is rare.
// Revisit if SDL asserts on concurrent video queries.
std::vector<ClientDisplayOutput> enumerateClientDisplays() {
    std::vector<ClientDisplayOutput> outputs;
    const bool initialized = SDL_WasInit(SDL_INIT_VIDEO) != 0;
    if (!initialized && SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        return outputs;
    }
    const auto displayCount = SDL_GetNumVideoDisplays();
    for (int index = 0; index < displayCount; ++index) {
        ClientDisplayOutput output;
        const auto* displayName = SDL_GetDisplayName(index);
        output.name = displayName ? displayName : std::string {};
        output.id = uniqueId(output.name, index, outputs);
        output.primary = index == 0;
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(index, &bounds) == 0) {
            output.x = bounds.x;
            output.y = bounds.y;
            output.width = bounds.w;
            output.height = bounds.h;
        }
        SDL_DisplayMode desktop{};
        if (SDL_GetDesktopDisplayMode(index, &desktop) == 0) {
            output.width = desktop.w;
            output.height = desktop.h;
            output.refreshRate = desktop.refresh_rate > 0 ? desktop.refresh_rate : 60;
        }
        const auto modeCount = SDL_GetNumDisplayModes(index);
        std::set<std::tuple<int, int, int>> seen;
        for (int modeIndex = 0; modeIndex < modeCount; ++modeIndex) {
            SDL_DisplayMode mode{};
            if (SDL_GetDisplayMode(index, modeIndex, &mode) != 0) continue;
            if (mode.w <= 0 || mode.h <= 0) continue;
            const auto refreshRate = mode.refresh_rate > 0 ? mode.refresh_rate : 60;
            if (!seen.emplace(mode.w, mode.h, refreshRate).second) continue;
            output.modes.push_back({mode.w, mode.h, refreshRate});
        }
        if (output.refreshRate <= 0) output.refreshRate = 60;
        if (output.width <= 0 || output.height <= 0) {
            output.width = 1920;
            output.height = 1080;
        }
        if (output.modes.empty()) {
            output.modes.push_back({output.width, output.height, output.refreshRate});
        }
        std::ranges::sort(output.modes, [](const auto& left, const auto& right) {
            return std::tie(left.width, left.height, left.refreshRate) <
                   std::tie(right.width, right.height, right.refreshRate);
        });
        outputs.push_back(std::move(output));
    }
    if (!initialized) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    return outputs;
}

}  // namespace terra

#else

namespace terra {

std::vector<ClientDisplayOutput> enumerateClientDisplays() { return {}; }

}  // namespace terra

#endif
