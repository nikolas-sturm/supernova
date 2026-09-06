/**
 * @file src/eclipse_sandboxes.h
 * @brief Standalone Eclipse sandbox policy and lifecycle core.
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
 * @brief Eclipse sandbox policy validation and lifecycle management.
 */
namespace eclipse_sandboxes {
  /**
   * @brief Sandbox lifecycle state.
   */
  enum class state_t {
    created,  ///< Definition exists but has not started.
    starting,  ///< Provider launch is in progress.
    running,  ///< Provider reports a live sandbox process.
    stopping,  ///< Provider termination is in progress.
    stopped,  ///< Sandbox process terminated normally.
    failed,  ///< Lifecycle operation or process failed.
    deleting,  ///< Resource deletion is in progress.
  };

  /**
   * @brief Stable error returned on a sandbox resource.
   */
  struct error_t {
    std::string code;  ///< Machine-readable error code.
    std::string message;  ///< Non-sensitive diagnostic message.
  };

  /**
   * @brief Application resolved immediately before provider launch.
   */
  struct application_t {
    std::string app_uuid;  ///< Canonical application UUID.
    std::optional<std::string> launch_profile_id;  ///< Selected canonical launch-profile UUID.
    std::string executable;  ///< Provider-only executable identity or path.
    nlohmann::json launch_data;  ///< Provider-only launch metadata.
  };

  /**
   * @brief Input supplied to provider capability and launch callbacks.
   */
  struct launch_request_t {
    std::string sandbox_id;  ///< Canonical sandbox UUID.
    application_t application;  ///< Resolved application.
    nlohmann::json effective_policy;  ///< Complete enforceable policy.
    std::optional<std::string> workspace_id;  ///< Associated workspace UUID.
    bool persistent;  ///< Whether definition survives owner loss and restart.
  };

  /**
   * @brief Runtime resources returned by a successful provider launch.
   */
  struct launch_result_t {
    std::string runtime_id;  ///< Opaque provider runtime identifier.
    std::optional<std::string> session_id;  ///< Canonical associated session UUID.
    std::vector<std::string> display_ids;  ///< Canonical display resource UUIDs.
    std::vector<std::string> peripheral_claim_ids;  ///< Canonical peripheral claim UUIDs.
  };

  /**
   * @brief Provider termination result.
   */
  struct termination_t {
    bool terminated;  ///< Whether all sandbox processes are terminated.
    std::optional<int> exit_code;  ///< Process exit code when available.
    std::optional<error_t> error;  ///< Provider error when termination failed.
  };

  /**
   * @brief Runtime state observed during host restart reconciliation.
   */
  struct reconciliation_t {
    enum class status_t {
      running,  ///< Runtime remains live and controlled.
      exited,  ///< Runtime exited and has an exit code when available.
      missing,  ///< Runtime no longer exists.
      error,  ///< Runtime state cannot be established safely.
    };

    status_t status;  ///< Observed provider status.
    std::optional<int> exit_code;  ///< Exit code for exited runtime.
    std::optional<error_t> error;  ///< Provider diagnostic for error status.
  };

  /**
   * @brief Published sandbox resource.
   */
  struct resource_t {
    std::string id;  ///< Canonical lowercase sandbox UUID.
    std::string name;  ///< User-visible sandbox name.
    std::optional<std::string> owner_client_uuid;  ///< Canonical owner UUID, or no owner when orphaned.
    std::string profile_id;  ///< Canonical sandbox-profile UUID.
    std::optional<std::string> workspace_id;  ///< Canonical workspace UUID.
    std::optional<std::string> app_uuid;  ///< Canonical configured application UUID.
    std::optional<std::string> session_id;  ///< Canonical active session UUID.
    bool persistent;  ///< Whether definition survives owner loss and restart.
    state_t state;  ///< Current lifecycle state.
    nlohmann::json effective_policy;  ///< Complete normalized provider-enforceable policy.
    std::vector<std::string> display_ids;  ///< Active display resource UUIDs.
    std::vector<std::string> peripheral_claim_ids;  ///< Active peripheral claim UUIDs.
    std::int64_t created_at;  ///< Creation Unix time in milliseconds.
    std::optional<std::int64_t> started_at;  ///< Most recent successful start time.
    std::int64_t updated_at;  ///< Last published mutation time.
    std::optional<std::int64_t> stopped_at;  ///< Most recent stop time.
    std::optional<int> exit_code;  ///< Most recent process exit code.
    std::optional<error_t> error;  ///< Current lifecycle error.
    std::uint64_t revision;  ///< Monotonic resource revision.
    std::optional<std::string> runtime_id;  ///< Internal opaque provider runtime identifier.
  };

