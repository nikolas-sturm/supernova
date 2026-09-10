/**
 * @file src/terra_virtual_display.h
 * @brief Platform-independent Terra virtual display lifecycle management.
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
 * @brief Terra virtual display lifecycle services.
 */
namespace terra_virtual_display {
  /**
   * @brief Requested display timing and format.
   */
  struct mode_t {
    int width;  ///< Pixel width.
    int height;  ///< Pixel height.
    std::uint32_t refresh_numerator;  ///< Refresh-rate numerator.
    std::uint32_t refresh_denominator;  ///< Refresh-rate denominator.
    int bit_depth;  ///< Bits per color component.
    bool hdr;  ///< Whether HDR output is requested.
  };

  /**
   * @brief Applied display timing and stable mode identity.
   */
  struct actual_mode_t {
    int width;  ///< Applied pixel width.
    int height;  ///< Applied pixel height.
    std::uint32_t refresh_numerator;  ///< Applied refresh-rate numerator.
    std::uint32_t refresh_denominator;  ///< Applied refresh-rate denominator.
    int bit_depth;  ///< Applied bits per color component.
    bool hdr;  ///< Whether HDR output was applied.
    std::string id;  ///< Stable platform mode identifier.
  };

  /**
   * @brief Desktop coordinate.
   */
  struct position_t {
    int x;  ///< Horizontal desktop coordinate.
    int y;  ///< Vertical desktop coordinate.
  };

  /**
   * @brief Virtual display lifecycle state.
   */
  enum class state_t {
    provisioning,  ///< Provider connector is being provisioned.
    ready,  ///< Connector exists and is not attached.
    attached,  ///< Connector is attached to a streaming session.
    error,  ///< Connector encountered an error.
    deleting,  ///< Connector is being removed.
  };

  /**
   * @brief Published virtual display resource.
   */
  struct resource_t {
    std::string id;  ///< Stable canonical resource UUID.
    std::string name;  ///< User-visible display name.
    std::optional<std::string> owner_client_uuid;  ///< Canonical owner client UUID, or no value when orphaned.
    state_t state;  ///< Current lifecycle state.
    mode_t requested_mode;  ///< Requested mode.
    actual_mode_t actual_mode;  ///< Provider-confirmed mode.
    position_t position;  ///< Desktop position.
    double scale;  ///< Desktop scale factor.
    int rotation;  ///< Clockwise rotation in degrees.
    bool primary;  ///< Whether display is primary.
    bool hdr;  ///< Whether HDR is enabled.
    bool persistent;  ///< Whether resource survives process restart.
    std::optional<std::string> workspace_id;  ///< Canonical workspace UUID.
    std::optional<std::string> session_id;  ///< Canonical attached session UUID.
    nlohmann::json error;  ///< Error object, or JSON null.
    std::uint64_t revision;  ///< Resource revision.
    std::string platform_id;  ///< Internal stable provider connector identifier.
  };

  /**
   * @brief Mutable resource fields used by create and patch.
   */
  struct specification_t {
    std::string name;  ///< User-visible display name.
    mode_t mode;  ///< Requested mode.
    position_t position;  ///< Desktop position.
    double scale;  ///< Desktop scale factor.
    int rotation;  ///< Clockwise rotation in degrees.
    bool primary;  ///< Whether display is primary.
    bool hdr;  ///< Whether HDR is enabled.
    bool persistent;  ///< Whether resource survives process restart.
    std::optional<std::string> workspace_id;  ///< Canonical workspace UUID.
  };

  /**
   * @brief Optional patch fields.
   */
  struct patch_t {
    std::optional<std::string> name;  ///< Replacement display name.
    std::optional<mode_t> mode;  ///< Replacement requested mode.
    std::optional<position_t> position;  ///< Replacement desktop position.
    std::optional<double> scale;  ///< Replacement scale factor.
    std::optional<int> rotation;  ///< Replacement rotation.
    std::optional<bool> primary;  ///< Replacement primary flag.
    std::optional<bool> hdr;  ///< Replacement HDR flag.
    std::optional<bool> persistent;  ///< Replacement persistence flag.
    std::optional<std::optional<std::string>> workspace_id;  ///< Replacement or cleared workspace UUID.
  };

