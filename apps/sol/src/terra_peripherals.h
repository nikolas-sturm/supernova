/**
 * @file src/terra_peripherals.h
 * @brief Terra peripherals-v1 device registry and claim manager.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json_fwd.hpp>

namespace terra_peripherals {
  /**
   * @brief Supported forwarding device classes for this host.
   */
  constexpr const char *SUPPORTED_CLASSES[] = {"keyboard", "mouse"};

  /**
   * @brief Host-supported peripheral capability names.
   */
  constexpr const char *SUPPORTED_CAPABILITIES[] = {
    "keyboard.hid",
    "keyboard.text",
    "mouse.hid",
    "mouse.relative",
  };

  /**
   * @brief Claim lifecycle states.
   */
  enum class claim_state_t {
    pending,  ///< Claim created but channel not opened.
    active,  ///< Channel open and forwarding permitted.
    suspended,  ///< Channel closed with suspend policy.
    released,  ///< Terminal released state.
  };

  /**
   * @brief Stable API text for a claim state.
   *
   * @param state Claim state.
   * @return Lowercase contract value.
   */
  std::string_view claim_state_name(claim_state_t state);

  /**
   * @brief Peripheral forwarding target reference.
   */
  struct target_t {
    std::string type;  ///< `session`, `workspace`, or `sandbox`.
    std::string id;  ///< Canonical target UUID.
  };

  /**
   * @brief Registered peripheral device.
   */
  struct device_t {
    std::string id;  ///< Canonical host-assigned device UUID.
    std::optional<std::string> owner_client_uuid;  ///< Canonical owner UUID.
    std::string device_class;  ///< Supported device class.
    std::string platform_id;  ///< Caller-supplied platform identifier.
    std::string name;  ///< Caller-supplied device name.
    std::uint32_t vendor_id {};  ///< USB vendor identifier.
    std::uint32_t product_id {};  ///< USB product identifier.
    std::optional<std::string> serial;  ///<< Policy-permitted serial number.
    std::vector<std::string> capabilities;  ///< Class-qualified capability names.
    std::optional<std::string> report_descriptor_base64;  ///< HID report descriptor when applicable.
    std::uint64_t revision = 1;  ///< Monotonic resource revision.
    std::int64_t created_at = 0;  ///< Creation Unix time in milliseconds.
    std::int64_t updated_at = 0;  ///< Last published mutation time.
  };

  /**
   * @brief Active peripheral claim.
   */
  struct claim_t {
    std::string id;  ///< Canonical claim UUID.
    std::optional<std::string> owner_client_uuid;  ///< Canonical owner UUID.
    std::string device_id;  ///< Canonical device UUID.
    std::string device_class;  ///< Class copied from the device.
    target_t target;  ///< Bound forwarding target.
    claim_state_t state {claim_state_t::pending};  ///< Current claim state.
    std::vector<std::string> granted_capabilities;  ///< Granted subset of device capabilities.
    bool exclusive = false;  ///< Whether conflicting claims are rejected.
    std::string disconnect_policy {"release"};  ///< `release` or `suspend`.
    std::int64_t created_at = 0;  ///< Creation Unix time in milliseconds.
    std::int64_t updated_at = 0;  ///< Last published mutation time.
    std::uint64_t revision = 1;  ///< Monotonic resource revision.
    std::string credential;  ///< Opaque claim channel credential.
  };

  /**
   * @brief Device creation fields.
   */
  struct create_device_t {
    std::string device_class;  ///< Supported device class.
    std::string platform_id;  ///< Caller-supplied platform identifier.
    std::string name;  ///< Caller-supplied device name.
    std::uint32_t vendor_id {};  ///< USB vendor identifier.
    std::uint32_t product_id {};  ///< USB product identifier.
    std::optional<std::string> serial;  ///< Optional policy-permitted serial number.
    std::vector<std::string> capabilities;  ///< Class-qualified capability names.
    std::optional<std::string> report_descriptor_base64;  ///< Optional HID report descriptor.
  };

  /**
   * @brief Claim creation fields.
   */
  struct create_claim_t {
    std::string device_id;  ///< Canonical device UUID.
    target_t target;  ///< Bound forwarding target.
    std::vector<std::string> requested_capabilities;  ///< Requested capability subset.
    bool exclusive = false;  ///< Whether conflicting claims are rejected.
    std::string disconnect_policy {"release"};  ///< `release` or `suspend`.
  };

  /**
   * @brief Operation result category.
   */
  enum class status_t {
    success,  ///< Operation completed.
    not_found,  ///< Device or claim does not exist or is not visible.
    invalid,  ///< Input failed validation.
    unsupported_configuration,  ///< Class, capability, or policy is not supported.
    conflict,  ///< Exclusive claim conflicts with an active or suspended claim.
  };

  /**
   * @brief Mutation output.
   */
  template<class T>
  struct result_t {
    status_t status;  ///< Operation outcome.
    std::optional<T> resource;  ///< Resulting resource snapshot.
  };

  /**
   * @brief Serialize one device resource.
   *
   * @param device Device record.
   * @return Peripheral device object without the active claim identifier.
   */
  nlohmann::json device_json(const device_t &device, const std::optional<std::string> &active_claim_id = std::nullopt);

  /**
   * @brief Serialize one claim resource.
   *
   * @param claim Claim record.
   * @return Claim object without the channel credential.
   */
  nlohmann::json claim_json(const claim_t &claim);

  /**
   * @brief Thread-safe in-memory peripheral registry and claim manager.
   */
  class manager_t {
  public:
    /**
     * @brief Construct an empty manager.
     *
     * @param now Clock returning Unix time in milliseconds.
     * @param uuid Factory returning canonical lowercase UUIDs.
     * @param token Factory returning opaque claim credentials.
     */
    manager_t(std::function<std::int64_t()> now, std::function<std::string()> uuid, std::function<std::string()> token);

    manager_t(const manager_t &) = delete;  ///< Copying a synchronized manager is unsupported.
    manager_t &operator=(const manager_t &) = delete;  ///< Copy assignment is unsupported.

    /**
     * @brief List devices visible to one caller.
     *
     * @param owner_uuid Calling client UUID, empty for administrative visibility.
     * @return Visible device snapshots with active claim identifiers.
     */
    std::vector<std::pair<device_t, std::optional<std::string>>> list_devices(const std::string &owner_uuid) const;

    /**
     * @brief Fetch one device visible to one caller.
     *
     * @param owner_uuid Calling client UUID, empty for administrative visibility.
     * @param device_id Canonical device UUID.
     * @return Device snapshot with active claim identifier.
     */
    std::optional<std::pair<device_t, std::optional<std::string>>> get_device(const std::string &owner_uuid, const std::string &device_id) const;

    /**
     * @brief Register one peripheral device.
     *
     * @param owner_uuid Canonical owner UUID.
     * @param request Validated creation fields.
     * @return Created device snapshot.
     */
    result_t<device_t> create_device(const std::string &owner_uuid, const create_device_t &request);

    /**
     * @brief Unregister one device and release its active claim.
     *
     * @param owner_uuid Calling client UUID, empty for administrative control.
     * @param device_id Canonical device UUID.
     * @return Deleted device snapshot, or no value after failure.
     */
    result_t<device_t> delete_device(const std::string &owner_uuid, const std::string &device_id);

    /**
     * @brief List claims visible to one caller.
     *
     * @param owner_uuid Calling client UUID, empty for administrative visibility.
     * @return Visible claim snapshots.
     */
    std::vector<claim_t> list_claims(const std::string &owner_uuid) const;

    /**
     * @brief Fetch one claim visible to one caller.
     *
     * @param owner_uuid Calling client UUID, empty for administrative visibility.
     * @param claim_id Canonical claim UUID.
     * @return Claim snapshot.
     */
    std::optional<claim_t> get_claim(const std::string &owner_uuid, const std::string &claim_id) const;

    /**
     * @brief Create one claim against a visible target.
     *
     * @param owner_uuid Canonical owner UUID.
     * @param request Validated creation fields.
     * @return Created claim snapshot including its channel credential.
     */
    result_t<claim_t> create_claim(const std::string &owner_uuid, const create_claim_t &request);

    /**
     * @brief Release one claim idempotently.
     *
     * @param owner_uuid Calling client UUID, empty for administrative control.
     * @param claim_id Canonical claim UUID.
     * @return Final claim snapshot.
     */
    result_t<claim_t> release_claim(const std::string &owner_uuid, const std::string &claim_id);

    /**
     * @brief Validate one channel credential without mutating state.
     *
     * @param claim_id Canonical claim UUID.
     * @param credential Presented credential.
     * @return Claim snapshot when the credential matches a non-terminal claim.
     */
    std::optional<claim_t> authenticate_claim(const std::string &claim_id, const std::string &credential) const;

    /**
     * @brief Mark an authenticated claim active when it is not terminal.
     *
     * @param claim_id Canonical claim UUID.
     * @return Claim snapshot after activation.
     */
    result_t<claim_t> open_channel(const std::string &claim_id);

    /**
     * @brief Close one claim channel and apply its disconnect policy.
     *
     * @param claim_id Canonical claim UUID.
     * @return Final claim snapshot, or no value when the claim is unknown.
     */
    std::optional<claim_t> close_channel(const std::string &claim_id);

    /**
     * @brief Release or suspend claims bound to one target.
     *
     * @param target_type Target type filter.
     * @param target_id Canonical target UUID.
     * @param release Whether matching claims release instead of applying policy.
     */
    void target_ended(const std::string &target_type, const std::string &target_id, bool release);

    /**
     * @brief Release every claim owned by one client.
     *
     * @param owner_uuid Canonical owner UUID.
     */
    void revoke_owner(const std::string &owner_uuid);

  private:
    mutable std::mutex mutex_;  ///< Protects all registry state.
    std::function<std::int64_t()> now_;  ///< Millisecond clock.
    std::function<std::string()> uuid_;  ///< Canonical UUID factory.
    std::function<std::string()> token_;  ///< Claim credential factory.
    std::map<std::string, device_t> devices_;  ///< Registered devices keyed by UUID.
    std::map<std::string, claim_t> claims_;  ///< Claims keyed by UUID.
  };
}  // namespace terra_peripherals
