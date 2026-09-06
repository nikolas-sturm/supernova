/**
 * @file src/platform/windows/eclipse_virtual_display_provider.cpp
 * @brief Windows provider callbacks for Eclipse MttVDD lifecycle management.
 */

// header include
#include "eclipse_virtual_display_provider.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <thread>

// system includes
#include <windows.h>

// lib includes
#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>

// local includes
#include "eclipse_display.h"
#include "mttvdd.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/uuid.h"

namespace eclipse::windows::virtual_display {
  namespace {
    using configuration_t = eclipse_virtual_display::platform_configuration_t;
    constexpr std::chrono::seconds DISPLAY_CONFIG_TIMEOUT {20};  ///< Maximum DisplayConfig convergence wait.
    constexpr std::chrono::milliseconds DISPLAY_CONFIG_POLL_INTERVAL {250};  ///< DisplayConfig polling interval.

    /**
     * @brief Find libdisplaydevice ID for one MttVDD PnP instance.
     *
     * @param windows_api Windows display API layer.
     * @param platform_id MttVDD monitor PnP instance ID.
     * @return Stable libdisplaydevice ID, or no value when correlation is ambiguous.
     */
    std::optional<std::string> device_id(display_device::WinApiLayer &windows_api, const std::string_view platform_id) {
      const auto config = windows_api.queryDisplayConfig(display_device::QueryType::All);
      if (!config) {
        return std::nullopt;
      }
      std::optional<std::string> result;
      for (const auto &path : config->m_paths) {
        const auto monitor_path = windows_api.getMonitorDevicePath(path);
        if (!mttvdd::monitor_id_matches_path(platform_id, monitor_path)) {
          continue;
        }
        const auto id = windows_api.getDeviceId(path);
        if (id.empty() || result) {
          return std::nullopt;
        }
        result = id;
      }
      return result;
    }

    /**
     * @brief Wait for DisplayConfig to expose one newly provisioned PnP monitor.
     *
     * @param windows_api Windows display API layer.
     * @param platform_id MttVDD monitor PnP instance ID.
     * @return Stable libdisplaydevice ID, or no value after timeout.
     */
    std::optional<std::string> wait_device_id(display_device::WinApiLayer &windows_api, const std::string_view platform_id) {
      const auto deadline = std::chrono::steady_clock::now() + DISPLAY_CONFIG_TIMEOUT;
      do {
        if (auto id = device_id(windows_api, platform_id)) {
          return id;
        }
        std::this_thread::sleep_for(DISPLAY_CONFIG_POLL_INTERVAL);
      } while (std::chrono::steady_clock::now() < deadline);
      return std::nullopt;
    }

    /**
     * @brief Convert requested rotation to Win32 display orientation.
     *
     * @param rotation Clockwise rotation in degrees.
     * @return Win32 orientation value.
     */
    DWORD orientation(const int rotation) {
      switch (rotation) {
        case 90:
          return DMDO_90;
        case 180:
          return DMDO_180;
        case 270:
          return DMDO_270;
        default:
          return DMDO_DEFAULT;
      }
    }