  /**
   * @brief Exclusive runtime attachment target.
   */
  struct attachment_t {
    std::optional<std::string> session_id;  ///< Canonical session UUID.
    std::optional<std::string> workspace_id;  ///< Canonical workspace UUID.
  };

  /**
   * @brief Provider configuration input and output.
   */
  struct platform_configuration_t {
    std::string platform_id;  ///< Stable connector identifier.
    specification_t specification;  ///< Desired connector configuration.
    actual_mode_t actual_mode;  ///< Applied mode returned by provider.
  };

  /**
   * @brief Caller-provided persistence, identity, and provider operations.
   */
  struct callbacks_t {
    std::function<std::optional<std::string>()> load;  ///< Load persisted JSON document.
    std::function<bool(const std::string &)> save;  ///< Atomically save complete JSON document.
    std::function<std::int64_t()> now;  ///< Return current Unix time in milliseconds.
    std::function<std::string()> uuid;  ///< Return a new canonical UUID.
    std::function<bool()> provider_healthy;  ///< Report whether provider is usable.
    std::function<std::optional<std::uint32_t>()> read_count;  ///< Read global MttVDD connector count.
    std::function<bool(std::uint32_t)> set_count;  ///< Set global MttVDD connector count and await completion.
    std::function<std::optional<std::vector<std::string>>()> inventory;  ///< Read stable provider connector identifiers.
    std::function<bool(std::vector<platform_configuration_t> &)> apply_configuration;  ///< Apply configurations and populate actual modes.
    std::uint32_t max_count {};  ///< Maximum global connector count supported by provider.
    std::function<void(const std::optional<resource_t> &, const std::optional<resource_t> &)> changed;  ///< Observe committed create, update, reconciliation, and removal changes.
    std::function<bool()> capture_configuration;  ///< Capture exact host topology before mutation.
    std::function<bool()> restore_configuration;  ///< Restore topology captured before failed mutation.
  };

  /**
   * @brief Mutation result category.
   */
  enum class status_t {
    success,  ///< Mutation completed and was published.
    not_found,  ///< Resource does not exist.
    invalid,  ///< Input failed validation.
    conflict,  ///< State or revision precondition conflicts.
    limit_reached,  ///< Provider connector capacity is exhausted.
    provider_error,  ///< Provider health, count, inventory, or apply operation failed.
    persistence_error,  ///< Candidate document could not be saved.
    unavailable,  ///< Manager failed closed during initialization or reconciliation.
  };

  /**
   * @brief Mutation output.
   */
  struct result_t {
    status_t status;  ///< Mutation result.
    std::optional<resource_t> resource;  ///< Created or updated resource.
  };

  /**
   * @brief Atomic multi-display mutation output.
   */
  struct batch_result_t {
    status_t status;  ///< Mutation result.
    std::vector<resource_t> resources;  ///< Created resources in specification order.
  };

  /**
   * @brief Collection listing output.
   */
  struct list_t {
    std::uint64_t revision;  ///< Current collection revision.
    bool changed;  ///< Whether collection changed after requested revision.
    std::vector<resource_t> resources;  ///< Snapshot, empty when unchanged.
  };

  /**
   * @brief Thread-safe virtual display lifecycle manager.
   */
  class manager_t {
  public:
    /**
     * @brief Construct manager and reconcile persisted/provider state.
     *
     * @param callbacks Injected platform operations.
     */
    explicit manager_t(callbacks_t callbacks);
    /** @brief Destroy manager implementation. */
    ~manager_t();
    manager_t(const manager_t &) = delete;  ///< Copying synchronized manager is unsupported.
    manager_t &operator=(const manager_t &) = delete;  ///< Copy assignment is unsupported.
    manager_t(manager_t &&) = delete;  ///< Moving synchronized manager is unsupported.
    manager_t &operator=(manager_t &&) = delete;  ///< Move assignment is unsupported.

