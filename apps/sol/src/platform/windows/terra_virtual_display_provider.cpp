/**
 * @file src/platform/windows/terra_virtual_display_provider.cpp
 * @brief Windows provider callbacks for Terra MttVDD lifecycle management.
 */

// header include
#include "terra_virtual_display_provider.h"

// standard includes
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <thread>

// system includes
#include <windows.h>

// lib includes
#include <display_device/windows/json.h>
#include <display_device/windows/win_api_layer.h>
#include <display_device/windows/win_display_device.h>
#include <nlohmann/json.hpp>

// local includes
#include "mttvdd.h"
#include "src/file_handler.h"
#include "src/logging.h"
#include "src/utility.h"
#include "src/uuid.h"
#include "terra_display.h"

namespace terra::windows::virtual_display {
  namespace {
    using configuration_t = terra_virtual_display::platform_configuration_t;
    constexpr std::chrono::seconds DISPLAY_CONFIG_TIMEOUT {20};  ///< Maximum DisplayConfig convergence wait.
    constexpr std::chrono::milliseconds DISPLAY_CONFIG_POLL_INTERVAL {250};  ///< DisplayConfig polling interval.

    /** @brief Exact Windows display state retained for failed mutation rollback. */
    struct rollback_state_t {
      std::mutex mutex;  ///< Protects captured state.
      std::filesystem::path path;  ///< Durable crash-recovery journal path.
      std::optional<display_device::ActiveTopology> topology;  ///< Active topology before mutation.
      std::optional<std::string> primary;  ///< Primary display device ID before mutation.
      std::map<std::string, DEVMODEA, std::less<>> modes;  ///< Exact active display modes and positions by stable device ID.
      display_device::HdrStateMap hdr_states;  ///< HDR state by stable device ID.
      bool recovery_failed {};  ///< Whether stale topology recovery must succeed before new activation.
    };

    std::shared_ptr<rollback_state_t> exclusive_rollback;  ///< Process-wide Terra exclusive-topology state.

    /** @brief Serialize one retained Win32 mode. */
    nlohmann::json mode_json(const DEVMODEA &mode) {
      return {{"x", mode.dmPosition.x}, {"y", mode.dmPosition.y}, {"width", mode.dmPelsWidth}, {"height", mode.dmPelsHeight}, {"bitsPerPel", mode.dmBitsPerPel}, {"frequency", mode.dmDisplayFrequency}, {"orientation", mode.dmDisplayOrientation}, {"flags", mode.dmDisplayFlags}, {"fields", mode.dmFields}};
    }

    /** @brief Deserialize one retained Win32 mode. */
    DEVMODEA parse_mode(const nlohmann::json &value) {
      DEVMODEA mode {.dmSize = sizeof(DEVMODEA)};
      mode.dmPosition.x = value.at("x").get<LONG>();
      mode.dmPosition.y = value.at("y").get<LONG>();
      mode.dmPelsWidth = value.at("width").get<DWORD>();
      mode.dmPelsHeight = value.at("height").get<DWORD>();
      mode.dmBitsPerPel = value.at("bitsPerPel").get<DWORD>();
      mode.dmDisplayFrequency = value.at("frequency").get<DWORD>();
      mode.dmDisplayOrientation = value.at("orientation").get<DWORD>();
      mode.dmDisplayFlags = value.at("flags").get<DWORD>();
      mode.dmFields = value.at("fields").get<DWORD>();
      return mode;
    }

    /** @brief Persist captured topology before destructive activation. */
    bool save_rollback(const rollback_state_t &state) {
      nlohmann::json modes = nlohmann::json::object();
      for (const auto &[id, mode] : state.modes) {
        modes[id] = mode_json(mode);
      }
      const nlohmann::json document {{"version", 1}, {"topology", *state.topology}, {"primary", *state.primary}, {"modes", std::move(modes)}, {"hdr", state.hdr_states}};
      return file_handler::write_file_atomic(state.path.string().c_str(), document.dump()) == 0;
    }

