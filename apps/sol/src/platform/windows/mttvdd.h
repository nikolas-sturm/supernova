/**
 * @file src/platform/windows/mttvdd.h
 * @brief Client for MikeTheTech Virtual Display Driver named-pipe control.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * @brief MikeTheTech Virtual Display Driver control operations.
 */
namespace mttvdd {
  constexpr std::uint32_t MAX_DISPLAY_COUNT = 16;  ///< Maximum display count exposed by the vendored driver adapter.

  /**
   * @brief Result of probing the installed MttVDD control protocol.
   */
  struct status_t {
    bool available;  ///< Whether a compatible control pipe answered the probe.
    std::string reason_code;  ///< Stable reason when the provider is unavailable.
    std::string reason;  ///< Human-readable provider status.
  };

  /**
   * @brief Build the MttVDD command that replaces the global virtual-display count.
   *
   * @param count Requested display count.
   * @return UTF-16 command, or no value when count exceeds provider policy.
   */
  std::optional<std::wstring> display_count_command(std::uint32_t count);

  /**
   * @brief Validate a decoded response to the MttVDD `GETSETTINGS` command.
   *
   * @param response UTF-16 response, optionally including a trailing null character.
   * @return `true` when response identifies compatible settings protocol.
   */
  bool is_settings_response(std::wstring_view response);

  /**
   * @brief Parse configured monitor count from MttVDD settings XML.
   *
   * @param xml Complete `vdd_settings.xml` contents.
   * @return Valid count, or no value for malformed or out-of-policy settings.
   */
  std::optional<std::uint32_t> parse_display_count(std::string_view xml);

  /**
   * @brief Read current global monitor count from MttVDD settings.
   *
   * @return Configured count, or no value when settings cannot be read safely.
   */
  std::optional<std::uint32_t> configured_display_count();

  /**
   * @brief Identify an MttVDD monitor hardware or device instance ID.
   *
   * @param id Windows monitor hardware or device instance ID.
   * @return `true` when ID contains upstream MttVDD model identifier.
   */
  bool is_monitor_id(std::wstring_view id);

  /**
   * @brief Reduce an MttVDD device or interface ID to its restart-stable connector UID.
   *
   * @param id PnP device instance ID, canonical connector ID, or monitor interface path.
   * @return Canonical connector ID, or no value when model and UID cannot be identified.
   */
  std::optional<std::string> canonical_monitor_id(std::string_view id);

  /**
   * @brief Match PnP instance ID against Windows monitor interface path.
   *
   * Windows represents separators differently between these two identifiers, and
   * reinstalling the root adapter changes the transient PnP instance component.
   *
   * @param instance_id Monitor PnP instance or canonical connector ID.
   * @param interface_path Monitor interface path.
   * @return `true` when both IDs identify the same MttVDD connector UID.
   */
  bool monitor_id_matches_path(std::string_view instance_id, std::string_view interface_path);

  /**
   * @brief Enumerate present MttVDD monitor device instances.
   *
   * Other indirect-display providers are excluded by MttVDD hardware ID.
   *
   * @return Sorted canonical connector IDs, or no value on enumeration failure.
   */
  std::optional<std::vector<std::string>> display_inventory();

  /**
   * @brief Probe installed MttVDD without changing display state.
   *
   * Older deployed builds do not implement `PING`, so this uses the read-only
   * `GETSETTINGS` command supported by both old and current pipe protocols.
   *
   * @return Provider availability and failure reason.
   */
  status_t probe();

  /**
   * @brief Replace MttVDD global virtual-display count.
   *
   * @param count Desired count from zero through `MAX_DISPLAY_COUNT`.
   * @return `true` after the complete command was delivered to the driver pipe.
   */
  bool set_display_count(std::uint32_t count);

  /**
   * @brief Replace display count and wait for settings and PnP inventory convergence.
   *
   * @param count Desired count from zero through `MAX_DISPLAY_COUNT`.
   * @return `true` when both configured count and present monitor inventory match.
   */
  bool set_display_count_and_wait(std::uint32_t count);
}  // namespace mttvdd