    /**
     * @brief Report whether initialization and reconciliation succeeded.
     *
     * @return True when mutations are available.
     */
    bool available() const;
    /**
     * @brief Return remaining provider capacity above immutable baseline.
     *
     * @return Maximum additional managed virtual displays.
     */
    std::uint32_t max_active() const;
    /**
     * @brief Create one managed connector above immutable baseline.
     *
     * @param owner_client_uuid Canonical owner UUID.
     * @param specification Desired display fields.
     * @return Mutation result.
     */
    result_t create(const std::string &owner_client_uuid, const specification_t &specification);
    /**
     * @brief Create one complete workspace display layout atomically.
     *
     * @param owner_client_uuid Canonical owner UUID.
     * @param specifications One to four desired displays.
     * @return Batch mutation result with no resources on failure.
     */
    batch_result_t create_batch(const std::string &owner_client_uuid, const std::vector<specification_t> &specifications);
    /**
     * @brief Find resource.
     *
     * @param id Canonical resource UUID.
     * @return Resource snapshot, or no value.
     */
    std::optional<resource_t> get(const std::string &id) const;
    /**
     * @brief List resources changed after collection revision.
     *
     * @param since_revision Optional last observed collection revision.
     * @return Collection revision and snapshot semantics.
     */
    list_t list(std::optional<std::uint64_t> since_revision = std::nullopt) const;
    /**
     * @brief Patch resource under revision precondition.
     *
     * @param id Resource UUID.
     * @param expected_revision Required current resource revision.
     * @param patch Replacement fields.
     * @return Mutation result.
     */
    result_t patch(const std::string &id, std::uint64_t expected_revision, const patch_t &patch);
    /**
     * @brief Delete unattached resource under revision precondition.
     *
     * @param id Resource UUID.
     * @param expected_revision Required current resource revision.
     * @return Mutation result.
     */
    result_t remove(const std::string &id, std::uint64_t expected_revision);
    /**
     * @brief Attach resource to exactly one session or workspace.
     *
     * @param id Resource UUID.
     * @param expected_revision Required current resource revision.
     * @param attachment Exclusive attachment target.
     * @return Mutation result.
     */
    result_t attach(const std::string &id, std::uint64_t expected_revision, const attachment_t &attachment);
    /**
     * @brief Detach resource from current session.
     *
     * @param id Resource UUID.
     * @param expected_revision Required current resource revision.
     * @return Mutation result.
     */
    result_t detach(const std::string &id, std::uint64_t expected_revision);
    /**
     * @brief Adopt an orphaned resource.
     *
     * @param id Orphaned resource UUID.
     * @param expected_revision Required current resource revision.
     * @param owner_client_uuid Canonical new owner UUID.
     * @return Mutation result.
     */
    result_t adopt(const std::string &id, std::uint64_t expected_revision, const std::string &owner_client_uuid);

    /**
     * @brief Orphan persistent resources and delete ephemeral resources for a revoked owner.
     *
     * Attached resources are detached before ownership cleanup. Provider and persistence
     * failures leave published ownership unchanged.
     *
     * @param owner_client_uuid Canonical revoked owner UUID.
     * @return Success, provider error, persistence error, or unavailable.
     */
    status_t revoke_owner(const std::string &owner_client_uuid);

  private:
    struct impl_t;  ///< Hidden implementation.
    std::unique_ptr<impl_t> impl_;  ///< Owned implementation.
  };

  /**
   * @brief Serialize resource contract without internal platform ID.
   *
   * @param resource Resource snapshot.
   * @return Terra JSON resource object.
   */
  nlohmann::json to_json(const resource_t &resource);
}  // namespace terra_virtual_display
