#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>

namespace eclipse {

struct DisplayModeSpec {
    int width = 0;
    int height = 0;
    int refreshRate = 0;

    friend bool operator==(const DisplayModeSpec&, const DisplayModeSpec&) = default;
};

[[nodiscard]] constexpr bool displayModeMatchesFrameRate(int refreshRate,
                                                          int frameRate) noexcept {
    if (refreshRate <= 0 || frameRate <= 0) return false;
    const auto multiple = std::max((refreshRate + frameRate / 2) / frameRate, 1);
    const auto difference = refreshRate - multiple * frameRate;
    return difference == 0 || difference == -1;
}

[[nodiscard]] inline DisplayModeSpec selectOptimalDisplayMode(
    std::span<const DisplayModeSpec> modes, DisplayModeSpec desktopMode, int videoWidth,
    int videoHeight, int frameRate) noexcept {
    DisplayModeSpec best{};
    for (const auto& mode : modes) {
        if (mode.width == desktopMode.width && mode.height == desktopMode.height &&
            displayModeMatchesFrameRate(mode.refreshRate, frameRate) &&
            mode.refreshRate > best.refreshRate) {
            best = mode;
        }
    }
    if (best.refreshRate > 0) return best;

    double bestAspectDifference = 0;
    std::int64_t bestArea = 0;
    const auto videoAspect = videoHeight > 0 ? static_cast<double>(videoWidth) / videoHeight : 0;
    for (const auto& mode : modes) {
        if (mode.width < videoWidth || mode.height < videoHeight || mode.height <= 0 ||
            !displayModeMatchesFrameRate(mode.refreshRate, frameRate)) {
            continue;
        }
        const auto aspectDifference =
            std::abs(videoAspect - static_cast<double>(mode.width) / mode.height);
        const auto area = static_cast<std::int64_t>(mode.width) * mode.height;
        if (best.refreshRate == 0 || aspectDifference < bestAspectDifference ||
            (aspectDifference == bestAspectDifference && area < bestArea) ||
            (aspectDifference == bestAspectDifference && area == bestArea &&
             mode.refreshRate > best.refreshRate)) {
            best = mode;
            bestAspectDifference = aspectDifference;
            bestArea = area;
        }
    }
    return best.refreshRate > 0 ? best : desktopMode;
}

}  // namespace eclipse
