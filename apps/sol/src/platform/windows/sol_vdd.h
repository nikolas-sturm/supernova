/**
 * @file src/platform/windows/sol_vdd.h
 * @brief Client for the SolVDD versioned binary topology control protocol.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// local includes
#include "third-party/solvdd/Common/Include/SolVddProtocol.h"

/**
 * @brief SolVDD topology control operations.
 */
namespace sol_vdd {
  constexpr std::uint32_t MAX_DISPLAY_COUNT = sol_vdd::MAX_CONNECTORS;  ///< Maximum connector slots exposed by the vendored driver adapter.

  /**
   * @brief Result of probing the installed SolVDD control protocol.
   */
  struct probe_status_t {
    bool available;  ///< Whether a compatible control pipe answered the probe.
    std::string reason_code;  ///< Stable reason when the provider is unavailable.
    std::string reason;  ///< Human-readable provider status.
  };

  /**
   * @brief Requested or committed state of one connector slot.
   */
  struct connector_t {
    std::uint32_t slot;  ///< Stable connector slot.
    std::uint32_t width;  ///< Active pixel width.
    std::uint32_t height;  ///< Active pixel height.
    std::uint32_t refresh_numerator;  ///< Vertical refresh numerator.
    std::uint32_t refresh_denominator;  ///< Vertical refresh denominator.
    std::uint32_t bit_depth;  ///< Bits per color component; 8 or 10.
    bool hdr;  ///< Whether HDR output is requested.
  };

  /**
   * @brief Present driver connector correlated to a stable slot.
   */
  struct display_connector_t {
    std::uint32_t slot;  ///< Stable connector slot.
    std::string id;  ///< Canonical SolVDD monitor connector ID.
  };

  /**
   * @brief Decoded control-pipe response.
   */
  struct response_t {
    sol_vdd::status_t status;  ///< Driver status code.
    std::vector<connector_t> connectors;  ///< Committed connector state.
  };

  /**
   * @brief Build a topology query request message.
   *
   * @param request_id Caller-selected correlation identifier.
   * @return Serialized request bytes.
   */
  std::vector<std::byte> encode_query_request(std::uint32_t request_id);

  /**
   * @brief Build a complete topology replacement request message.
   *
   * @param request_id Caller-selected correlation identifier.
   * @param topology Complete sparse desired topology.
   * @return Serialized request bytes.
   */
  std::vector<std::byte> encode_apply_request(std::uint32_t request_id, const std::vector<connector_t> &topology);

  /**
   * @brief Validate and decode a control-pipe response message.
   *
   * @param response Raw response bytes.
   * @param expected_request_id Request identifier that must be echoed.
   * @return Decoded response, or no value for malformed or mismatched messages.
   */
  std::optional<response_t> decode_response(std::span<const std::byte> response, std::uint32_t expected_request_id);

  /**
   * @brief Parse the connector slot encoded in an SolVDD EDID serial number.
   *
   * @param edid EDID bytes.
   * @return Connector slot, or no value when the EDID is not slot encoded.
   */
  std::optional<std::uint32_t> monitor_slot_from_edid(std::span<const std::byte> edid);

  /**
   * @brief Validate one topology record against protocol limits.
   *
   * @param connector Connector record.
   * @return True when every field is in range.
   */
  bool valid_connector(const connector_t &connector);

  /**
   * @brief Identify an SolVDD monitor hardware or device instance ID.
   *
   * @param id Windows monitor hardware or device instance ID.
   * @return `true` when ID contains upstream SolVDD model identifier.
   */
  bool is_monitor_id(std::wstring_view id);

  /**
   * @brief Reduce an SolVDD device or interface ID to its restart-stable connector ID.
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
   * @return `true` when both IDs identify the same SolVDD connector UID.
   */
  bool monitor_id_matches_path(std::string_view instance_id, std::string_view interface_path);

  /**
   * @brief Enumerate present SolVDD monitor device instances with EDID-derived slots.
   *
   * Other indirect-display providers are excluded by SolVDD hardware ID. Devices whose
   * EDID serial number does not encode a connector slot fail enumeration, because the
   * installed driver does not implement the Sol topology protocol.
   *
   * @return Connectors sorted by slot, or no value on enumeration failure.
   */
  std::optional<std::vector<display_connector_t>> display_inventory();

  /**
   * @brief Probe the installed SolVDD protocol without changing display state.
   *
   * @return Provider availability and failure reason.
   */
  probe_status_t probe();

  /**
   * @brief Read the driver's committed sparse topology.
   *
   * @return Committed connectors, or no value when the protocol is unavailable.
   */
  std::optional<std::vector<connector_t>> query_topology();

  /**
   * @brief Atomically replace the complete topology and await PnP inventory convergence.
   *
   * @param topology Desired sparse topology from zero through `MAX_DISPLAY_COUNT` connectors.
   * @return `true` when the driver committed the topology and every slot is present.
   */
  bool apply_topology_and_wait(const std::vector<connector_t> &topology);
}  // namespace sol_vdd
