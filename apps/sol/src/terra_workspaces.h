/**
 * @file src/terra_workspaces.h
 * @brief Standalone Terra workspace definition and lifecycle core.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

/**
 * @brief Thread-safe Terra workspace services.
 */
namespace terra_workspaces {
  /**
   * @brief Workspace lifecycle state.
   */
  enum class state_t {
    stopped,  ///< No runtime resources exist.
    preparing,  ///< Runtime resources are being prepared.
    ready,  ///< Resources exist without an active transport.
    active,  ///< Application transport is active.
    stopping,  ///< Runtime resources are being stopped.
    failed,  ///< Preparation or reconciliation failed.
  };

  /**
   * @brief Cleanup trigger for workspace resources.
   */
  enum class cleanup_policy_t {
    on_disconnect,  ///< Clean resources after transport disconnect.
    on_stop,  ///< Clean resources after terminating stop.
    retain,  ///< Retain resources until explicit deletion or revocation.
  };

  /**
   * @brief Peripheral requirements attached to workspace definition.
   */
  struct peripheral_policy_t {
    std::vector<std::string> required_device_ids;  ///< Required canonical device UUIDs.
    std::vector<std::string> required_classes;  ///< Required provider-defined device classes.
    std::string disconnect_policy;  ///< Either `release` or `suspend`.
  };

  /**
   * @brief Exact mutable workspace definition fields.
   */
  struct definition_t {
    std::string name;  ///< User-visible name.
    std::string description;  ///< User-visible description.
    bool shared;  ///< Whether authorized clients may see workspace.
    std::string desktop_app_uuid;  ///< Default desktop application UUID.
    std::vector<std::string> permitted_app_uuids;  ///< Applications permitted in workspace.
    std::optional<std::string> display_profile_id;  ///< Display profile UUID.
    std::optional<std::string> stream_profile_id;  ///< Stream profile UUID.
    std::optional<std::string> launch_profile_id;  ///< Launch profile UUID.
    std::optional<std::string> sandbox_profile_id;  ///< Sandbox profile UUID.
    std::vector<nlohmann::json> virtual_displays;  ///< Complete virtual-display creation objects without workspace ID.
    peripheral_policy_t peripheral_policy;  ///< Peripheral requirements.
    bool persistent;  ///< Whether definition survives restart.
    cleanup_policy_t cleanup_policy;  ///< Resource cleanup trigger.
  };

  /**
   * @brief Optional complete replacements accepted by workspace patch.
   */
  struct patch_t {
    std::optional<std::string> name;  ///< Replacement name.
    std::optional<std::string> description;  ///< Replacement description.
    std::optional<bool> shared;  ///< Replacement sharing state.
    std::optional<std::string> desktop_app_uuid;  ///< Replacement desktop app UUID.
    std::optional<std::vector<std::string>> permitted_app_uuids;  ///< Replacement app UUID array.
    std::optional<std::optional<std::string>> display_profile_id;  ///< Replacement or cleared display profile.
    std::optional<std::optional<std::string>> stream_profile_id;  ///< Replacement or cleared stream profile.
    std::optional<std::optional<std::string>> launch_profile_id;  ///< Replacement or cleared launch profile.
    std::optional<std::optional<std::string>> sandbox_profile_id;  ///< Replacement or cleared sandbox profile.
    std::optional<std::vector<nlohmann::json>> virtual_displays;  ///< Replacement virtual-display array.
    std::optional<peripheral_policy_t> peripheral_policy;  ///< Replacement peripheral policy.
    std::optional<bool> persistent;  ///< Replacement persistence state.
    std::optional<cleanup_policy_t> cleanup_policy;  ///< Replacement cleanup policy.
  };

  /**
   * @brief Per-start complete profile overrides.
   */
  struct profile_overrides_t {
    std::optional<nlohmann::json> display;  ///< Complete display profile configuration or JSON null.
    std::optional<nlohmann::json> stream;  ///< Complete stream profile configuration or JSON null.
    std::optional<nlohmann::json> launch;  ///< Complete launch profile configuration or JSON null.
    std::optional<nlohmann::json> sandbox;  ///< Complete sandbox profile configuration or JSON null.
  };

  /**
   * @brief Optional workspace start request fields.
   */
  struct start_request_t {
    std::optional<std::string> app_uuid;  ///< Application override UUID.
    profile_overrides_t profile_overrides;  ///< Ephemeral complete profile overrides.
    std::optional<std::vector<nlohmann::json>> virtual_displays;  ///< Ephemeral runtime display topology, or no value to use the stored definition.
  };

  /**
   * @brief Resolved start-time selections retained while workspace runtime exists.
   */
  struct runtime_selection_t {
    std::string app_uuid;  ///< Application UUID selected when workspace was prepared.
    profile_overrides_t profile_overrides;  ///< Validated effective start-time profile overrides.
  };

