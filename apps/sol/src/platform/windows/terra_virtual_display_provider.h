/**
 * @file src/platform/windows/terra_virtual_display_provider.h
 * @brief Windows provider callbacks for Terra MttVDD lifecycle management.
 */
#pragma once

// standard includes
#include <filesystem>

// local includes
#include "src/terra_virtual_display.h"

namespace terra::windows::virtual_display {
  /**
   * @brief Probe complete read-only MttVDD and display-correlation path.
   *
   * @return `true` when provider, inventory, and every connector mapping are usable.
   */
  [[nodiscard]] bool available();

  /**
   * @brief Build production MttVDD and libdisplaydevice callbacks.
   *
   * @param persistence_path Atomic lifecycle persistence document path.
   * @return Callbacks suitable for `terra_virtual_display::manager_t`.
   */
  [[nodiscard]] terra_virtual_display::callbacks_t make_callbacks(const std::filesystem::path &persistence_path);
}  // namespace terra::windows::virtual_display