    /** @brief Load durable topology after process restart. */
    bool load_rollback(rollback_state_t &state) {
      if (state.topology && state.primary) {
        return true;
      }
      if (state.path.empty() || !std::filesystem::exists(state.path)) {
        return false;
      }
      try {
        const auto document = nlohmann::json::parse(file_handler::read_file(state.path.string().c_str()));
        if (document.at("version") != 1) {
          return false;
        }
        state.topology = document.at("topology").get<display_device::ActiveTopology>();
        state.primary = document.at("primary").get<std::string>();
        state.modes.clear();
        for (const auto &[id, value] : document.at("modes").items()) {
          state.modes.emplace(id, parse_mode(value));
        }
        state.hdr_states = document.at("hdr").get<display_device::HdrStateMap>();
        return true;
      } catch (...) {
        return false;
      }
    }

    /** @brief Capture exact current active topology into memory and durable journal. */
    bool capture_rollback(rollback_state_t &rollback) {
      auto api_layer = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice display_api {api_layer};
      if (!display_api.isApiAccessAvailable()) {
        return false;
      }
      const auto topology = display_api.getCurrentTopology();
      const auto devices = display_api.enumAvailableDevices();
      const auto primary = std::ranges::find_if(devices, [](const auto &device) {
        return device.m_info && device.m_info->m_primary;
      });
      if (!display_api.isTopologyValid(topology) || primary == devices.end()) {
        return false;
      }
      display_device::StringSet active_ids;
      for (const auto &group : topology) {
        active_ids.insert(group.begin(), group.end());
      }
      std::map<std::string, DEVMODEA, std::less<>> modes;
      for (const auto &device : devices) {
        if (!active_ids.contains(device.m_device_id) || device.m_display_name.empty()) {
          continue;
        }
        DEVMODEA mode {.dmSize = sizeof(DEVMODEA)};
        if (!EnumDisplaySettingsExA(device.m_display_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
          return false;
        }
        modes.emplace(device.m_device_id, mode);
      }
      const auto hdr_states = display_api.getCurrentHdrStates(active_ids);
      if (modes.size() != active_ids.size() || hdr_states.size() != active_ids.size()) {
        return false;
      }
      rollback.topology = topology;
      rollback.primary = primary->m_device_id;
      rollback.modes = std::move(modes);
      rollback.hdr_states = hdr_states;
      return save_rollback(rollback);
    }

    /** @brief Restore retained topology and clear journal only after complete success. */
    bool restore_rollback(rollback_state_t &rollback) {
      if (!load_rollback(rollback)) {
        return rollback.path.empty() || !std::filesystem::exists(rollback.path);
      }
      auto api_layer = std::make_shared<display_device::WinApiLayer>();
      display_device::WinDisplayDevice display_api {api_layer};
      if (!display_api.isApiAccessAvailable() || !display_api.setTopology(*rollback.topology) || !display_api.setAsPrimary(*rollback.primary)) {
        return false;
      }
      const auto devices = display_api.enumAvailableDevices();
      for (auto &[id, mode] : rollback.modes) {
        const auto device = std::ranges::find(devices, id, &display_device::EnumeratedDevice::m_device_id);
        if (device == devices.end() || device->m_display_name.empty() || ChangeDisplaySettingsExA(device->m_display_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) != DISP_CHANGE_SUCCESSFUL) {
          return false;
        }
      }
      if (ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL || (!rollback.hdr_states.empty() && !display_api.setHdrStates(rollback.hdr_states))) {
        return false;
      }
      const auto normalize_topology = [](auto topology) {
        for (auto &group : topology) {
          std::ranges::sort(group);
        }
        std::ranges::sort(topology);
        return topology;
      };
      if (normalize_topology(display_api.getCurrentTopology()) != normalize_topology(*rollback.topology)) {
        return false;
      }
      const auto restored_devices = display_api.enumAvailableDevices();
      const auto restored_primary = std::ranges::find_if(restored_devices, [](const auto &device) {
        return device.m_info && device.m_info->m_primary;
      });
      if (restored_primary == restored_devices.end() || restored_primary->m_device_id != *rollback.primary) {
        return false;
      }
      for (const auto &[id, expected] : rollback.modes) {
        const auto device = std::ranges::find(restored_devices, id, &display_device::EnumeratedDevice::m_device_id);
        DEVMODEA actual {.dmSize = sizeof(DEVMODEA)};
        if (device == restored_devices.end() || device->m_display_name.empty() || !EnumDisplaySettingsExA(device->m_display_name.c_str(), ENUM_CURRENT_SETTINGS, &actual, 0) || actual.dmPosition.x != expected.dmPosition.x || actual.dmPosition.y != expected.dmPosition.y || actual.dmPelsWidth != expected.dmPelsWidth || actual.dmPelsHeight != expected.dmPelsHeight || actual.dmBitsPerPel != expected.dmBitsPerPel || actual.dmDisplayFrequency != expected.dmDisplayFrequency || actual.dmDisplayOrientation != expected.dmDisplayOrientation || actual.dmDisplayFlags != expected.dmDisplayFlags) {
          return false;
        }
      }
      display_device::StringSet active_ids;
      for (const auto &[id, mode] : rollback.modes) {
        (void) mode;
        active_ids.emplace(id);
      }
      if (display_api.getCurrentHdrStates(active_ids) != rollback.hdr_states) {
        return false;
      }
      std::error_code error;
      std::filesystem::remove(rollback.path, error);
      if (error) {
        return false;
      }
      rollback.topology.reset();
      rollback.primary.reset();
      rollback.modes.clear();
      rollback.hdr_states.clear();
      return true;
    }

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
        BOOST_LOG(error) << "Terra MttVDD: display configuration API unavailable";
        return false;
      }