  /**
   * @brief Parse exact workspace creation object.
   *
   * @param value JSON creation object without `schemaVersion`.
   * @return Parsed definition, or no value for unknown, missing, or mistyped fields.
   */
  std::optional<definition_t> parse_definition(const nlohmann::json &value);

  /**
   * @brief Parse non-empty workspace patch object.
   *
   * @param value JSON patch object without `schemaVersion`.
   * @return Parsed patch, or no value for unknown or mistyped fields.
   */
  std::optional<patch_t> parse_patch(const nlohmann::json &value);

  /**
   * @brief Parse exact optional workspace start fields.
   *
   * @param value JSON start object without `schemaVersion`.
   * @return Parsed start request, or no value for unknown or mistyped fields.
   */
  std::optional<start_request_t> parse_start_request(const nlohmann::json &value);

  /**
   * @brief Published workspace resource.
   */
  struct resource_t {
    std::string id;  ///< Stable lowercase canonical UUID.
    std::optional<std::string> owner_client_uuid;  ///< Owner UUID, or no value when orphaned.
    definition_t definition;  ///< Published definition fields.
    state_t state;  ///< Current lifecycle state.
    std::optional<std::string> session_id;  ///< Actual runtime session UUID.
    std::optional<std::string> sandbox_id;  ///< Actual runtime sandbox UUID.
    std::vector<std::string> display_ids;  ///< Actual runtime display UUIDs.
    std::vector<std::string> peripheral_claim_ids;  ///< Actual runtime claim UUIDs.
    std::int64_t created_at;  ///< Creation Unix time in milliseconds.
    std::int64_t updated_at;  ///< Last published change Unix time in milliseconds.
    nlohmann::json error;  ///< Standard error object or JSON null.
    std::uint64_t revision;  ///< Monotonic resource revision.
    std::optional<runtime_selection_t> runtime_selection;  ///< Persisted start-time selections consumed by launch and resume.
  };

  /**
   * @brief Runtime preparation context after policy validation.
   */
  struct preparation_t {
    std::string workspace_id;  ///< Workspace UUID.
    std::string owner_client_uuid;  ///< Workspace owner UUID.
    std::string app_uuid;  ///< Resolved application UUID.
    definition_t definition;  ///< Definition snapshot.
    profile_overrides_t profile_overrides;  ///< Validated start overrides.
    std::optional<std::vector<nlohmann::json>> virtual_displays;  ///< Ephemeral runtime display topology override, or no value to use the definition.
  };

  /**
   * @brief Provider result carrying actual created runtime identifiers.
   */
  struct prepared_t {
    bool success;  ///< Whether preparation completed; failed results may carry partial IDs for cleanup.
    std::optional<std::string> session_id;  ///< Reserved session UUID; workspace preparation requires no value.
    std::optional<std::string> sandbox_id;  ///< Actual sandbox UUID.
    std::vector<std::string> display_ids;  ///< Actual display UUIDs.
    std::vector<std::string> peripheral_claim_ids;  ///< Actual claim UUIDs.
  };

  /**
   * @brief Injected persistence, policy, and runtime provider operations.
   */
  struct callbacks_t {
    /**
     * Callbacks are invoked while manager serialization lock is held and MUST NOT call back into
     * same manager instance.
     */
    std::function<std::optional<std::string>()> load;  ///< Load complete persisted JSON document.
    std::function<bool(const std::string &)> save;  ///< Atomically replace persisted JSON document.
    std::function<std::int64_t()> now;  ///< Return current Unix time in milliseconds.
    std::function<std::string()> uuid;  ///< Generate lowercase canonical UUID.
    std::function<bool(const std::string &)> validate_app;  ///< Validate application existence and policy.
    std::function<bool(const std::string &, const std::string &)> validate_profile;  ///< Validate profile kind and UUID.
    std::function<bool(const std::string &, const nlohmann::json &)> validate_profile_configuration;  ///< Validate complete override by kind.
    std::function<bool(const nlohmann::json &)> validate_virtual_display;  ///< Validate one creation object.
    std::function<bool(const peripheral_policy_t &)> validate_peripheral_policy;  ///< Validate peripheral availability and policy.
    std::function<std::optional<prepared_t>(const preparation_t &, const std::function<bool(const prepared_t &)> &)> prepare;  ///< Prepare resources and durably report cumulative acquired IDs before acquiring another.
    std::function<prepared_t(const prepared_t &)> cleanup_failed_prepare;  ///< Clean failed preparation and return unresolved IDs for retry.
    std::function<bool(const resource_t &, bool)> stop;  ///< Disconnect or terminate runtime resources.
    std::function<bool(const resource_t &)> restore_after_failed_stop;  ///< Restore runtime and confirm success after transactional stop rollback.
    std::function<bool(const resource_t &)> reconcile;  ///< Confirm persisted runtime resources still exist after restart.
    std::function<void(const std::optional<resource_t> &, const std::optional<resource_t> &)> changed;  ///< Publish `(previous, current)` after durable state changes.
  };

