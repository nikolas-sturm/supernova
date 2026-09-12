/**
 * @file src/platform/windows/terra_virtual_display_provider.h
 * @brief Windows provider callbacks for Terra MttVDD lifecycle management.
 */
#pragma once

// standard includes
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "src/terra_virtual_display.h"

namespace terra::windows::virtual_display {
  /**
   * @brief Migrate persisted transient MttVDD PnP IDs to stable connector IDs.
   *
   * @param document Version-one virtual-display persistence document.
   * @return Migrated document, or no value when its provider IDs are invalid.
   */
  [[nodiscard]] std::optional<std::string> canonicalize_persistence_ids(std::string_view document);

  /**
   * @brief Resolve one MttVDD connector to its libdisplaydevice identifier.
   *
   * @param platform_id Canonical MttVDD monitor connector ID.
   * @return Stable libdisplaydevice ID, or no value when correlation is unavailable.
   */
  [[nodiscard]] std::optional<std::string> resolve_device_id(std::string_view platform_id);

  /**
   * @brief Probe complete read-only MttVDD and display-correlation path.
   *
   * @return `true` when provider, inventory, and every connector mapping are usable.
   */
  [[nodiscard]] bool available();

  /**
   * @brief Activate only requested managed virtual displays and durably retain host topology.
   *
   * @param displays Managed displays and exact requested configurations to keep active.
   * @return True after exact virtual-only topology is verified.
   */
  [[nodiscard]] bool activate_exclusive(const std::vector<terra_virtual_display::resource_t> &displays);

  /**
   * @brief Restore exact topology retained before exclusive Terra streaming.
   *
   * @return True when restoration completed or no rollback journal exists.
   */
  [[nodiscard]] bool restore_exclusive();

  /**
   * @brief Build production MttVDD and libdisplaydevice callbacks.
   *
   * @param persistence_path Atomic lifecycle persistence document path.
   * @return Callbacks suitable for `terra_virtual_display::manager_t`.
   */
  [[nodiscard]] terra_virtual_display::callbacks_t make_callbacks(const std::filesystem::path &persistence_path);
}  // namespace terra::windows::virtual_display
