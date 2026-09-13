#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace terra {

/** @brief Pixel rectangle in a local or remote desktop. */
struct MouseRectangle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const MouseRectangle&) const = default;
};

/** @brief Fullscreen local output paired with its remote workspace display. */
struct WorkspaceMouseDisplay {
    MouseRectangle local;
    MouseRectangle remote;
    bool operator==(const WorkspaceMouseDisplay&) const = default;
};

/** @brief Signed, source-display-relative Moonlight absolute mouse packet. */
struct WorkspaceMousePosition {
    short x;
    short y;
    short width;
    short height;
};

/**
 * @brief Map a captured desktop pointer through letterboxed stream outputs.
 * The first display owns the connection. All drag events stay on that connection;
 * negotiated Sol workspace input accepts coordinates outside its source display.
 */
inline std::optional<WorkspaceMousePosition> workspaceMousePosition(
    const std::vector<WorkspaceMouseDisplay>& displays, int x, int y) {
    if (displays.size() < 2 || displays.size() > 4) return std::nullopt;
    const auto& source = displays.front().remote;
    double extentX = 1;
    double extentY = 1;
    const WorkspaceMouseDisplay* target = nullptr;
    for (const auto& display : displays) {
        const auto& local = display.local;
        const auto& remote = display.remote;
        if (local.width <= 0 || local.height <= 0 || remote.width <= 0 || remote.height <= 0) {
            return std::nullopt;
        }
        extentX = std::max({extentX, std::abs(double(remote.x) - source.x),
                            std::abs(double(remote.x) + remote.width - source.x)});
        extentY = std::max({extentY, std::abs(double(remote.y) - source.y),
                            std::abs(double(remote.y) + remote.height - source.y)});
        if (x >= local.x && double(x) < double(local.x) + local.width && y >= local.y &&
            double(y) < double(local.y) + local.height) {
            if (target) return std::nullopt;
            target = &display;
        }
    }
    if (!target) return std::nullopt;
    // Reserve signed range for the entire workspace, including negative monitors.
    const auto width = std::min(32766.0, std::floor(32766.0 * source.width / extentX));
    const auto height = std::min(32766.0, std::floor(32766.0 * source.height / extentY));
    if (width < 1 || height < 1) return std::nullopt;
    const auto& local = target->local;
    const auto& remote = target->remote;
    const double scale = std::min(double(local.width) / remote.width,
                                  double(local.height) / remote.height);
    const double left = local.x + (local.width - remote.width * scale) / 2;
    const double top = local.y + (local.height - remote.height * scale) / 2;
    const double remoteX = remote.x + std::clamp((x - left) / scale, 0.0, double(remote.width - 1));
    const double remoteY = remote.y + std::clamp((y - top) / scale, 0.0, double(remote.height - 1));
    return WorkspaceMousePosition{
        static_cast<short>(std::lround((remoteX - source.x) * width / source.width)),
        static_cast<short>(std::lround((remoteY - source.y) * height / source.height)),
        // Moonlight subtracts one from the reference dimensions on the wire.
        static_cast<short>(width + 1), static_cast<short>(height + 1)};
}

}  // namespace terra
