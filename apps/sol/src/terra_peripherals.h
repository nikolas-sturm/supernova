/**
 * @file src/terra_peripherals.h
 * @brief Terra peripherals-v1 device registry and claim manager.
 */
#pragma once

// standard includes
#include <cstddef>
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
    "mouse.hid",
  };

  constexpr std::size_t MAX_DEVICES = 32;  ///< Maximum registered devices.
  constexpr std::size_t MAX_CLAIMS = 32;  ///< Maximum concurrent non-terminal claims.
  constexpr std::size_t MAX_RETAINED_CLAIMS = 128;  ///< Maximum retained active and terminal claim records.
  constexpr std::size_t MAX_MESSAGE_BYTES = 64 * 1024;  ///< Maximum encoded WebSocket message size.
  constexpr std::size_t MAX_PAYLOAD_BYTES = 8;  ///< Maximum decoded fixed boot-protocol HID report size.
  constexpr std::int64_t CLAIM_CREDENTIAL_TTL_MS = 5 * 60 * 1000;  ///< Claim channel credential lifetime.

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
    std::optional<std::string> serial;  ///< Policy-permitted serial number.
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
    std::int64_t credential_expires_at = 0;  ///< Credential expiry Unix time in milliseconds.
  };

  /**
   * @brief Resources removed or released when one owner loses authorization.
   */
  struct owner_revocation_t {
    std::vector<device_t> devices;  ///< Devices removed from the registry.
    std::vector<claim_t> claims;  ///< Claims transitioned to released.
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
    conflict,  ///< Exclusive claim conflicts with another non-terminal claim.
    limit_reached,  ///< Published device or active-claim limit was reached.
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
   * @brief Claim-creation output including atomic credential-expiry transitions.
   */
  struct claim_result_t {
    status_t status;  ///< Creation outcome.
    std::optional<claim_t> resource;  ///< Created claim snapshot.
    std::vector<claim_t> expired_claims;  ///< Claims released before conflict and quota checks.
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
   * @brief Serialize one newly created claim with its one-time channel grant.
   *
   * @param claim Newly created claim containing channel credential metadata.
   * @param endpoint Relative WebSocket endpoint for this claim.
   * @return Claim object whose channel field includes protocol and credential data.
   */
  nlohmann::json claim_creation_json(const claim_t &claim, std::string_view endpoint);

  /**
   * @brief Count non-terminal claims in a caller-visible snapshot.
   *
   * @param claims Claims already filtered for caller visibility.
   * @return Number of pending, active, or suspended claims.
   */
  std::size_t active_claim_count(const std::vector<claim_t> &claims);

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
     * @return Visible device snapshots with non-terminal claim identifiers.
     */
    std::vector<std::pair<device_t, std::optional<std::string>>> list_devices(const std::string &owner_uuid) const;

    /**
     * @brief Fetch one device visible to one caller.
     *
     * @param owner_uuid Calling client UUID, empty for administrative visibility.
     * @param device_id Canonical device UUID.
     * @return Device snapshot with non-terminal claim identifier.
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
     * @param expected_revision Required current device revision.
     * @param released_claims Receives claims released by the deletion transaction.
     * @return Deleted device snapshot, or no value after failure.
     */
    result_t<device_t> delete_device(const std::string &owner_uuid, const std::string &device_id, std::uint64_t expected_revision, std::vector<claim_t> &released_claims);

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
    claim_result_t create_claim(const std::string &owner_uuid, const create_claim_t &request);

    /**
     * @brief Release one claim idempotently.
     *
     * @param owner_uuid Calling client UUID, empty for administrative control.
     * @param claim_id Canonical claim UUID.
     * @param expected_revision Required current claim revision.
     * @return Final claim snapshot.
     */
    result_t<claim_t> release_claim(const std::string &owner_uuid, const std::string &claim_id, std::uint64_t expected_revision);

    /**
     * @brief Validate one channel credential without mutating state.
     *
     * @param claim_id Canonical claim UUID.
     * @param credential Presented credential.
     * @param owner_uuid Canonical UUID authenticated by the presented client certificate.
     * @return Claim snapshot when the credential matches a non-terminal claim.
     */
    std::optional<claim_t> authenticate_claim(const std::string &claim_id, const std::string &credential, const std::string &owner_uuid) const;

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
     * @brief Release inactive claims whose channel credentials expired.
     *
     * @return Changed claim snapshots.
     */
    std::vector<claim_t> expire_credentials();

    /**
     * @brief Release or suspend claims bound to one target.
     *
     * @param target_type Target type filter.
     * @param target_id Canonical target UUID.
     * @param release Whether matching claims release instead of applying policy.
     * @return Changed claim snapshots.
     */
    std::vector<claim_t> target_ended(const std::string &target_type, const std::string &target_id, bool release);

    /**
     * @brief Remove every device and release every claim owned by one client.
     *
     * @param owner_uuid Canonical owner UUID.
     * @param device_classes Optional device-class filter; empty revokes every class.
     * @return Removed devices and changed claim snapshots.
     */
    owner_revocation_t revoke_owner(const std::string &owner_uuid, const std::vector<std::string> &device_classes = {});

  private:
    /**
     * @brief Resolve device's currently projected non-terminal claim.
     *
     * @param device_id Canonical device UUID.
     * @return Claim UUID included by device serialization, or no value.
     */
    std::optional<std::string> active_claim_id_locked(const std::string &device_id) const;

    /**
     * @brief Advance device revision when an associated claim changes.
     *
     * @param device_id Canonical device UUID.
     * @param now Mutation timestamp.
     */
    void touch_device_locked(const std::string &device_id, std::int64_t now);

    /**
     * @brief Release expired inactive claims while caller holds manager lock.
     *
     * @param now Current Unix time in milliseconds.
     * @return Changed claim snapshots.
     */
    std::vector<claim_t> expire_credentials_locked(std::int64_t now);

    /**
     * @brief Remove oldest released claims until one new record fits.
     */
    void prune_released_claims_locked();

    mutable std::mutex mutex_;  ///< Protects all registry state.
    std::function<std::int64_t()> now_;  ///< Millisecond clock.
    std::function<std::string()> uuid_;  ///< Canonical UUID factory.
    std::function<std::string()> token_;  ///< Claim credential factory.
    std::map<std::string, device_t> devices_;  ///< Registered devices keyed by UUID.
    std::map<std::string, claim_t> claims_;  ///< Claims keyed by UUID.
  };
}  // namespace terra_peripherals
