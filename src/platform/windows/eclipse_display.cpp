/**
 * @file src/platform/windows/eclipse_display.cpp
 * @brief Windows display snapshot implementation for Eclipse APIs.
 */

#include "eclipse_display.h"

// standard includes
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numeric>

// local includes
#include "src/crypto.h"

// lib includes
#include <nlohmann/json.hpp>

#ifdef _WIN32
  // platform includes
  #include <dxgi1_6.h>
  #include <windows.h>
  #include <wrl/client.h>

  // local includes
  #include <display_device/windows/win_api_layer.h>
#endif

namespace eclipse::windows::display {
  namespace {
    /**
     * @brief Find platform details matching a device ID.
     *
     * @param details Detail records to search.
     * @param device_id Stable device ID.
     * @return Matching record, or null when absent.
     */
    const PlatformDetails *find_details(const std::vector<PlatformDetails> &details, const std::string_view device_id) {
      const auto found = std::ranges::find(details, device_id, &PlatformDetails::device_id);
      return found == details.end() ? nullptr : &*found;
    }
  }  // namespace

  std::optional<Rational> make_rational(const std::uint64_t numerator, const std::uint64_t denominator) {
    if (denominator == 0) {
      return std::nullopt;
    }
    const auto divisor = std::gcd(numerator, denominator);
    const auto reduced_numerator = numerator / divisor;
    const auto reduced_denominator = denominator / divisor;
    if (reduced_numerator > std::numeric_limits<std::uint32_t>::max() || reduced_denominator > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    return Rational {static_cast<std::uint32_t>(reduced_numerator), static_cast<std::uint32_t>(reduced_denominator)};
  }

  std::optional<Rational> scale_rational(const display_device::FloatingPoint &scale) {
    if (const auto *rational = std::get_if<display_device::Rational>(&scale)) {
      return make_rational(rational->m_numerator, rational->m_denominator);
    }

    const double value = std::get<double>(scale);
    if (!std::isfinite(value) || value < 0.0) {
      return std::nullopt;
    }
    if (value == 0.0) {
      return Rational {0, 1};
    }

    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
    const auto stored_exponent = static_cast<unsigned int>((bits >> 52U) & 0x7ffU);
    if (stored_exponent == 0) {
      return std::nullopt;
    }
    const auto exponent = static_cast<int>(stored_exponent) - 1023;
    std::uint64_t significand = (bits & ((1ULL << 52U) - 1U)) | (1ULL << 52U);
    int shift = exponent - 52;
    while (shift < 0 && (significand & 1U) == 0U) {
      significand >>= 1U;
      ++shift;
    }
    if (shift >= 0) {
      if (shift >= 64 || significand > (std::uint64_t {std::numeric_limits<std::uint32_t>::max()} >> shift)) {
        return std::nullopt;
      }
      return Rational {static_cast<std::uint32_t>(significand << shift), 1};
    }
    if (-shift >= 32) {
      return std::nullopt;
    }
    return make_rational(significand, 1ULL << -shift);
  }

  std::optional<Size> logical_size(const Size physical, const Rational scale) {
    if (scale.numerator == 0 || scale.denominator == 0) {
      return std::nullopt;
    }
    return Size {
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(physical.width) * scale.denominator) / scale.numerator),
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(physical.height) * scale.denominator) / scale.numerator)
    };
  }

  std::string stable_mode_id(const Size size, const Rational refresh, const std::uint32_t bit_depth, const bool hdr) {
    return std::to_string(size.width) + "x" + std::to_string(size.height) + "@" + std::to_string(refresh.numerator) + "/" + std::to_string(refresh.denominator) + ":" + std::to_string(bit_depth) + ":" + (hdr ? "hdr" : "sdr");
  }

  std::string display_resource_uuid(const std::string_view host_uuid, const std::string_view device_id) {
    std::string key {host_uuid};
    key.push_back('\0');
    key.append(device_id);
    const auto digest = crypto::hash(key);
    std::array<unsigned char, 16> bytes {};
    std::ranges::copy_n(digest.begin(), bytes.size(), bytes.begin());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x80U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    char result[37] {};
    std::snprintf(result, sizeof(result), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    return result;
  }

  Kind kind_from_output_technology(const std::uint32_t technology) {
#ifdef _WIN32
    switch (static_cast<DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY>(technology)) {
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
        return Kind::Internal;
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:
        return Kind::Virtual;
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:
      case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
        return Kind::External;
      default:
        break;
    }
#else
    static_cast<void>(technology);
#endif
    return Kind::Unknown;
  }

  std::vector<Snapshot> make_snapshot(const std::string_view host_uuid, const display_device::EnumeratedDeviceList &devices, const std::vector<PlatformDetails> &details) {
    std::vector<Snapshot> result;
    result.reserve(devices.size());
    for (const auto &device : devices) {
      const auto *platform = find_details(details, device.m_device_id);
      Snapshot snapshot {
        .resource_uuid = display_resource_uuid(host_uuid, device.m_device_id),
        .device_id = device.m_device_id,
        .platform_id = device.m_display_name,
        .name = device.m_friendly_name.empty() ? device.m_display_name : device.m_friendly_name,
        .enabled = device.m_info.has_value(),
        .capture_eligible = false
      };
      if (platform) {
        snapshot.enabled = platform->enabled;
        snapshot.capture_eligible = platform->capture_eligible;
        snapshot.kind = platform->kind;
        snapshot.hdr_supported = platform->hdr_supported;
        snapshot.supported_modes = platform->supported_modes;
      }
      if (device.m_info) {
        const auto &info = *device.m_info;
        snapshot.primary = info.m_primary;
        snapshot.position = {info.m_origin_point.m_x, info.m_origin_point.m_y};
        snapshot.scale = scale_rational(info.m_resolution_scale).value_or(Rational {1, 1});
        const Size physical {info.m_resolution.m_width, info.m_resolution.m_height};
        snapshot.logical_size = logical_size(physical, snapshot.scale).value_or(physical);
        snapshot.hdr_enabled = info.m_hdr_state.transform([](const display_device::HdrState state) {
          return state == display_device::HdrState::Enabled;
        });
        const auto refresh = scale_rational(info.m_refresh_rate);
        if (refresh) {
          const auto bit_depth = platform && platform->current_bit_depth ? *platform->current_bit_depth : 0U;
          const bool hdr = snapshot.hdr_enabled.value_or(false);
          snapshot.current_mode = Mode {stable_mode_id(physical, *refresh, bit_depth, hdr), physical, *refresh, bit_depth, hdr};
        }
      }
      result.push_back(std::move(snapshot));
    }
    return result;
  }

  std::vector<Snapshot> enumerate_snapshot(const std::string_view host_uuid, const display_device::EnumeratedDeviceList &devices) {
    std::vector<PlatformDetails> details;
#ifdef _WIN32
    display_device::WinApiLayer windows_api;
    if (const auto display_config = windows_api.queryDisplayConfig(display_device::QueryType::All)) {
      for (const auto &path : display_config->m_paths) {
        const auto device_id = windows_api.getDeviceId(path);
        if (device_id.empty()) {
          continue;
        }
        const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        const auto kind = kind_from_output_technology(path.targetInfo.outputTechnology);
        const auto existing = std::ranges::find(details, device_id, &PlatformDetails::device_id);
        if (existing == details.end()) {
          details.push_back({.device_id = device_id, .enabled = active, .kind = kind});
        } else {
          existing->enabled = existing->enabled || active;
          if (existing->kind == Kind::Unknown || active) {
            existing->kind = kind;
          }
        }
      }
    }

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
      for (UINT adapter_index = 0;; ++adapter_index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) {
          break;
        }
        for (UINT output_index = 0;; ++output_index) {
          Microsoft::WRL::ComPtr<IDXGIOutput> output;
          if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) {
            break;
          }
          DXGI_OUTPUT_DESC description {};
          if (FAILED(output->GetDesc(&description)) || !description.AttachedToDesktop) {
            continue;
          }
          std::string platform_id;
          for (const wchar_t *character = description.DeviceName; *character; ++character) {
            platform_id.push_back(*character <= 0x7f ? static_cast<char>(*character) : '?');
          }
          const auto device = std::ranges::find(devices, platform_id, &display_device::EnumeratedDevice::m_display_name);
          if (device == devices.end()) {
            continue;
          }
          auto platform = std::ranges::find(details, device->m_device_id, &PlatformDetails::device_id);
          if (platform == details.end()) {
            platform = details.emplace(details.end(), PlatformDetails {.device_id = device->m_device_id, .enabled = true});
          }
          auto &item = *platform;
          item.enabled = true;
          item.capture_eligible = true;
          Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
          if (SUCCEEDED(output.As(&output6))) {
            DXGI_OUTPUT_DESC1 description1 {};
            if (SUCCEEDED(output6->GetDesc1(&description1))) {
              item.current_bit_depth = description1.BitsPerColor;
              item.hdr_supported = device->m_info && device->m_info->m_hdr_state.has_value();
            }
          }
          for (const auto format : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
            UINT count = 0;
            if (FAILED(output->GetDisplayModeList(format, 0, &count, nullptr)) || count == 0) {
              continue;
            }
            std::vector<DXGI_MODE_DESC> modes(count);
            if (FAILED(output->GetDisplayModeList(format, 0, &count, modes.data()))) {
              continue;
            }
            for (const auto &mode : modes) {
              const auto refresh = make_rational(mode.RefreshRate.Numerator, mode.RefreshRate.Denominator);
              if (!refresh) {
                continue;
              }
              const Size size {mode.Width, mode.Height};
              const std::uint32_t bit_depth = format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10U : 8U;
              const bool hdr = bit_depth == 10 && item.hdr_supported.value_or(false);
              item.supported_modes.push_back({stable_mode_id(size, *refresh, bit_depth, hdr), size, *refresh, bit_depth, hdr});
            }
          }
          std::ranges::sort(item.supported_modes, {}, &Mode::id);
          item.supported_modes.erase(std::ranges::unique(item.supported_modes).begin(), item.supported_modes.end());
        }
      }
    }