  /**
   * @brief Operation result category.
   */
  enum class status_t {
    success,  ///< Operation published successfully.
    not_found,  ///< Workspace does not exist.
    invalid,  ///< Input or callback result is invalid.
    conflict,  ///< Revision or lifecycle state conflicts.
    resource_busy,  ///< Adoption target already has owner.
    preparation_error,  ///< Runtime preparation failed.
    provider_error,  ///< Runtime stop or reconciliation failed.
    persistence_error,  ///< Candidate state could not be persisted.
    unavailable,  ///< Initialization failed closed.
  };

  /**
   * @brief Mutation result.
   */
  struct result_t {
    status_t status;  ///< Result category.
    std::optional<resource_t> resource;  ///< Resulting resource when available.
  };

  /**
   * @brief Workspace collection snapshot.
   */
  struct list_t {
    std::uint64_t revision;  ///< Current collection revision.
    bool changed;  ///< Whether collection changed after requested revision.
    std::vector<resource_t> workspaces;  ///< Full snapshot, empty when unchanged.
  };

  /**
   * @brief Thread-safe persisted workspace manager.
   */
  class manager_t {
  public:
    /**
     * @brief Construct manager and reconcile persisted runtime state.
     *
     * @param callbacks Injected host operations.
     */
    explicit manager_t(callbacks_t callbacks);
    /** @brief Destroy manager implementation. */
    ~manager_t();
    manager_t(const manager_t &) = delete;  ///< Copying synchronized manager is unsupported.
    manager_t &operator=(const manager_t &) = delete;  ///< Copy assignment is unsupported.
    manager_t(manager_t &&) = delete;  ///< Moving synchronized manager is unsupported.
    manager_t &operator=(manager_t &&) = delete;  ///< Move assignment is unsupported.

    /** @brief Return whether initialization succeeded. @return True when usable. */
    bool available() const;
    /** @brief Create workspace. @param owner_client_uuid Owner UUID. @param definition Definition fields. @return Mutation result. */
    result_t create(const std::string &owner_client_uuid, const definition_t &definition);
    /** @brief Read workspace. @param id Workspace UUID. @return Snapshot or no value. */
    std::optional<resource_t> get(const std::string &id) const;
    /** @brief List workspaces using collection revision semantics. @param since_revision Last observed revision. @return Collection snapshot. */
    list_t list(std::optional<std::uint64_t> since_revision = std::nullopt) const;
    /** @brief Patch stopped workspace. @param id Workspace UUID. @param expected_revision Current revision. @param patch Replacements. @return Mutation result. */
    result_t patch(const std::string &id, std::uint64_t expected_revision, const patch_t &patch);
    /** @brief Delete stopped workspace. @param id Workspace UUID. @param expected_revision Current revision. @return Mutation result. */
    result_t remove(const std::string &id, std::uint64_t expected_revision);
    /** @brief Adopt orphaned workspace. @param id Workspace UUID. @param expected_revision Current revision. @param owner_client_uuid New owner UUID. @return Mutation result. */
    result_t adopt(const std::string &id, std::uint64_t expected_revision, const std::string &owner_client_uuid);
    /** @brief Prepare workspace runtime. @param id Workspace UUID. @param expected_revision Current revision. @param request Start options. @return Mutation result. */
    result_t start(const std::string &id, std::uint64_t expected_revision, const start_request_t &request = {});
    /**
     * @brief Associate prepared workspace with launched media session.
     *
     * @param id Workspace UUID.
     * @param expected_revision Current revision.
     * @param session_id Actual session UUID.
     * @return Mutation result.
     */
    result_t activate(const std::string &id, std::uint64_t expected_revision, const std::string &session_id);
    /** @brief Stop or disconnect workspace runtime. @param id Workspace UUID. @param expected_revision Current revision. @param terminate_application Whether application and resources must terminate. @return Mutation result. */
    result_t stop(const std::string &id, std::uint64_t expected_revision, bool terminate_application);
    /** @brief Revoke owner, stopping runtime and orphaning persistent definitions. @param owner_client_uuid Revoked owner UUID. @return Operation status. */
    status_t revoke_owner(const std::string &owner_client_uuid);
    /** @brief Revoke only owned workspaces absent from current authorization. @param owner_client_uuid Owner UUID. @param authorized_ids Workspace UUIDs owner may retain. @param expected_collection_revision Revision used to build authorized IDs, or empty for unconditional owner revocation. @return Operation status. */
    status_t revoke_unauthorized(const std::string &owner_client_uuid, const std::vector<std::string> &authorized_ids, std::optional<std::uint64_t> expected_collection_revision = std::nullopt);

  private:
    struct impl_t;  ///< Hidden implementation.
    std::unique_ptr<impl_t> impl_;  ///< Owned implementation.
  };

  /** @brief Serialize workspace resource contract. @param resource Workspace snapshot. @return JSON resource. */
  nlohmann::json to_json(const resource_t &resource);
}  // namespace terra_workspaces
