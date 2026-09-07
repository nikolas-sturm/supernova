/**
 * @file src/platform/windows/terra_display.h
 * @brief Windows display snapshot declarations for Terra APIs.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <display_device/types.h>
#include <nlohmann/json_fwd.hpp>

namespace terra::windows::display {
  /**
   * @brief Exact unsigned rational value.
   */
  struct Rational {
    std::uint32_t numerator {};  ///< Rational numerator.
    std::uint32_t denominator {};  ///< Rational denominator.

    /**
     * @brief Compare rational storage exactly.
     */
    friend bool operator==(const Rational &, const Rational &) = default;
  };

  /**
   * @brief Integer two-dimensional size.
   */
  struct Size {
    std::uint32_t width {};  ///< Width in pixels or logical units.
    std::uint32_t height {};  ///< Height in pixels or logical units.

    /**
     * @brief Compare sizes exactly.
     */
    friend bool operator==(const Size &, const Size &) = default;
  };

  /**
   * @brief Integer desktop position.
   */
  struct Position {
    std::int32_t x {};  ///< Horizontal desktop coordinate.
    std::int32_t y {};  ///< Vertical desktop coordinate.

    /**
     * @brief Compare positions exactly.
     */
    friend bool operator==(const Position &, const Position &) = default;
  };

  /**
   * @brief Display classification reported by Windows.
   */
  enum class Kind {
    Unknown,  ///< Windows supplied no reliable classification.
    Internal,  ///< Integrated panel.
    External,  ///< Physically connected external output.
    Virtual  ///< Indirect or virtual output.
  };

  /**
   * @brief Exact display mode exposed to Terra serialization.
   */
  struct Mode {
    std::string id {};  ///< Deterministic mode identifier.
    Size size {};  ///< Physical pixel dimensions.
    Rational refresh {};  ///< Exact refresh-rate rational.
    std::uint32_t bit_depth {};  ///< Bits per color channel, or zero when unavailable.
    bool hdr {};  ///< Whether mode is HDR-capable.

    /**
     * @brief Compare modes exactly.
     */
    friend bool operator==(const Mode &, const Mode &) = default;
  };

  /**
   * @brief Windows details collected for one stable libdisplaydevice ID.
   */
  struct PlatformDetails {
    std::string device_id {};  ///< Stable libdisplaydevice ID used for joining.
    bool enabled {};  ///< Whether output participates in desktop topology.
    bool capture_eligible {};  ///< Whether Sol can resolve active DXGI output.
    Kind kind {Kind::Unknown};  ///< OS-derived output classification.
    std::optional<bool> hdr_supported {};  ///< HDR capability, when discoverable.
    std::optional<std::uint32_t> current_bit_depth {};  ///< Active bits per color channel.
    std::vector<Mode> supported_modes {};  ///< Exact modes reported by DXGI.
  };

  /**
   * @brief Contract-ready read model for one display.
   */
  struct Snapshot {
    std::string resource_uuid {};  ///< Host-scoped deterministic display UUID.
    std::string device_id {};  ///< Stable libdisplaydevice device ID.
    std::string platform_id {};  ///< Current platform display identifier.
    std::string name {};  ///< Current user-facing display name.
    bool enabled {};  ///< Whether display participates in desktop topology.
    bool primary {};  ///< Whether display is primary.
    Position position {};  ///< Desktop origin for active display.
    Size logical_size {};  ///< Desktop size after scaling.
    Rational scale {1, 1};  ///< Exact physical-to-logical scale where representable.
    std::optional<Mode> current_mode {};  ///< Current mode for active display.
    std::vector<Mode> supported_modes {};  ///< Exact supported modes.
    std::optional<bool> hdr_supported {};  ///< HDR capability when known.
    std::optional<bool> hdr_enabled {};  ///< Current HDR state when known.
    bool capture_eligible {};  ///< Whether Sol can capture output.
    Kind kind {Kind::Unknown};  ///< OS-derived output classification.
  };

  /**
   * @brief Reduce an unsigned rational to canonical form.
   *
   * @param numerator Numerator to reduce.
   * @param denominator Nonzero denominator to reduce.
   * @return Reduced rational, or empty when denominator is zero.
   */
  [[nodiscard]] std::optional<Rational> make_rational(std::uint64_t numerator, std::uint64_t denominator);

  /**
   * @brief Convert libdisplaydevice scale storage without decimal rounding.
   *
   * @param scale Scale value to convert.
   * @return Exact rational when representable with 32-bit components.
   */
  [[nodiscard]] std::optional<Rational> scale_rational(const display_device::FloatingPoint &scale);

  /**
   * @brief Convert physical dimensions to logical dimensions.
   *
   * @param physical Physical pixel dimensions.
   * @param scale Physical-to-logical scale.
   * @return Logical dimensions truncated to complete logical units, or empty for invalid input.
   */
  [[nodiscard]] std::optional<Size> logical_size(Size physical, Rational scale);

  /**
   * @brief Build deterministic identifier for an exact mode.
   *
   * @param size Mode dimensions.
   * @param refresh Exact refresh rate.
   * @param bit_depth Bits per color channel, or zero when unknown.
   * @param hdr Whether mode is HDR-capable.
   * @return Stable mode identifier.
   */
  [[nodiscard]] std::string stable_mode_id(Size size, Rational refresh, std::uint32_t bit_depth, bool hdr);

  /**
   * @brief Derive host-scoped deterministic display resource UUID.
   *
   * @param host_uuid Stable Sol host UUID.
   * @param device_id Stable libdisplaydevice device ID.
   * @return Canonical lowercase UUID derived from both inputs.
   */
  [[nodiscard]] std::string display_resource_uuid(std::string_view host_uuid, std::string_view device_id);

  /**
   * @brief Classify Windows display output technology without name heuristics.
   *
   * @param technology Numeric `DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY` value.
   * @return OS-derived display kind, or `Unknown` for unclassified technology.
   */
  [[nodiscard]] Kind kind_from_output_technology(std::uint32_t technology);

  /**
   * @brief Transform enumerated devices and platform details into snapshots.
   *
   * @param host_uuid Stable Sol host UUID.
   * @param devices Existing libdisplaydevice enumeration.
   * @param details Windows details joined by stable device ID.
   * @return Snapshots preserving input device order.
   */
  [[nodiscard]] std::vector<Snapshot> make_snapshot(std::string_view host_uuid, const display_device::EnumeratedDeviceList &devices, const std::vector<PlatformDetails> &details);

  /**
   * @brief Collect Windows details and create contract-ready display snapshots.
   *
   * @param host_uuid Stable Sol host UUID.
   * @param devices Existing libdisplaydevice enumeration.
   * @return Enriched snapshots preserving input device order.
   */
  [[nodiscard]] std::vector<Snapshot> enumerate_snapshot(std::string_view host_uuid, const display_device::EnumeratedDeviceList &devices);

  /**
   * @brief Return stable API text for display kind.
   *
   * @param kind Display kind.
   * @return Lowercase contract value.
   */
  [[nodiscard]] std::string_view kind_name(Kind kind);

  /**
   * @brief Serialize exact display mode.
   *
   * @param mode Display mode.
   * @return Terra display mode object.
   */
  [[nodiscard]] nlohmann::json to_json(const Mode &mode);

  /**
   * @brief Serialize physical display snapshot.
   *
   * Internal libdisplaydevice identifier is intentionally omitted.
   *
   * @param snapshot Physical display snapshot.
   * @return Terra physical display object.
   */
  [[nodiscard]] nlohmann::json to_json(const Snapshot &snapshot);
}  // namespace terra::windows::display
