#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <vector>

namespace terra {

/** @brief Explain why launch-time workspace routing is unavailable; null means enabled. */
inline const char* workspaceMouseBlocker(bool fullscreen, std::size_t streams,
                                       std::size_t localOutputs, bool layoutEligible,
                                       bool hostSupported) {
    if (streams < 2 || streams > 4) return "Workspace mouse requires two to four streams.";
    if (!fullscreen) {
        return "Stream display mode is Windowed; select Fullscreen or Borderless in Workstation Settings before launching. Toggling a running window does not enable workspace routing.";
    }
    if (streams > localOutputs) return "More workspace streams than detected local outputs.";
    if (!layoutEligible) return "Host display scaling or rotation is not eligible for workspace mouse routing.";
    if (!hostSupported) return "Host does not advertise workspace-mouse-v1; update or reconnect to the intended Sol host.";
    return nullptr;
}

/** @brief Pixel rectangle in a local or remote desktop. */
struct MouseRectangle {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const MouseRectangle&) const = default;
};

/** @brief Resolve an assigned output across independently enumerated worker processes. */
inline std::optional<int> workspaceDisplayIndex(std::span<const MouseRectangle> outputs,
                                              const MouseRectangle& assigned) {
    if (assigned.width <= 0 || assigned.height <= 0) return std::nullopt;
    std::optional<int> result;
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        if (outputs[index] != assigned) continue;
        if (result) return std::nullopt;
        result = static_cast<int>(index);
    }
    return result;
}

/** @brief Require a fullscreen surface to match the output used by its mouse map. */
inline bool workspaceWindowMatches(const MouseRectangle& assigned, const MouseRectangle& actual,
                                   int width, int height) {
    return assigned.width > 0 && assigned.height > 0 && assigned == actual &&
           width == assigned.width && height == assigned.height;
}

/**
 * @brief Order local output indices by desktop position starting at the
 * selected output, wrapping cyclically. Workspace streams must claim
 * physically adjacent outputs; enumeration order may interleave unrelated
 * panels such as an embedded display between two external monitors.
 */
inline std::vector<std::size_t> workspaceOutputOrder(std::span<const MouseRectangle> outputs,
                                                     std::size_t selected) {
    std::vector<std::size_t> order(outputs.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::sort(order, [&](std::size_t left, std::size_t right) {
        return std::tie(outputs[left].x, outputs[left].y) <
               std::tie(outputs[right].x, outputs[right].y);
    });
    const auto start = std::ranges::find(order, selected);
    if (start != order.end()) std::ranges::rotate(order, start);
    return order;
}

/**
 * @brief Pair each mirrored workspace stream with the local output at the
 * same desktop origin. Topology specs carry the client origin of every
 * streamed output, so disabled outputs are never claimed. Returns nullopt
 * unless every mirror matches exactly one unused local output.
 */
inline std::optional<std::vector<std::size_t>> workspacePairByOrigin(
    std::span<const MouseRectangle> mirrors, std::span<const MouseRectangle> outputs) {
    if (mirrors.empty() || mirrors.size() > outputs.size()) return std::nullopt;
    std::vector<std::size_t> result;
    result.reserve(mirrors.size());
    std::vector<char> used(outputs.size(), 0);
    for (const auto& mirror : mirrors) {
        std::optional<std::size_t> match;
        for (std::size_t index = 0; index < outputs.size(); ++index) {
            if (used[index] || outputs[index].x != mirror.x || outputs[index].y != mirror.y) {
                continue;
            }
            if (match) return std::nullopt;
            match = index;
        }
        if (!match) return std::nullopt;
        used[*match] = 1;
        result.push_back(*match);
    }
    return result;
}

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