      std::map<std::string, std::string, std::less<>> platform_to_device;
      std::set<std::string, std::less<>> requested_devices;
      std::size_t primary_count = 0;
      for (const auto &configuration : configurations) {
        const auto &spec = configuration.specification;
        if (spec.scale != 1.0 || (spec.primary && (spec.position.x != 0 || spec.position.y != 0))) {
          BOOST_LOG(error) << "Terra MttVDD: unsupported scale or primary position";
          return false;
        }
        primary_count += spec.primary;
        const auto id = wait_device_id(*api_layer, configuration.platform_id);
        if (!id || !requested_devices.emplace(*id).second) {
          BOOST_LOG(error) << "Terra MttVDD: connector correlation failed for " << configuration.platform_id;
          return false;
        }
        platform_to_device.emplace(configuration.platform_id, *id);
      }
      if (primary_count > 1) {
        BOOST_LOG(error) << "Terra MttVDD: multiple primary displays requested";
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
        BOOST_LOG(error) << "Terra MttVDD: topology activation failed";
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
        BOOST_LOG(error) << "Terra MttVDD: mode or HDR application failed";
        return false;
      }
      const auto primary = std::ranges::find_if(configurations, [](const auto &configuration) {
        return configuration.specification.primary;
      });
      if (primary != configurations.end() && !display_api.setAsPrimary(platform_to_device.at(primary->platform_id))) {
        BOOST_LOG(error) << "Terra MttVDD: primary display application failed";
        return false;
      }

      const auto devices = display_api.enumAvailableDevices();
      for (const auto &configuration : configurations) {
        const auto &id = platform_to_device.at(configuration.platform_id);
        const auto device = std::ranges::find(devices, id, &display_device::EnumeratedDevice::m_device_id);
        if (device == devices.end() || device->m_display_name.empty()) {
          BOOST_LOG(error) << "Terra MttVDD: activated connector is absent from display enumeration";
          return false;
        }
        DEVMODEA mode {.dmSize = sizeof(DEVMODEA)};
        if (!EnumDisplaySettingsExA(device->m_display_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
          BOOST_LOG(error) << "Terra MttVDD: current DEVMODE query failed";
          return false;
        }
        mode.dmPosition.x = configuration.specification.position.x;
        mode.dmPosition.y = configuration.specification.position.y;
        mode.dmDisplayOrientation = orientation(configuration.specification.rotation);
        mode.dmFields = DM_POSITION | DM_DISPLAYORIENTATION;
        if (ChangeDisplaySettingsExA(device->m_display_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) != DISP_CHANGE_SUCCESSFUL) {
          BOOST_LOG(error) << "Terra MttVDD: position or rotation staging failed";
          return false;
        }
      }
      if (!configurations.empty() && ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr) != DISP_CHANGE_SUCCESSFUL) {
        BOOST_LOG(error) << "Terra MttVDD: staged position or rotation commit failed";
        return false;
      }