  /**
   * @brief Sandbox creation fields.
   */
  struct create_t {
    std::string profile_id;  ///< Canonical sandbox-profile UUID.
    std::optional<std::string> workspace_id;  ///< Optional canonical workspace UUID.
    std::optional<std::string> app_uuid;  ///< Optional canonical application UUID.
    bool persistent;  ///< Whether definition survives owner loss and restart.
    std::string name;  ///< User-visible name.
    nlohmann::json profile_configuration;  ///< Partial or complete sandbox-profile configuration.
  };

  /**
   * @brief Optional start overrides.
   */
  struct start_t {
    std::optional<std::string> app_uuid;  ///< Application UUID used for this and future starts.
    std::optional<std::string> launch_profile_id;  ///< Optional launch-profile UUID.
    std::optional<nlohmann::json> launch_data;  ///< Optional validated provider launch metadata override.
  };

  /**
   * @brief Caller-provided storage and platform operations.
   *
   * Callbacks are serialized under the manager lock and MUST NOT call back into
   * the same manager instance.
   */
  struct callbacks_t {
    std::function<std::optional<std::string>()> load;  ///< Load complete persisted JSON document.
    std::function<bool(const std::string &)> save;  ///< Atomically replace complete persisted JSON document.
    std::function<std::int64_t()> now;  ///< Return current Unix time in milliseconds.
    std::function<std::string()> uuid;  ///< Return a new canonical UUID.
    std::function<std::optional<application_t>(const std::string &, const std::optional<std::string> &)> resolve_application;  ///< Resolve authorized application and launch profile.
    std::function<bool(const launch_request_t &, std::string &)> provider_capable;  ///< Confirm every effective policy field is enforceable and return a safe reason on failure.
    std::function<std::optional<launch_result_t>(const launch_request_t &, error_t &)> launch;  ///< Establish isolation and launch application atomically.
    std::function<termination_t(const std::string &, bool)> terminate;  ///< Terminate all processes associated with runtime ID.
    std::function<reconciliation_t(const std::string &)> reconcile;  ///< Observe persisted runtime during manager construction.
    std::function<bool(const std::string &, const nlohmann::json &)> cleanup;  ///< Release ephemeral provider resources according to effective cleanup policy.
    std::function<void(const std::optional<resource_t> &, const std::optional<resource_t> &)> on_change;  ///< Observe each published resource transition.
  };

  /**
   * @brief Lifecycle operation result category.
   */
  enum class status_t {
    success,  ///< Operation completed and persisted.
    not_found,  ///< Sandbox does not exist.
    invalid,  ///< Input or persisted data failed validation.
    unsupported_configuration,  ///< Provider cannot enforce requested policy.
    conflict,  ///< State or revision precondition conflicts.
    application_not_found,  ///< Application or launch profile did not resolve before launch.
    provider_error,  ///< Provider lifecycle operation failed.
    persistence_error,  ///< Atomic persistence rejected candidate state.
    unavailable,  ///< Manager failed closed during initialization or rollback.
  };

  /**
   * @brief Lifecycle mutation output.
   */
  struct result_t {
    status_t status;  ///< Operation outcome.
    std::optional<resource_t> resource;  ///< Resulting resource snapshot.
  };

  /**
   * @brief Collection listing output.
   */
  struct list_t {
    std::uint64_t revision;  ///< Current collection revision.
    bool changed;  ///< Whether collection differs from requested revision.
    std::vector<resource_t> resources;  ///< Full snapshot, empty when unchanged.
  };

  /**
   * @brief Normalize and strictly validate sandbox-profile configuration.
   *
   * Missing categories receive contract deny-by-default values. Unknown fields,
   * malformed nested values, duplicate identifiers, and out-of-range values fail.
   *
   * @param configuration Candidate profile configuration.
   * @param effective Receives complete normalized policy on success.
   * @return True when configuration is structurally valid.
   */
  bool normalize_policy(const nlohmann::json &configuration, nlohmann::json &effective);