    /**
     * @brief Apply and verify complete managed virtual-display configuration.
     *
     * @param configurations Managed connector configurations.
     * @return `true` only after every requested property can be confirmed.
     */
    bool apply(std::vector<configuration_t> &configurations) {
      auto api_layer = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice display_api {api_layer};
      if (!display_api.isApiAccessAvailable()) {
        BOOST_LOG(error) << "Eclipse MttVDD: display configuration API unavailable";
        return false;
      }

      std::map<std::string, std::string, std::less<>> platform_to_device;
      std::set<std::string, std::less<>> requested_devices;
      std::size_t primary_count = 0;
      for (const auto &configuration : configurations) {
        const auto &spec = configuration.specification;
        if (spec.scale != 1.0 || (spec.primary && (spec.position.x != 0 || spec.position.y != 0))) {
          BOOST_LOG(error) << "Eclipse MttVDD: unsupported scale or primary position";
          return false;
        }
        primary_count += spec.primary;
        const auto id = wait_device_id(*api_layer, configuration.platform_id);
        if (!id || !requested_devices.emplace(*id).second) {
          BOOST_LOG(error) << "Eclipse MttVDD: connector correlation failed for " << configuration.platform_id;
          return false;
        }
        platform_to_device.emplace(configuration.platform_id, *id);
      }
      if (primary_count > 1) {
        BOOST_LOG(error) << "Eclipse MttVDD: multiple primary displays requested";
        return false;
      }

      auto topology = display_api.getCurrentTopology();
      for (const auto &id : requested_devices) {
        const bool active = std::ranges::any_of(topology, [&](const auto &group) {
          return std::ranges::find(group, id) != group.end();
        });
        if (!active) {
          topology.push_back({id});
        }
      }
      if (!display_api.isTopologyValid(topology) || !display_api.setTopology(topology)) {
        BOOST_LOG(error) << "Eclipse MttVDD: topology activation failed";
        return false;
      }

      display_device::DeviceDisplayModeMap modes;
      display_device::HdrStateMap hdr_states;
      for (const auto &configuration : configurations) {
        const auto &id = platform_to_device.at(configuration.platform_id);
        const auto &mode = configuration.specification.mode;
        modes[id] = {{static_cast<unsigned int>(mode.width), static_cast<unsigned int>(mode.height)}, {mode.refresh_numerator, mode.refresh_denominator}};
        hdr_states[id] = configuration.specification.hdr ? display_device::HdrState::Enabled : display_device::HdrState::Disabled;
      }
      if ((!modes.empty() && !display_api.setDisplayModes(modes)) || (!hdr_states.empty() && !display_api.setHdrStates(hdr_states))) {
        BOOST_LOG(error) << "Eclipse MttVDD: mode or HDR application failed";
        return false;
      }
      const auto primary = std::ranges::find_if(configurations, [](const auto &configuration) {
        return configuration.specification.primary;
      });
      if (primary != configurations.end() && !display_api.setAsPrimary(platform_to_device.at(primary->platform_id))) {
        BOOST_LOG(error) << "Eclipse MttVDD: primary display application failed";
        return false;
      }

      const auto devices = display_api.enumAvailableDevices();
      for (const auto &configuration : configurations) {
        const auto &id = platform_to_device.at(configuration.platform_id);
        const auto device = std::ranges::find(devices, id, &display_device::EnumeratedDevice::m_device_id);
        if (device == devices.end() || device->m_display_name.empty()) {
          BOOST_LOG(error) << "Eclipse MttVDD: activated connector is absent from display enumeration";
          return false;
        }
        DEVMODEA mode {.dmSize = sizeof(DEVMODEA)};
        if (!EnumDisplaySettingsExA(device->m_display_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
          BOOST_LOG(error) << "Eclipse MttVDD: current DEVMODE query failed";
          return false;
        }
        mode.dmPosition.x = configuration.specification.position.x;
        mode.dmPosition.y = configuration.specification.position.y;
        mode.dmDisplayOrientation = orientation(configuration.specification.rotation);
        mode.dmFields = DM_POSITION | DM_DISPLAYORIENTATION;
        if (ChangeDisplaySettingsExA(device->m_display_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) != DISP_CHANGE_SUCCESSFUL) {
          BOOST_LOG(error) << "Eclipse MttVDD: position or rotation staging failed";
          return false;
        }
      }
      if (!configurations.empty() && ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        BOOST_LOG(error) << "Eclipse MttVDD: staged position or rotation commit failed";
        return false;
      }

      const auto snapshots = display::enumerate_snapshot({}, display_api.enumAvailableDevices());
      for (auto &configuration : configurations) {
        const auto &id = platform_to_device.at(configuration.platform_id);
        const auto snapshot = std::ranges::find(snapshots, id, &display::Snapshot::device_id);
        const auto &requested = configuration.specification.mode;
        if (snapshot == snapshots.end() || !snapshot->current_mode || snapshot->scale != display::Rational {1, 1} || snapshot->position != display::Position {configuration.specification.position.x, configuration.specification.position.y} || snapshot->primary != configuration.specification.primary || snapshot->hdr_enabled.value_or(false) != configuration.specification.hdr) {
          BOOST_LOG(error) << "Eclipse MttVDD: applied connector state could not be verified";
          return false;
        }
        const auto &actual = *snapshot->current_mode;
        const auto requested_refresh = display::make_rational(requested.refresh_numerator, requested.refresh_denominator);
        if (!requested_refresh || actual.size != display::Size {static_cast<std::uint32_t>(requested.width), static_cast<std::uint32_t>(requested.height)} || actual.refresh != *requested_refresh || actual.bit_depth < static_cast<std::uint32_t>(requested.bit_depth) || actual.hdr != requested.hdr) {
          BOOST_LOG(error) << "Eclipse MttVDD: applied mode differs from request";
          return false;
        }
        configuration.actual_mode = {static_cast<int>(actual.size.width), static_cast<int>(actual.size.height), actual.refresh.numerator, actual.refresh.denominator, static_cast<int>(actual.bit_depth), actual.hdr, actual.id};
      }
      return true;
    }
  }  // namespace

  bool available() {
    if (!mttvdd::probe().available) {
      return false;
    }
    const auto inventory = mttvdd::display_inventory();
    if (!inventory) {
      return false;
    }
    auto api_layer = std::make_shared<display_device::WinApiLayer>();
    display_device::WinDisplayDevice display_api {api_layer};
    return display_api.isApiAccessAvailable() && std::ranges::all_of(*inventory, [&](const auto &platform_id) {
             return device_id(*api_layer, platform_id).has_value();
           });
  }

  eclipse_virtual_display::callbacks_t make_callbacks(const std::filesystem::path &persistence_path) {
    return {
      [persistence_path]() -> std::optional<std::string> {
        if (!std::filesystem::exists(persistence_path)) {
          return std::nullopt;
        }
        return file_handler::read_file(persistence_path.string().c_str());
      },
      [persistence_path](const std::string &document) {
        return file_handler::write_file_atomic(persistence_path.string().c_str(), document) == 0;
      },
      []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      },
      []() {
        return uuid_util::uuid_t::generate().string();
      },
      available,
      mttvdd::configured_display_count,
      mttvdd::set_display_count_and_wait,
      mttvdd::display_inventory,
      apply,
    };
  }
}  // namespace eclipse::windows::virtual_display