      const auto snapshots = display::enumerate_snapshot({}, display_api.enumAvailableDevices());
      for (auto &configuration : configurations) {
        const auto &id = platform_to_device.at(configuration.platform_id);
        const auto snapshot = std::ranges::find(snapshots, id, &display::Snapshot::device_id);
        const auto &requested = configuration.specification.mode;
        if (snapshot == snapshots.end() || !snapshot->current_mode || snapshot->scale != display::Rational {1, 1} || snapshot->position != display::Position {configuration.specification.position.x, configuration.specification.position.y} || snapshot->primary != configuration.specification.primary || snapshot->hdr_enabled.value_or(false) != configuration.specification.hdr) {
          BOOST_LOG(error) << "Terra MttVDD: applied connector state could not be verified";
          return false;
        }
        const auto &actual = *snapshot->current_mode;
        const auto requested_refresh = display::make_rational(requested.refresh_numerator, requested.refresh_denominator);
        if (!requested_refresh || actual.size != display::Size {static_cast<std::uint32_t>(requested.width), static_cast<std::uint32_t>(requested.height)} || actual.refresh != *requested_refresh || actual.bit_depth < static_cast<std::uint32_t>(requested.bit_depth) || actual.hdr != requested.hdr) {
          BOOST_LOG(error) << "Terra MttVDD: applied mode differs from request";
          return false;
        }
        configuration.actual_mode = {static_cast<int>(actual.size.width), static_cast<int>(actual.size.height), actual.refresh.numerator, actual.refresh.denominator, static_cast<int>(actual.bit_depth), actual.hdr, actual.id};
      }
      return true;
    }
  }  // namespace

  std::optional<std::string> resolve_device_id(const std::string_view platform_id) {
    display_device::WinApiLayer api_layer;
    return device_id(api_layer, platform_id);
  }

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

  bool activate_exclusive(const std::vector<terra_virtual_display::resource_t> &displays) {
    if (!exclusive_rollback || displays.empty() || displays.size() > 4) {
      return false;
    }
    std::lock_guard lock {exclusive_rollback->mutex};
    if (exclusive_rollback->recovery_failed) {
      if (!restore_rollback(*exclusive_rollback)) {
        return false;
      }
      exclusive_rollback->recovery_failed = false;
    }
    if (!exclusive_rollback->topology && !capture_rollback(*exclusive_rollback)) {
      return false;
    }
    auto api_layer = std::make_shared<display_device::WinApiLayer>();
    display_device::WinDisplayDevice display_api {api_layer};
    display_device::ActiveTopology topology;
    display_device::StringSet expected;
    std::vector<configuration_t> configurations;
    configurations.reserve(displays.size());
    for (const auto &display : displays) {
      const auto id = wait_device_id(*api_layer, display.platform_id);
      if (!id || !expected.emplace(*id).second) {
        static_cast<void>(restore_rollback(*exclusive_rollback));
        return false;
      }
      topology.push_back({*id});
      configurations.push_back({
        display.platform_id,
        {
          display.name,
          display.requested_mode,
          display.position,
          display.scale,
          display.rotation,
          display.primary,
          display.hdr,
          display.persistent,
          display.workspace_id,
        },
        display.actual_mode,
      });
    }
    if (!display_api.isApiAccessAvailable() || !display_api.isTopologyValid(topology) || !display_api.setTopology(topology) || !apply(configurations)) {
      static_cast<void>(restore_rollback(*exclusive_rollback));
      return false;
    }
    display_device::StringSet active;
    for (const auto &group : display_api.getCurrentTopology()) {
      active.insert(group.begin(), group.end());
    }
    if (active != expected) {
      static_cast<void>(restore_rollback(*exclusive_rollback));
      return false;
    }
    return true;
  }

  bool restore_exclusive() {
    if (!exclusive_rollback) {
      return true;
    }
    std::lock_guard lock {exclusive_rollback->mutex};
    const bool restored = restore_rollback(*exclusive_rollback);
    exclusive_rollback->recovery_failed = !restored;
    return restored;
  }

  terra_virtual_display::callbacks_t make_callbacks(const std::filesystem::path &persistence_path) {
    const auto rollback = std::make_shared<rollback_state_t>();
    const auto exclusive = std::make_shared<rollback_state_t>();
    exclusive->path = persistence_path.parent_path() / "eclipse_display_topology_rollback.json";
    exclusive_rollback = exclusive;
    if (std::filesystem::exists(exclusive->path) && !restore_exclusive()) {
      exclusive->recovery_failed = true;
      BOOST_LOG(error) << "Terra MttVDD: stale exclusive display topology could not be restored";
    }
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
      mttvdd::MAX_DISPLAY_COUNT,
      {},
      [rollback]() {
        auto api_layer = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice display_api {api_layer};
        if (!display_api.isApiAccessAvailable()) {
          return false;
        }
        const auto topology = display_api.getCurrentTopology();
        const auto devices = display_api.enumAvailableDevices();
        const auto primary = std::ranges::find_if(devices, [](const auto &device) {
          return device.m_info && device.m_info->m_primary;
        });
        if (!display_api.isTopologyValid(topology) || primary == devices.end()) {
          return false;
        }
        std::map<std::string, DEVMODEA, std::less<>> modes;
        display_device::StringSet active_ids;
        for (const auto &group : topology) {
          active_ids.insert(group.begin(), group.end());
        }
        for (const auto &device : devices) {
          if (!active_ids.contains(device.m_device_id) || device.m_display_name.empty()) {
            continue;
          }
          DEVMODEA mode {.dmSize = sizeof(DEVMODEA)};
          if (!EnumDisplaySettingsExA(device.m_display_name.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0)) {
            return false;
          }
          modes.emplace(device.m_device_id, mode);
        }
        const auto hdr_states = display_api.getCurrentHdrStates(active_ids);
        if (modes.size() != active_ids.size() || hdr_states.size() != active_ids.size()) {
          return false;
        }
        std::lock_guard lock {rollback->mutex};
        rollback->topology = topology;
        rollback->primary = primary->m_device_id;
        rollback->modes = std::move(modes);
        rollback->hdr_states = hdr_states;
        return true;
      },
      [rollback]() {
        std::optional<display_device::ActiveTopology> topology;
        std::optional<std::string> primary;
        std::map<std::string, DEVMODEA, std::less<>> modes;
        display_device::HdrStateMap hdr_states;
        {
          std::lock_guard lock {rollback->mutex};
          topology = rollback->topology;
          primary = rollback->primary;
          modes = rollback->modes;
          hdr_states = rollback->hdr_states;
        }
        if (!topology || !primary || modes.empty()) {
          return false;
        }
        auto api_layer = std::make_shared<display_device::WinApiLayer>();
        display_device::WinDisplayDevice display_api {api_layer};
        if (!display_api.isApiAccessAvailable()) {
          return false;
        }
        const bool topology_restored = display_api.setTopology(*topology);
        const bool primary_restored = topology_restored && display_api.setAsPrimary(*primary);
        const auto devices = display_api.enumAvailableDevices();
        bool staged = true;
        for (auto &[device_id, mode] : modes) {
          const auto device = std::ranges::find(devices, device_id, &display_device::EnumeratedDevice::m_device_id);
          if (device == devices.end() || device->m_display_name.empty() || ChangeDisplaySettingsExA(device->m_display_name.c_str(), &mode, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr) != DISP_CHANGE_SUCCESSFUL) {
            staged = false;
          }
        }
        const bool committed = ChangeDisplaySettingsExA(nullptr, nullptr, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
        const bool hdr_restored = hdr_states.empty() || display_api.setHdrStates(hdr_states);
        return topology_restored && staged && committed && hdr_restored && primary_restored;
      },
    };
  }
}  // namespace terra::windows::virtual_display
