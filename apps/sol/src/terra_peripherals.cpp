/**
 * @file src/terra_peripherals.cpp
 * @brief Terra peripherals-v1 device registry and claim manager implementation.
 */
// standard includes
#include "terra_peripherals.h"

#include <algorithm>
#include <ranges>
#include <set>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include <src/utility.h>

namespace terra_peripherals {
  namespace {
    /**
     * @brief Confirm a value appears in a supported-name list.
     *
     * @param value Candidate value.
     * @param supported Supported name list.
     * @return True when the value is supported.
     */
    template<std::size_t N>
    bool supported_value(std::string_view value, const char *const (&supported)[N]) {
      for (const auto candidate : supported) {
        if (value == candidate) {
          return true;
        }
      }
      return false;
    }

    /**
     * @brief Confirm every entry appears in a supported-name list.
     *
     * @param values Candidate values.
     * @param supported Supported name list.
     * @return True when every value is supported.
     */
    template<std::size_t N>
    bool supported_values(const std::vector<std::string> &values, const char *const (&supported)[N]) {
      return std::ranges::all_of(values, [&](std::string_view value) {
        return supported_value(value, supported);
      });
    }

    /**
     * @brief Confirm every entry is unique.
     *
     * @param values Candidate values.
     * @return True when no duplicates exist.
     */
    bool unique_values(const std::vector<std::string> &values) {
      return std::set<std::string>(values.begin(), values.end()).size() == values.size();
    }
  }  // namespace

  std::string_view claim_state_name(claim_state_t state) {
    switch (state) {
      case claim_state_t::pending:
        return "pending";
      case claim_state_t::active:
        return "active";
      case claim_state_t::suspended:
        return "suspended";
      case claim_state_t::released:
      default:
        return "released";
    }
  }

  nlohmann::json device_json(const device_t &device, const std::optional<std::string> &active_claim_id) {
    nlohmann::json capabilities = nlohmann::json::array();
    for (const auto &capability : device.capabilities) {
      capabilities.push_back(capability);
    }
    return {
      {"id", device.id},
      {"ownerClientUuid", device.owner_client_uuid ? nlohmann::json(*device.owner_client_uuid) : nlohmann::json(nullptr)},
      {"class", device.device_class},
      {"platformId", device.platform_id},
      {"name", device.name},
      {"vendorId", device.vendor_id},
      {"productId", device.product_id},
      {"serial", device.serial ? nlohmann::json(*device.serial) : nlohmann::json(nullptr)},
      {"capabilities", std::move(capabilities)},
      {"reportDescriptorBase64", device.report_descriptor_base64 ? nlohmann::json(*device.report_descriptor_base64) : nlohmann::json(nullptr)},
      {"connected", true},
      {"claimable", !active_claim_id},
      {"activeClaimId", active_claim_id ? nlohmann::json(*active_claim_id) : nlohmann::json(nullptr)},
      {"revision", device.revision},
      {"createdAt", device.created_at},
      {"updatedAt", device.updated_at},
    };
  }

  nlohmann::json claim_json(const claim_t &claim) {
    nlohmann::json capabilities = nlohmann::json::array();
    for (const auto &capability : claim.granted_capabilities) {
      capabilities.push_back(capability);
    }
    return {
      {"id", claim.id},
      {"ownerClientUuid", claim.owner_client_uuid ? nlohmann::json(*claim.owner_client_uuid) : nlohmann::json(nullptr)},
      {"deviceId", claim.device_id},
      {"deviceClass", claim.device_class},
      {"target", {
                   {"type", claim.target.type},
                   {"id", claim.target.id},
                 }},
      {"state", claim_state_name(claim.state)},
      {"grantedCapabilities", std::move(capabilities)},
      {"exclusive", claim.exclusive},
      {"disconnectPolicy", claim.disconnect_policy},
      {"createdAt", claim.created_at},
      {"updatedAt", claim.updated_at},
      {"error", nullptr},
      {"revision", claim.revision},
      {"channel", nullptr},
    };
  }

  manager_t::manager_t(std::function<std::int64_t()> now, std::function<std::string()> uuid, std::function<std::string()> token):
      now_(std::move(now)),
      uuid_(std::move(uuid)),
      token_(std::move(token)) {
  }

