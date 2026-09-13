/**
 * @file src/input_workspace.h
 * @brief Coordinate validation for negotiated workspace-wide mouse input.
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace input {
  /** @brief Host-authorized workspace display in desktop pixels. */
  struct mouse_viewport_t {
    int x {};  ///< Desktop X coordinate.
    int y {};  ///< Desktop Y coordinate.
    int width {};  ///< Display pixel width.
    int height {};  ///< Display pixel height.
    std::uint64_t generation {};  ///< Host topology generation captured during authorization.
  };

  /**
   * @brief Validate signed source-relative mouse input against attached workspace displays.
   * @param displays Authorized displays, with the stream's source display first.
   * @param x Signed packet X coordinate.
   * @param y Signed packet Y coordinate.
   * @param width Positive packet reference width.
   * @param height Positive packet reference height.
   * @return Source-relative pixel coordinates, or no value for gaps and invalid packets.
   */
  inline std::optional<std::pair<float, float>> workspace_mouse_position(
    std::span<const mouse_viewport_t> displays, float x, float y, float width, float height
  ) {
    if (displays.size() < 2 || displays.size() > 4 || width <= 0 || height <= 0 ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height)) {
      return std::nullopt;
    }
    const auto &source = displays.front();
    if (source.width <= 0 || source.height <= 0) {
      return std::nullopt;
    }
    const double relative_x = std::round(double(x) * source.width / width);
    const double relative_y = std::round(double(y) * source.height / height);
    const double desktop_x = source.x + relative_x;
    const double desktop_y = source.y + relative_y;
    for (const auto &display : displays) {
      if (display.width > 0 && display.height > 0 && desktop_x >= display.x && desktop_y >= display.y &&
          desktop_x < double(display.x) + display.width && desktop_y < double(display.y) + display.height) {
        return std::pair {static_cast<float>(relative_x), static_cast<float>(relative_y)};
      }
    }
    return std::nullopt;
  }
}  // namespace input