#endif
    return make_snapshot(host_uuid, devices, details);
  }

  std::string_view kind_name(const Kind kind) {
    switch (kind) {
      case Kind::Internal:
        return "internal";
      case Kind::External:
        return "external";
      case Kind::Virtual:
        return "virtual";
      case Kind::Unknown:
        return "unknown";
    }
    return "unknown";
  }

  nlohmann::json to_json(const Mode &mode) {
    return {
      {"id", mode.id},
      {"width", mode.size.width},
      {"height", mode.size.height},
      {"refreshNumerator", mode.refresh.numerator},
      {"refreshDenominator", mode.refresh.denominator},
      {"bitDepth", mode.bit_depth},
      {"hdr", mode.hdr},
    };
  }

  nlohmann::json to_json(const Snapshot &snapshot) {
    nlohmann::json modes = nlohmann::json::array();
    for (const auto &mode : snapshot.supported_modes) {
      modes.push_back(to_json(mode));
    }
    return {
      {"id", snapshot.resource_uuid},
      {"platformId", snapshot.platform_id},
      {"name", snapshot.name},
      {"kind", kind_name(snapshot.kind)},
      {"connected", true},
      {"enabled", snapshot.enabled},
      {"primary", snapshot.primary},
      {"position", {{"x", snapshot.position.x}, {"y", snapshot.position.y}}},
      {"logicalSize", {{"width", snapshot.logical_size.width}, {"height", snapshot.logical_size.height}}},
      {"scale", {{"numerator", snapshot.scale.numerator}, {"denominator", snapshot.scale.denominator}}},
      {"rotation", 0},  ///< Windows desktop coordinates are post-rotation; no distinct rotation state is exposed.
      {"currentMode", snapshot.current_mode ? to_json(*snapshot.current_mode) : nlohmann::json(nullptr)},
      {"supportedModes", std::move(modes)},
      {"hdrSupported", snapshot.hdr_supported ? nlohmann::json(*snapshot.hdr_supported) : nlohmann::json(nullptr)},
      {"hdrEnabled", snapshot.hdr_enabled ? nlohmann::json(*snapshot.hdr_enabled) : nlohmann::json(nullptr)},
      {"hdr", {
                {"supported", snapshot.hdr_supported ? nlohmann::json(*snapshot.hdr_supported) : nlohmann::json(nullptr)},
                {"enabled", snapshot.hdr_enabled ? nlohmann::json(*snapshot.hdr_enabled) : nlohmann::json(nullptr)},
              }},
      {"captureEligible", snapshot.capture_eligible},
      {"ownerClientUuid", nullptr},
      {"workspaceId", nullptr},
      {"sessionId", nullptr},
    };
  }
}  // namespace eclipse::windows::display