  std::vector<std::pair<device_t, std::optional<std::string>>> manager_t::list_devices(const std::string &owner_uuid) const {
    std::scoped_lock lock {mutex_};
    std::vector<std::pair<device_t, std::optional<std::string>>> result;
    for (const auto &[id, device] : devices_) {
      if (!owner_uuid.empty() && (!device.owner_client_uuid || *device.owner_client_uuid != owner_uuid)) {
        continue;
      }
      std::optional<std::string> active_claim;
      for (const auto &[claim_id, claim] : claims_) {
        if (claim.device_id == id && (claim.state == claim_state_t::active || claim.state == claim_state_t::suspended)) {
          active_claim = claim_id;
        }
      }
      result.emplace_back(device, active_claim);
    }
    return result;
  }

  std::optional<std::pair<device_t, std::optional<std::string>>> manager_t::get_device(const std::string &owner_uuid, const std::string &device_id) const {
    auto devices = list_devices(owner_uuid);
    const auto found = std::ranges::find_if(devices, [&](const auto &entry) {
      return entry.first.id == device_id;
    });
    return found == devices.end() ? std::nullopt : std::optional {*found};
  }

  result_t<device_t> manager_t::create_device(const std::string &owner_uuid, const create_device_t &request) {
    std::scoped_lock lock {mutex_};
    if (request.platform_id.empty() || request.name.empty()) {
      return {status_t::invalid, std::nullopt};
    }
    if (!supported_value(request.device_class, SUPPORTED_CLASSES)) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    if (request.capabilities.empty() || !supported_values(request.capabilities, SUPPORTED_CAPABILITIES) || !unique_values(request.capabilities)) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    for (const auto &capability : request.capabilities) {
      if (capability.starts_with(request.device_class)) {
        continue;
      }
      return {status_t::unsupported_configuration, std::nullopt};
    }
    const auto now = now_();
    device_t device {
      .id = uuid_(),
      .owner_client_uuid = owner_uuid.empty() ? std::nullopt : std::optional {owner_uuid},
      .device_class = request.device_class,
      .platform_id = request.platform_id,
      .name = request.name,
      .vendor_id = request.vendor_id,
      .product_id = request.product_id,
      .serial = request.serial,
      .capabilities = request.capabilities,
      .report_descriptor_base64 = request.report_descriptor_base64,
      .revision = 1,
      .created_at = now,
      .updated_at = now,
    };
    devices_.emplace(device.id, device);
    return {status_t::success, std::move(device)};
  }

