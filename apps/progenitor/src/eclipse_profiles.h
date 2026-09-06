/**
 * @file eclipse_profiles.h
 * @brief Standalone storage and validation for Eclipse profiles-v1 resources.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace eclipse::profiles {
  /** @brief Result status for profile operations. */
  enum class status_t {
    success,
    invalid,
    unsupported,
    not_found,
    forbidden,
    resource_busy,
    conflict,
    persistence,
    unavailable,
  };

  /** @brief Calling-client authorization and visibility context. */
  struct actor_t {
    std::string client_uuid;  ///< Canonical calling-client UUID.
    std::set<std::string> scopes;  ///< Granted Eclipse scopes.
    std::set<std::string> allowed_apps;  ///< Empty permits every application.
  };

  /** @brief Stored profiles-v1 resource. */
  struct profile_t {
    std::string id;  ///< Stable lowercase canonical UUID.
    std::string type;  ///< `display`, `stream`, `launch`, or `sandbox`.
    std::string name;  ///< User-visible profile name.
    std::optional<std::string> owner_client_uuid;  ///< Owner, or null for orphan.
    bool shared {};  ///< Whether other authorized clients can read this profile.
    std::uint64_t revision {};  ///< Per-resource revision.
    nlohmann::json configuration;  ///< Validated complete configuration.
  };

  /** @brief Operation result and optional returned resources. */
  struct result_t {
    status_t status {status_t::success};  ///< Machine-usable outcome.
    std::string message;  ///< Diagnostic without secret material.
    std::optional<profile_t> profile;  ///< Item result when applicable.
    std::vector<profile_t> profiles;  ///< List result when applicable.
    std::uint64_t collection_revision {};  ///< Current collection revision.
  };

  /** @brief Injected host services used by the standalone manager. */
  struct callbacks_t {
    std::function<std::optional<std::string>()> load;  ///< Loads persisted JSON; null means no document.
    std::function<bool(const std::string &)> save;  ///< Atomically saves complete JSON document.
    std::function<std::uint64_t()> clock;  ///< Supplies audit/event time for integrations.
    std::function<std::string()> uuid;  ///< Supplies new canonical UUIDs.
    std::function<status_t(const std::string &, const std::string &)> validate_reference;  ///< Validates `(kind, UUID)` references.
    /**
     * @brief Validate host-specific configuration before publication.
     *
     * This callback MUST reject unsupported combinations and unsafe host resources. In particular,
     * display profiles require validation of `topology` and every `virtualDisplays` entry, while
     * launch profiles require administrator-root checks for `workingDirectory` and paths or
     * commands referenced by pre-launch and post-exit actions. Returning false fails closed with
     * `unsupported`; manager never publishes or persists rejected configuration.
     */
    std::function<bool(const std::string &, const nlohmann::json &)> provider_supports;
    std::function<bool(const profile_t &)> reference_busy;  ///< Reports external references before deletion.
  };

  /** @brief Thread-safe profiles-v1 resource manager. */
  class manager_t {
  public:
    /**
     * @brief Construct manager and recover valid persisted records.
     * @param callbacks Injected persistence and host services.
     */
    explicit manager_t(callbacks_t callbacks);
    ~manager_t();
    manager_t(manager_t &&) noexcept;
    manager_t &operator=(manager_t &&) noexcept;
    manager_t(const manager_t &) = delete;
    manager_t &operator=(const manager_t &) = delete;

    /** @brief Create profile from exact creation object. */
    result_t create(const actor_t &actor, const nlohmann::json &request);
    /** @brief Read visible profile by canonical UUID. */
    result_t get(const actor_t &actor, const std::string &id) const;
    /** @brief List readable profiles, optionally filtering by type. */
    result_t list(const actor_t &actor, const std::optional<std::string> &type = std::nullopt) const;
    /**
     * @brief Return stored profile type for request authorization.
     *
     * @param id Canonical profile UUID.
     * @return Stored type, or no value when profile is absent.
     */
    std::optional<std::string> type(const std::string &id) const;
    /**
     * @brief Inspect profile without applying caller visibility.
     *
     * Intended for request handlers after domain-scope authorization.
     *
     * @param id Canonical profile UUID.
     * @return Stored profile, or no value when absent.
     */
    std::optional<profile_t> inspect(const std::string &id) const;

    /**
     * @brief Scan every stored launch profile for a profile reference.
     *
     * Bypasses actor visibility because deletion integrity is host-wide.
     *
     * @param id Referenced profile UUID.
     * @return True when any launch profile's configuration references `id`.
     */
    bool launch_profile_references(const std::string &id) const;
    /**
     * @brief Validate complete type-specific configuration against host support.
     *
     * @param type Profile type.
     * @param configuration Complete configuration object.
     * @return Validation or provider-support status.
     */
    status_t validate_configuration(const std::string &type, const nlohmann::json &configuration) const;
    /** @brief Replace supplied mutable fields using optimistic revision. */
    result_t patch(const actor_t &actor, const std::string &id, std::uint64_t expected_revision, const nlohmann::json &request);
    /** @brief Delete profile using optimistic revision. */
    result_t erase(const actor_t &actor, const std::string &id, std::uint64_t expected_revision);
    /** @brief Adopt orphaned profile using optimistic revision. */
    result_t adopt(const actor_t &actor, const std::string &id, std::uint64_t expected_revision);
    /**
     * @brief Orphan every profile owned by one removed or disabled client.
     *
     * @param owner Canonical owner UUID.
     * @return Changed profiles and resulting collection revision.
     */
    result_t revoke_owner(const std::string &owner);
    /**
     * @brief Orphan owned profiles no longer authorized by current client policy.
     *
     * @param actor Current owner authorization and allowlist.
     * @return Changed profiles and resulting collection revision.
     */
    result_t revoke_unauthorized(const actor_t &actor);
    /** @brief Return startup availability; malformed whole documents are unavailable. */
    status_t availability() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  /** @brief Serialize profile using protocol field names. */
  nlohmann::json to_json(const profile_t &profile);
}  // namespace eclipse::profiles