  /**
   * @brief Thread-safe persisted sandbox lifecycle manager.
   */
  class manager_t {
  public:
    /**
     * @brief Construct manager and reconcile persisted resources.
     *
     * @param callbacks Persistence and provider operations.
     */
    explicit manager_t(callbacks_t callbacks);
    /** @brief Destroy manager implementation. */
    ~manager_t();
    manager_t(const manager_t &) = delete;  ///< Copying synchronized manager is unsupported.
    manager_t &operator=(const manager_t &) = delete;  ///< Copy assignment is unsupported.
    manager_t(manager_t &&) = delete;  ///< Moving synchronized manager is unsupported.
    manager_t &operator=(manager_t &&) = delete;  ///< Move assignment is unsupported.

    /** @brief Return true when initialization and reconciliation succeeded. */
    bool available() const;
    /**
     * @brief Create persisted sandbox definition.
     * @param owner_client_uuid Canonical owner UUID.
     * @param request Creation fields and profile configuration.
     * @return Mutation result.
     */
    result_t create(const std::string &owner_client_uuid, const create_t &request);
    /**
     * @brief Get sandbox snapshot.
     * @param id Canonical sandbox UUID.
     * @return Resource or no value.
     */
    std::optional<resource_t> get(const std::string &id) const;
    /**
     * @brief List full collection unless caller already has current revision.
     * @param since_revision Optional observed collection revision.
     * @return Collection snapshot semantics.
     */
    list_t list(std::optional<std::uint64_t> since_revision = std::nullopt) const;
    /**
     * @brief Resolve application, verify capabilities, and launch sandbox.
     * @param id Sandbox UUID.
     * @param expected_revision Required resource revision.
     * @param request Optional application and launch-profile overrides.
     * @return Mutation result.
     */
    result_t start(const std::string &id, std::uint64_t expected_revision, const start_t &request = {});
    /**
     * @brief Terminate sandbox and release runtime resources.
     * @param id Sandbox UUID.
     * @param expected_revision Required resource revision.
     * @param force Whether provider should force termination.
     * @return Mutation result.
     */
    result_t stop(const std::string &id, std::uint64_t expected_revision, bool force);
    /**
     * @brief Stop and start sandbox after reconciling termination.
     * @param id Sandbox UUID.
     * @param expected_revision Required resource revision.
     * @param force Whether provider should force termination.
     * @param request Optional start overrides.
     * @return Mutation result.
     */
    result_t restart(const std::string &id, std::uint64_t expected_revision, bool force, const start_t &request = {});
    /**
     * @brief Delete sandbox after terminating runtime and cleaning resources.
     * @param id Sandbox UUID.
     * @param expected_revision Required resource revision.
     * @return Mutation result containing deleted snapshot.
     */
    result_t remove(const std::string &id, std::uint64_t expected_revision);
    /**
     * @brief Adopt orphaned persistent sandbox.
     * @param id Sandbox UUID.
     * @param expected_revision Required resource revision.
     * @param owner_client_uuid Canonical new owner UUID.
     * @return Mutation result.
     */
    result_t adopt(const std::string &id, std::uint64_t expected_revision, const std::string &owner_client_uuid);
    /**
     * @brief Reconcile running resources against current provider state.
     * @return Aggregate status after persisting observed exits or failures.
     */
    status_t reconcile();
    /**
     * @brief Orphan persistent definitions and remove ephemeral definitions for revoked owner.
     * @param owner_client_uuid Canonical revoked owner UUID.
     * @return Aggregate operation status.
     */
    status_t revoke_owner(const std::string &owner_client_uuid);

  private:
    struct impl_t;  ///< Hidden synchronized implementation.
    std::unique_ptr<impl_t> impl_;  ///< Owned implementation.
  };

  /**
   * @brief Serialize public sandbox contract without provider runtime ID.
   * @param resource Sandbox resource.
   * @return Eclipse sandbox JSON object.
   */
  nlohmann::json to_json(const resource_t &resource);
}  // namespace eclipse_sandboxes