  result_t<device_t> manager_t::delete_device(const std::string &owner_uuid, const std::string &device_id) {
    std::scoped_lock lock {mutex_};
    const auto found = devices_.find(device_id);
    if (found == devices_.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (!owner_uuid.empty() && (!found->second.owner_client_uuid || *found->second.owner_client_uuid != owner_uuid)) {
      return {status_t::not_found, std::nullopt};
    }
    std::vector<claim_t> released;
    for (auto &[claim_id, claim] : claims_) {
      if (claim.device_id != device_id || claim.state == claim_state_t::released) {
        continue;
      }
      claim.state = claim_state_t::released;
      claim.updated_at = now_();
      ++claim.revision;
      released.push_back(claim);
    }
    auto device = found->second;
    devices_.erase(found);
    static_cast<void>(released);
    return {status_t::success, std::move(device)};
  }

  std::vector<claim_t> manager_t::list_claims(const std::string &owner_uuid) const {
    std::scoped_lock lock {mutex_};
    std::vector<claim_t> result;
    for (const auto &[id, claim] : claims_) {
      if (!owner_uuid.empty() && (!claim.owner_client_uuid || *claim.owner_client_uuid != owner_uuid)) {
        continue;
      }
      result.push_back(claim);
    }
    return result;
  }

  std::optional<claim_t> manager_t::get_claim(const std::string &owner_uuid, const std::string &claim_id) const {
    std::scoped_lock lock {mutex_};
    const auto found = claims_.find(claim_id);
    if (found == claims_.end()) {
      return std::nullopt;
    }
    if (!owner_uuid.empty() && (!found->second.owner_client_uuid || *found->second.owner_client_uuid != owner_uuid)) {
      return std::nullopt;
    }
    return found->second;
  }

  result_t<claim_t> manager_t::create_claim(const std::string &owner_uuid, const create_claim_t &request) {
    std::scoped_lock lock {mutex_};
    const auto device = devices_.find(request.device_id);
    if (device == devices_.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (!owner_uuid.empty() && (!device->second.owner_client_uuid || *device->second.owner_client_uuid != owner_uuid)) {
      return {status_t::not_found, std::nullopt};
    }
    if (request.target.type != "session" && request.target.type != "workspace" && request.target.type != "sandbox") {
      return {status_t::invalid, std::nullopt};
    }
    if (request.disconnect_policy != "release" && request.disconnect_policy != "suspend") {
      return {status_t::invalid, std::nullopt};
    }
    if (request.requested_capabilities.empty() || !supported_values(request.requested_capabilities, SUPPORTED_CAPABILITIES) || !unique_values(request.requested_capabilities)) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    for (const auto &capability : request.requested_capabilities) {
      if (std::ranges::contains(device->second.capabilities, capability)) {
        continue;
      }
      return {status_t::unsupported_configuration, std::nullopt};
    }
    if (request.exclusive) {
      for (const auto &[claim_id, claim] : claims_) {
        if (claim.device_id == request.device_id && claim.state != claim_state_t::released) {
          return {status_t::conflict, std::nullopt};
        }
      }
    }
    const auto now = now_();
    claim_t claim {
      .id = uuid_(),
      .owner_client_uuid = owner_uuid.empty() ? std::nullopt : std::optional {owner_uuid},
      .device_id = request.device_id,
      .device_class = device->second.device_class,
      .target = request.target,
      .state = claim_state_t::pending,
      .granted_capabilities = request.requested_capabilities,
      .exclusive = request.exclusive,
      .disconnect_policy = request.disconnect_policy,
      .created_at = now,
      .updated_at = now,
      .revision = 1,
      .credential = token_(),
    };
    claims_.emplace(claim.id, claim);
    return {status_t::success, std::move(claim)};
  }

  result_t<claim_t> manager_t::release_claim(const std::string &owner_uuid, const std::string &claim_id) {
    std::scoped_lock lock {mutex_};
    const auto found = claims_.find(claim_id);
    if (found == claims_.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (!owner_uuid.empty() && (!found->second.owner_client_uuid || *found->second.owner_client_uuid != owner_uuid)) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.state != claim_state_t::released) {
      found->second.state = claim_state_t::released;
      found->second.updated_at = now_();
      ++found->second.revision;
    }
    return {status_t::success, found->second};
  }

  std::optional<claim_t> manager_t::authenticate_claim(const std::string &claim_id, const std::string &credential) const {
    std::scoped_lock lock {mutex_};
    const auto found = claims_.find(claim_id);
    if (found == claims_.end() || found->second.credential != credential || found->second.state == claim_state_t::released) {
      return std::nullopt;
    }
    return found->second;
  }

  result_t<claim_t> manager_t::open_channel(const std::string &claim_id) {
    std::scoped_lock lock {mutex_};
    const auto found = claims_.find(claim_id);
    if (found == claims_.end() || found->second.state == claim_state_t::released) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.state != claim_state_t::active) {
      found->second.state = claim_state_t::active;
      found->second.updated_at = now_();
      ++found->second.revision;
    }
    return {status_t::success, found->second};
  }

  std::optional<claim_t> manager_t::close_channel(const std::string &claim_id) {
    std::scoped_lock lock {mutex_};
    const auto found = claims_.find(claim_id);
    if (found == claims_.end()) {
      return std::nullopt;
    }
    if (found->second.state == claim_state_t::active) {
      if (found->second.disconnect_policy == "suspend") {
        found->second.state = claim_state_t::suspended;
      } else {
        found->second.state = claim_state_t::released;
      }
      found->second.updated_at = now_();
      ++found->second.revision;
    }
    return found->second;
  }

  void manager_t::target_ended(const std::string &target_type, const std::string &target_id, bool release) {
    std::scoped_lock lock {mutex_};
    for (auto &[claim_id, claim] : claims_) {
      if (claim.target.type != target_type || claim.target.id != target_id || claim.state == claim_state_t::released) {
        continue;
      }
      if (release || claim.disconnect_policy != "suspend") {
        claim.state = claim_state_t::released;
      } else {
        claim.state = claim_state_t::suspended;
      }
      claim.updated_at = now_();
      ++claim.revision;
    }
  }

  void manager_t::revoke_owner(const std::string &owner_uuid) {
    std::scoped_lock lock {mutex_};
    for (auto &[claim_id, claim] : claims_) {
      if (claim.owner_client_uuid && *claim.owner_client_uuid == owner_uuid && claim.state != claim_state_t::released) {
        claim.state = claim_state_t::released;
        claim.updated_at = now_();
        ++claim.revision;
      }
    }
  }
}  // namespace terra_peripherals
