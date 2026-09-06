/**
 * @file src/eclipse_workspaces.cpp
 * @brief Standalone Eclipse workspace definition and lifecycle implementation.
 */

// standard includes
#include <algorithm>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>

// local includes
#include "eclipse_workspaces.h"
#include "utility.h"
#include "uuid.h"

namespace eclipse_workspaces {
  namespace {
    constexpr int DOCUMENT_VERSION = 1;  ///< Persistence schema version.

    /** @brief Validate lowercase canonical UUID. @param value Candidate text. @return True when valid. */
    bool valid_uuid(const std::string &value) {
      return uuid_util::is_valid(value) && std::none_of(value.begin(), value.end(), [](const unsigned char character) {
               return character >= 'A' && character <= 'F';
             });
    }

    /** @brief Convert state to stable text. @param state State value. @return State name. */
    std::string state_name(const state_t state) {
      switch (state) {
        case state_t::stopped:
          return "stopped";
        case state_t::preparing:
          return "preparing";
        case state_t::ready:
          return "ready";
        case state_t::active:
          return "active";
        case state_t::stopping:
          return "stopping";
        case state_t::failed:
          return "failed";
      }
      return {};
    }

    /** @brief Parse stable state text. @param value State name. @return Parsed state. */
    std::optional<state_t> parse_state(const std::string &value) {
      static const std::map<std::string, state_t, std::less<>> values {{"stopped", state_t::stopped}, {"preparing", state_t::preparing}, {"ready", state_t::ready}, {"active", state_t::active}, {"stopping", state_t::stopping}, {"failed", state_t::failed}};
      const auto found = values.find(value);
      return found == values.end() ? std::nullopt : std::optional {found->second};
    }

    /** @brief Convert cleanup policy to text. @param policy Policy value. @return Policy name. */
    std::string cleanup_name(const cleanup_policy_t policy) {
      switch (policy) {
        case cleanup_policy_t::on_disconnect:
          return "on-disconnect";
        case cleanup_policy_t::on_stop:
          return "on-stop";
        case cleanup_policy_t::retain:
          return "retain";
      }
      return {};
    }

    /** @brief Parse cleanup policy. @param value Policy name. @return Parsed policy. */
    std::optional<cleanup_policy_t> parse_cleanup(const std::string &value) {
      if (value == "on-disconnect") {
        return cleanup_policy_t::on_disconnect;
      }
      if (value == "on-stop") {
        return cleanup_policy_t::on_stop;
      }
      if (value == "retain") {
        return cleanup_policy_t::retain;
      }
      return std::nullopt;
    }

    /** @brief Validate UUID array without duplicates. @param values UUID values. @return True when valid. */
    bool valid_uuids(const std::vector<std::string> &values) {
      return std::all_of(values.begin(), values.end(), valid_uuid) && std::set<std::string>(values.begin(), values.end()).size() == values.size();
    }

    /** @brief Require an object to contain exactly expected keys. @param value Candidate object. @param expected Expected key names. @return True on exact match. */
    bool exact_keys(const nlohmann::json &value, const std::set<std::string, std::less<>> &expected) {
      if (!value.is_object() || value.size() != expected.size()) {
        return false;
      }
      for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (!expected.contains(iterator.key())) {
          return false;
        }
      }
      return true;
    }

    /** @brief Serialize definition fields. @param value Definition. @return JSON fields. */
    nlohmann::json definition_json(const definition_t &value) {
      return {
        {"name", value.name},
        {"description", value.description},
        {"shared", value.shared},
        {"desktopAppUuid", value.desktop_app_uuid},
        {"permittedAppUuids", value.permitted_app_uuids},
        {"displayProfileId", value.display_profile_id ? nlohmann::json(*value.display_profile_id) : nlohmann::json(nullptr)},
        {"streamProfileId", value.stream_profile_id ? nlohmann::json(*value.stream_profile_id) : nlohmann::json(nullptr)},
        {"launchProfileId", value.launch_profile_id ? nlohmann::json(*value.launch_profile_id) : nlohmann::json(nullptr)},
        {"sandboxProfileId", value.sandbox_profile_id ? nlohmann::json(*value.sandbox_profile_id) : nlohmann::json(nullptr)},
        {"virtualDisplays", value.virtual_displays},
        {"peripheralPolicy", {{"requiredDeviceIds", value.peripheral_policy.required_device_ids}, {"requiredClasses", value.peripheral_policy.required_classes}, {"disconnectPolicy", value.peripheral_policy.disconnect_policy}}},
        {"persistent", value.persistent},
        {"cleanupPolicy", cleanup_name(value.cleanup_policy)},
      };
    }

    /** @brief Parse exact definition fields. @param value JSON object. @return Parsed definition. */
    definition_t definition_from_json(const nlohmann::json &value) {
      static const std::set<std::string, std::less<>> peripheral_keys {"disconnectPolicy", "requiredClasses", "requiredDeviceIds"};
      if (!exact_keys(value.at("peripheralPolicy"), peripheral_keys)) {
        throw std::invalid_argument("workspace definition keys");
      }
      const auto cleanup = parse_cleanup(value.at("cleanupPolicy").get<std::string>());
      if (!cleanup) {
        throw std::invalid_argument("cleanupPolicy");
      }
      definition_t result {
        value.at("name").get<std::string>(),
        value.at("description").get<std::string>(),
        value.at("shared").get<bool>(),
        value.at("desktopAppUuid").get<std::string>(),
        value.at("permittedAppUuids").get<std::vector<std::string>>(),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        value.at("virtualDisplays").get<std::vector<nlohmann::json>>(),
        {value.at("peripheralPolicy").at("requiredDeviceIds").get<std::vector<std::string>>(), value.at("peripheralPolicy").at("requiredClasses").get<std::vector<std::string>>(), value.at("peripheralPolicy").at("disconnectPolicy").get<std::string>()},
        value.at("persistent").get<bool>(),
        *cleanup,
      };
      const auto optional_uuid = [&](const char *name, std::optional<std::string> &target) {
        if (!value.at(name).is_null()) {
          target = value.at(name).get<std::string>();
        }
      };
      optional_uuid("displayProfileId", result.display_profile_id);
      optional_uuid("streamProfileId", result.stream_profile_id);
      optional_uuid("launchProfileId", result.launch_profile_id);
      optional_uuid("sandboxProfileId", result.sandbox_profile_id);
      return result;
    }

    /** @brief Return standard internal error object. @param code Stable code. @param message Human-readable message. @return Error object. */
    nlohmann::json error_json(const std::string &code, const std::string &message) {
      return {{"code", code}, {"message", message}};
    }

    /** @brief Clear all runtime associations. @param resource Workspace resource. */
    void clear_runtime(resource_t &resource) {
      resource.session_id.reset();
      resource.sandbox_id.reset();
      resource.display_ids.clear();
      resource.peripheral_claim_ids.clear();
    }

  }  // namespace

  std::optional<definition_t> parse_definition(const nlohmann::json &value) {
    static const std::set<std::string, std::less<>> keys {
      "cleanupPolicy",
      "description",
      "desktopAppUuid",
      "displayProfileId",
      "launchProfileId",
      "name",
      "peripheralPolicy",
      "permittedAppUuids",
      "persistent",
      "sandboxProfileId",
      "shared",
      "streamProfileId",
      "virtualDisplays",
    };
    if (!exact_keys(value, keys)) {
      return std::nullopt;
    }
    try {
      return definition_from_json(value);
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<patch_t> parse_patch(const nlohmann::json &value) {
    static const std::set<std::string, std::less<>> allowed {
      "cleanupPolicy",
      "description",
      "desktopAppUuid",
      "displayProfileId",
      "launchProfileId",
      "name",
      "peripheralPolicy",
      "permittedAppUuids",
      "persistent",
      "sandboxProfileId",
      "shared",
      "streamProfileId",
      "virtualDisplays",
    };
    if (!value.is_object() || value.empty() || std::ranges::any_of(value.items(), [&](const auto &item) {
          return !allowed.contains(item.key());
        })) {
      return std::nullopt;
    }
    try {
      patch_t result;
      if (value.contains("name")) {
        result.name = value.at("name").get<std::string>();
      }
      if (value.contains("description")) {
        result.description = value.at("description").get<std::string>();
      }
      if (value.contains("shared")) {
        result.shared = value.at("shared").get<bool>();
      }
      if (value.contains("desktopAppUuid")) {
        result.desktop_app_uuid = value.at("desktopAppUuid").get<std::string>();
      }
      if (value.contains("permittedAppUuids")) {
        result.permitted_app_uuids = value.at("permittedAppUuids").get<std::vector<std::string>>();
      }
      const auto optional_uuid = [&](const char *name, std::optional<std::optional<std::string>> &target) {
        if (!value.contains(name)) {
          return;
        }
        target = value.at(name).is_null() ? std::optional<std::string> {} : std::optional<std::string> {value.at(name).get<std::string>()};
      };
      optional_uuid("displayProfileId", result.display_profile_id);
      optional_uuid("streamProfileId", result.stream_profile_id);
      optional_uuid("launchProfileId", result.launch_profile_id);
      optional_uuid("sandboxProfileId", result.sandbox_profile_id);
      if (value.contains("virtualDisplays")) {
        result.virtual_displays = value.at("virtualDisplays").get<std::vector<nlohmann::json>>();
      }
      if (value.contains("peripheralPolicy")) {
        static const std::set<std::string, std::less<>> peripheral_keys {"disconnectPolicy", "requiredClasses", "requiredDeviceIds"};
        const auto &policy = value.at("peripheralPolicy");
        if (!exact_keys(policy, peripheral_keys)) {
          return std::nullopt;
        }
        result.peripheral_policy = peripheral_policy_t {
          policy.at("requiredDeviceIds").get<std::vector<std::string>>(),
          policy.at("requiredClasses").get<std::vector<std::string>>(),
          policy.at("disconnectPolicy").get<std::string>(),
        };
      }
      if (value.contains("persistent")) {
        result.persistent = value.at("persistent").get<bool>();
      }
      if (value.contains("cleanupPolicy")) {
        const auto policy = parse_cleanup(value.at("cleanupPolicy").get<std::string>());
        if (!policy) {
          return std::nullopt;
        }
        result.cleanup_policy = *policy;
      }
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<start_request_t> parse_start_request(const nlohmann::json &value) {
    static const std::set<std::string, std::less<>> allowed {"appUuid", "profileOverrides"};
    if (!value.is_object() || std::ranges::any_of(value.items(), [&](const auto &item) {
          return !allowed.contains(item.key());
        })) {
      return std::nullopt;
    }
    try {
      start_request_t result;
      if (value.contains("appUuid")) {
        result.app_uuid = value.at("appUuid").get<std::string>();
      }
      if (!value.contains("profileOverrides")) {
        return result;
      }
      const auto &overrides = value.at("profileOverrides");
      static const std::set<std::string, std::less<>> override_keys {"display", "launch", "sandbox", "stream"};
      if (!overrides.is_object() || std::ranges::any_of(overrides.items(), [&](const auto &item) {
            return !override_keys.contains(item.key()) || (!item.value().is_null() && !item.value().is_object());
          })) {
        return std::nullopt;
      }
      const auto set_override = [&](const char *name, std::optional<nlohmann::json> &target) {
        if (overrides.contains(name)) {
          target = overrides.at(name);
        }
      };
      set_override("display", result.profile_overrides.display);
      set_override("stream", result.profile_overrides.stream);
      set_override("launch", result.profile_overrides.launch);
      set_override("sandbox", result.profile_overrides.sandbox);
      return result;
    } catch (...) {
      return std::nullopt;
    }
  }

  struct manager_t::impl_t {
    callbacks_t callbacks;  ///< Injected host services.
    mutable std::mutex mutex;  ///< Serializes state and callbacks.
    bool usable = false;  ///< Initialization result.
    std::map<std::string, resource_t, std::less<>> resources;  ///< Published resources.
    std::uint64_t collection_revision = 0;  ///< Published collection revision.

    /** @brief Serialize candidate persistent state. @param values Candidate resources. @param revision Candidate collection revision. @return Persistence document. */
    nlohmann::json document(const decltype(resources) &values, const std::uint64_t revision) const {
      nlohmann::json records = nlohmann::json::array();
      for (const auto &[id, resource] : values) {
        if (resource.definition.persistent) {
          records.push_back(to_json(resource));
        }
      }
      return {{"version", DOCUMENT_VERSION}, {"collectionRevision", revision}, {"workspaces", std::move(records)}};
    }

    /** @brief Save candidate state. @param values Candidate resources. @param revision Candidate collection revision. @return True on atomic save. */
    bool save(const decltype(resources) &values, const std::uint64_t revision) const {
      try {
        return callbacks.save && callbacks.save(document(values, revision).dump());
      } catch (...) {
        return false;
      }
    }

    /** @brief Validate definition using injected policies. @param value Definition. @return True when valid. */
    bool validate(const definition_t &value) const {
      if (value.name.empty() || !valid_uuid(value.desktop_app_uuid) || !valid_uuids(value.permitted_app_uuids) || !valid_uuids(value.peripheral_policy.required_device_ids) || (value.peripheral_policy.disconnect_policy != "release" && value.peripheral_policy.disconnect_policy != "suspend")) {
        return false;
      }
      if (!callbacks.validate_app || !callbacks.validate_app(value.desktop_app_uuid) || std::any_of(value.permitted_app_uuids.begin(), value.permitted_app_uuids.end(), [&](const auto &id) {
            return !callbacks.validate_app(id);
          })) {
        return false;
      }
      const std::pair<const char *, const std::optional<std::string> *> profiles[] {{"display", &value.display_profile_id}, {"stream", &value.stream_profile_id}, {"launch", &value.launch_profile_id}, {"sandbox", &value.sandbox_profile_id}};
      for (const auto &[kind, id] : profiles) {
        if (*id && (!valid_uuid(**id) || !callbacks.validate_profile || !callbacks.validate_profile(kind, **id))) {
          return false;
        }
      }
      if (!callbacks.validate_virtual_display || std::any_of(value.virtual_displays.begin(), value.virtual_displays.end(), [&](const auto &display) {
            return !display.is_object() || display.contains("workspaceId") || !callbacks.validate_virtual_display(display);
          })) {
        return false;
      }
      return callbacks.validate_peripheral_policy && callbacks.validate_peripheral_policy(value.peripheral_policy);
    }

    /** @brief Publish candidate map after durable save. @param candidate Candidate resources. @return Mutation status. */
    status_t publish(decltype(resources) candidate) {
      const auto revision = collection_revision + 1;
      if (!save(candidate, revision)) {
        return status_t::persistence_error;
      }
      const auto previous = resources;
      resources = std::move(candidate);
      collection_revision = revision;
      if (callbacks.changed) {
        try {
          for (const auto &[id, resource] : previous) {
            if (!resources.contains(id)) {
              callbacks.changed(resource, std::nullopt);
            }
          }
          for (const auto &[id, resource] : resources) {
            const auto old = previous.find(id);
            if (old == previous.end()) {
              callbacks.changed(std::nullopt, resource);
            } else if (to_json(old->second) != to_json(resource)) {
              callbacks.changed(old->second, resource);
            }
          }
        } catch (...) {
        }
      }
      return status_t::success;
    }
  };

  manager_t::manager_t(callbacks_t callbacks):
      impl_(std::make_unique<impl_t>()) {
    impl_->callbacks = std::move(callbacks);
    try {
      const auto text = impl_->callbacks.load ? impl_->callbacks.load() : std::nullopt;
      if (!text) {
        impl_->usable = impl_->save(impl_->resources, 0);
        return;
      }
      const auto document = nlohmann::json::parse(*text);
      static const std::set<std::string, std::less<>> document_keys {"collectionRevision", "version", "workspaces"};
      if (!exact_keys(document, document_keys) || document.at("version") != DOCUMENT_VERSION || !document.at("workspaces").is_array()) {
        return;
      }
      impl_->collection_revision = document.at("collectionRevision").get<std::uint64_t>();
      bool reconciled = false;
      for (const auto &value : document.at("workspaces")) {
        try {
          static const std::set<std::string, std::less<>> resource_keys {
            "cleanupPolicy",
            "createdAt",
            "description",
            "desktopAppUuid",
            "displayIds",
            "displayProfileId",
            "error",
            "id",
            "launchProfileId",
            "name",
            "ownerClientUuid",
            "peripheralClaimIds",
            "peripheralPolicy",
            "permittedAppUuids",
            "persistent",
            "revision",
            "sandboxId",
            "sandboxProfileId",
            "sessionId",
            "shared",
            "state",
            "streamProfileId",
            "updatedAt",
            "virtualDisplays"
          };
          if (!exact_keys(value, resource_keys)) {
            continue;
          }
          const auto parsed_state = parse_state(value.at("state").get<std::string>());
          if (!parsed_state) {
            continue;
          }
          resource_t resource {value.at("id").get<std::string>(), std::nullopt, definition_from_json(value), *parsed_state, std::nullopt, std::nullopt, value.at("displayIds").get<std::vector<std::string>>(), value.at("peripheralClaimIds").get<std::vector<std::string>>(), value.at("createdAt").get<std::int64_t>(), value.at("updatedAt").get<std::int64_t>(), value.at("error"), value.at("revision").get<std::uint64_t>()};
          if (!value.at("ownerClientUuid").is_null()) {
            resource.owner_client_uuid = value.at("ownerClientUuid").get<std::string>();
          }
          if (!value.at("sessionId").is_null()) {
            resource.session_id = value.at("sessionId").get<std::string>();
          }
          if (!value.at("sandboxId").is_null()) {
            resource.sandbox_id = value.at("sandboxId").get<std::string>();
          }
          const bool ids_valid = valid_uuid(resource.id) && (!resource.owner_client_uuid || valid_uuid(*resource.owner_client_uuid)) && (!resource.session_id || valid_uuid(*resource.session_id)) && (!resource.sandbox_id || valid_uuid(*resource.sandbox_id)) && valid_uuids(resource.display_ids) && valid_uuids(resource.peripheral_claim_ids);
          if (!ids_valid || !resource.definition.persistent || resource.revision == 0 || resource.created_at < 0 || resource.updated_at < resource.created_at || !impl_->validate(resource.definition) || impl_->resources.contains(resource.id)) {
            continue;
          }
          if (resource.state == state_t::preparing || resource.state == state_t::stopping || ((resource.state == state_t::ready || resource.state == state_t::active) && (!impl_->callbacks.reconcile || !impl_->callbacks.reconcile(resource)))) {
            resource.state = state_t::failed;
            resource.error = error_json("runtime_unavailable", "Persisted workspace runtime could not be reconciled");
            clear_runtime(resource);
            resource.updated_at = impl_->callbacks.now();
            ++resource.revision;
            reconciled = true;
          }
          impl_->resources.emplace(resource.id, std::move(resource));
        } catch (...) {
          // Optional malformed records are isolated from valid definitions.
        }
      }
      if (reconciled) {
        ++impl_->collection_revision;
        if (!impl_->save(impl_->resources, impl_->collection_revision)) {
          return;
        }
      }
      impl_->usable = true;
    } catch (...) {
    }
  }

  manager_t::~manager_t() = default;

  bool manager_t::available() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->usable;
  }

  result_t manager_t::create(const std::string &owner_client_uuid, const definition_t &definition) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    if (!valid_uuid(owner_client_uuid) || !impl_->validate(definition)) {
      return {status_t::invalid, std::nullopt};
    }
    std::string id;
    try {
      id = impl_->callbacks.uuid();
    } catch (...) {
      return {status_t::invalid, std::nullopt};
    }
    if (!valid_uuid(id) || impl_->resources.contains(id)) {
      return {status_t::invalid, std::nullopt};
    }
    const auto now = impl_->callbacks.now();
    resource_t resource {id, owner_client_uuid, definition, state_t::stopped, std::nullopt, std::nullopt, {}, {}, now, now, nullptr, 1};
    auto candidate = impl_->resources;
    candidate.emplace(id, resource);
    const auto status = impl_->publish(std::move(candidate));
    return {status, status == status_t::success ? std::optional {resource} : std::nullopt};
  }

  std::optional<resource_t> manager_t::get(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->resources.find(id);
    return found == impl_->resources.end() ? std::nullopt : std::optional {found->second};
  }

  list_t manager_t::list(const std::optional<std::uint64_t> since_revision) const {
    std::lock_guard lock(impl_->mutex);
    const bool changed = !since_revision || *since_revision != impl_->collection_revision;
    std::vector<resource_t> values;
    if (changed) {
      for (const auto &[id, resource] : impl_->resources) {
        values.push_back(resource);
      }
    }
    return {impl_->collection_revision, changed, std::move(values)};
  }

  result_t manager_t::patch(const std::string &id, const std::uint64_t expected_revision, const patch_t &patch) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::stopped) {
      return {status_t::conflict, std::nullopt};
    }
    auto resource = found->second;
#define ECLIPSE_PATCH_FIELD(field) \
  if (patch.field) { \
    resource.definition.field = *patch.field; \
  }
    ECLIPSE_PATCH_FIELD(name)
    ECLIPSE_PATCH_FIELD(description)
    ECLIPSE_PATCH_FIELD(shared)
    ECLIPSE_PATCH_FIELD(desktop_app_uuid)
    ECLIPSE_PATCH_FIELD(permitted_app_uuids)
    ECLIPSE_PATCH_FIELD(display_profile_id)
    ECLIPSE_PATCH_FIELD(stream_profile_id)
    ECLIPSE_PATCH_FIELD(launch_profile_id)
    ECLIPSE_PATCH_FIELD(sandbox_profile_id)
    ECLIPSE_PATCH_FIELD(virtual_displays)
    ECLIPSE_PATCH_FIELD(peripheral_policy)
    ECLIPSE_PATCH_FIELD(persistent)
    ECLIPSE_PATCH_FIELD(cleanup_policy)
#undef ECLIPSE_PATCH_FIELD
    if (!impl_->validate(resource.definition)) {
      return {status_t::invalid, std::nullopt};
    }
    resource.updated_at = impl_->callbacks.now();
    ++resource.revision;
    auto candidate = impl_->resources;
    candidate[id] = resource;
    const auto status = impl_->publish(std::move(candidate));
    return {status, status == status_t::success ? std::optional {resource} : std::nullopt};
  }

  result_t manager_t::remove(const std::string &id, const std::uint64_t expected_revision) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::stopped) {
      return {status_t::conflict, std::nullopt};
    }
    auto candidate = impl_->resources;
    candidate.erase(id);
    return {impl_->publish(std::move(candidate)), std::nullopt};
  }

  result_t manager_t::adopt(const std::string &id, const std::uint64_t expected_revision, const std::string &owner_client_uuid) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.owner_client_uuid) {
      return {status_t::resource_busy, std::nullopt};
    }
    if (found->second.revision != expected_revision) {
      return {status_t::conflict, std::nullopt};
    }
    if (!valid_uuid(owner_client_uuid)) {
      return {status_t::invalid, std::nullopt};
    }
    auto resource = found->second;
    resource.owner_client_uuid = owner_client_uuid;
    resource.updated_at = impl_->callbacks.now();
    ++resource.revision;
    auto candidate = impl_->resources;
    candidate[id] = resource;
    const auto status = impl_->publish(std::move(candidate));
    return {status, status == status_t::success ? std::optional {resource} : std::nullopt};
  }

  result_t manager_t::start(const std::string &id, const std::uint64_t expected_revision, const start_request_t &request) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.state == state_t::active && found->second.revision == expected_revision) {
      return {status_t::success, found->second};
    }
    if (found->second.revision != expected_revision || (found->second.state != state_t::stopped && found->second.state != state_t::failed) || !found->second.owner_client_uuid) {
      return {status_t::conflict, std::nullopt};
    }
    const auto app = request.app_uuid.value_or(found->second.definition.desktop_app_uuid);
    if (!valid_uuid(app) || !impl_->callbacks.validate_app || !impl_->callbacks.validate_app(app) || (app != found->second.definition.desktop_app_uuid && !std::ranges::contains(found->second.definition.permitted_app_uuids, app))) {
      return {status_t::invalid, std::nullopt};
    }
    const std::pair<const char *, const std::optional<nlohmann::json> *> overrides[] {{"display", &request.profile_overrides.display}, {"stream", &request.profile_overrides.stream}, {"launch", &request.profile_overrides.launch}, {"sandbox", &request.profile_overrides.sandbox}};
    for (const auto &[kind, value] : overrides) {
      if (*value && !(*value)->is_null() && (!impl_->callbacks.validate_profile_configuration || !impl_->callbacks.validate_profile_configuration(kind, **value))) {
        return {status_t::invalid, std::nullopt};
      }
    }
    const auto original = found->second;
    auto preparing_resource = original;
    preparing_resource.state = state_t::preparing;
    preparing_resource.error = nullptr;
    preparing_resource.updated_at = impl_->callbacks.now();
    ++preparing_resource.revision;
    auto preparing_candidate = impl_->resources;
    preparing_candidate[id] = preparing_resource;
    if (impl_->publish(std::move(preparing_candidate)) != status_t::success) {
      return {status_t::persistence_error, std::nullopt};
    }
    std::optional<prepared_t> runtime;
    try {
      runtime = impl_->callbacks.prepare ? impl_->callbacks.prepare({id, *preparing_resource.owner_client_uuid, app, preparing_resource.definition, request.profile_overrides}) : std::nullopt;
    } catch (...) {
    }
    const auto runtime_valid = runtime && runtime->success && !runtime->session_id && (!runtime->sandbox_id || valid_uuid(*runtime->sandbox_id)) && valid_uuids(runtime->display_ids) && valid_uuids(runtime->peripheral_claim_ids);
    const auto rollback_to_stopped = [&](const std::uint64_t revision) -> std::optional<resource_t> {
      auto rollback = impl_->resources;
      auto stopped = original;
      stopped.revision = revision;
      stopped.updated_at = impl_->callbacks.now();
      rollback[id] = stopped;
      if (impl_->publish(std::move(rollback)) != status_t::success) {
        impl_->usable = false;
        return std::nullopt;
      }
      return stopped;
    };
    if (!runtime_valid) {
      try {
        if (impl_->callbacks.cleanup_failed_prepare) {
          impl_->callbacks.cleanup_failed_prepare(runtime.value_or(prepared_t {false}));
        }
      } catch (...) {
      }
      const auto stopped = rollback_to_stopped(preparing_resource.revision + 1);
      if (!stopped) {
        return {status_t::unavailable, std::nullopt};
      }
      return {status_t::preparation_error, stopped};
    }
    auto resource = preparing_resource;
    resource.session_id.reset();
    resource.sandbox_id = runtime->sandbox_id;
    resource.display_ids = runtime->display_ids;
    resource.peripheral_claim_ids = runtime->peripheral_claim_ids;
    resource.state = state_t::ready;
    resource.updated_at = impl_->callbacks.now();
    ++resource.revision;
    auto candidate = impl_->resources;
    candidate[id] = resource;
    const auto status = impl_->publish(std::move(candidate));
    if (status != status_t::success) {
      try {
        if (impl_->callbacks.cleanup_failed_prepare) {
          impl_->callbacks.cleanup_failed_prepare(*runtime);
        }
      } catch (...) {
      }
      if (!rollback_to_stopped(resource.revision + 1)) {
        return {status_t::unavailable, std::nullopt};
      }
      return {status_t::persistence_error, std::nullopt};
    }
    return {status_t::success, resource};
  }

  result_t manager_t::activate(const std::string &id, const std::uint64_t expected_revision, const std::string &session_id) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.state == state_t::active && found->second.session_id == session_id) {
      return {status_t::success, found->second};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::ready || !valid_uuid(session_id)) {
      return {status_t::conflict, std::nullopt};
    }
    auto resource = found->second;
    resource.state = state_t::active;
    resource.session_id = session_id;
    resource.updated_at = impl_->callbacks.now();
    ++resource.revision;
    auto candidate = impl_->resources;
    candidate[id] = resource;
    const auto status = impl_->publish(std::move(candidate));
    return {status, status == status_t::success ? std::optional {resource} : std::nullopt};
  }

  result_t manager_t::stop(const std::string &id, const std::uint64_t expected_revision, const bool terminate_application) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || (found->second.state != state_t::ready && found->second.state != state_t::active && found->second.state != state_t::failed)) {
      return {status_t::conflict, std::nullopt};
    }
    const auto original = found->second;
    auto stopping = original;
    stopping.state = state_t::stopping;
    stopping.updated_at = impl_->callbacks.now();
    ++stopping.revision;
    auto stopping_candidate = impl_->resources;
    stopping_candidate[id] = stopping;
    if (impl_->publish(std::move(stopping_candidate)) != status_t::success) {
      return {status_t::persistence_error, std::nullopt};
    }
    const auto rollback = [&](const std::uint64_t revision) -> std::optional<resource_t> {
      auto restored = original;
      restored.revision = revision;
      restored.updated_at = impl_->callbacks.now();
      auto candidate = impl_->resources;
      candidate[id] = restored;
      if (impl_->publish(std::move(candidate)) != status_t::success) {
        impl_->usable = false;
        return std::nullopt;
      }
      return restored;
    };
    const auto recover_provider_failure = [&]() -> result_t {
      bool runtime_restored = false;
      try {
        runtime_restored = impl_->callbacks.restore_after_failed_stop && impl_->callbacks.restore_after_failed_stop(original);
      } catch (...) {
      }
      if (!runtime_restored) {
        impl_->usable = false;
        return {status_t::unavailable, std::nullopt};
      }
      const auto restored = rollback(stopping.revision + 1);
      return restored ? result_t {status_t::provider_error, restored} : result_t {status_t::unavailable, std::nullopt};
    };
    try {
      if (!impl_->callbacks.stop || !impl_->callbacks.stop(original, terminate_application)) {
        return recover_provider_failure();
      }
    } catch (...) {
      return recover_provider_failure();
    }
    auto resource = stopping;
    resource.state = terminate_application ? state_t::stopped : state_t::ready;
    resource.session_id.reset();
    if (terminate_application) {
      clear_runtime(resource);
    }
    resource.error = nullptr;
    resource.updated_at = impl_->callbacks.now();
    ++resource.revision;
    auto candidate = impl_->resources;
    candidate[id] = resource;
    const auto status = impl_->publish(std::move(candidate));
    if (status != status_t::success) {
      bool restored = false;
      try {
        restored = impl_->callbacks.restore_after_failed_stop && impl_->callbacks.restore_after_failed_stop(original);
      } catch (...) {
      }
      if (!restored || !rollback(resource.revision + 1)) {
        impl_->usable = false;
        return {status_t::unavailable, std::nullopt};
      }
      return {status_t::persistence_error, std::nullopt};
    }
    return {status_t::success, resource};
  }

  status_t manager_t::revoke_owner(const std::string &owner_client_uuid) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->usable) {
      return status_t::unavailable;
    }
    std::vector<resource_t> runtimes;
    for (const auto &[id, resource] : impl_->resources) {
      if (resource.owner_client_uuid == owner_client_uuid && resource.state != state_t::stopped) {
        runtimes.push_back(resource);
      }
    }
    if (!runtimes.empty() && (!impl_->callbacks.stop || !impl_->callbacks.restore_after_failed_stop)) {
      return status_t::provider_error;
    }
    std::vector<resource_t> stopped;
    const auto restore_stopped = [&]() {
      bool restored = true;
      for (auto iterator = stopped.rbegin(); iterator != stopped.rend(); ++iterator) {
        try {
          restored = impl_->callbacks.restore_after_failed_stop(*iterator) && restored;
        } catch (...) {
          restored = false;
        }
      }
      if (!restored) {
        impl_->usable = false;
      }
      return restored;
    };
    for (const auto &resource : runtimes) {
      try {
        if (!impl_->callbacks.stop(resource, true)) {
          restore_stopped();
          return status_t::provider_error;
        }
        stopped.push_back(resource);
      } catch (...) {
        restore_stopped();
        return status_t::provider_error;
      }
    }
    auto candidate = impl_->resources;
    bool changed = false;
    for (auto iterator = candidate.begin(); iterator != candidate.end();) {
      auto &resource = iterator->second;
      if (resource.owner_client_uuid != owner_client_uuid) {
        ++iterator;
        continue;
      }
      changed = true;
      if (!resource.definition.persistent) {
        iterator = candidate.erase(iterator);
        continue;
      }
      clear_runtime(resource);
      resource.state = state_t::stopped;
      resource.owner_client_uuid.reset();
      resource.error = nullptr;
      resource.updated_at = impl_->callbacks.now();
      ++resource.revision;
      ++iterator;
    }
    if (!changed) {
      return status_t::success;
    }
    const auto status = impl_->publish(std::move(candidate));
    if (status != status_t::success && !restore_stopped()) {
      return status_t::unavailable;
    }
    return status;
  }

  nlohmann::json to_json(const resource_t &resource) {
    auto value = definition_json(resource.definition);
    value.update({
      {"id", resource.id},
      {"ownerClientUuid", resource.owner_client_uuid ? nlohmann::json(*resource.owner_client_uuid) : nlohmann::json(nullptr)},
      {"state", state_name(resource.state)},
      {"sessionId", resource.session_id ? nlohmann::json(*resource.session_id) : nlohmann::json(nullptr)},
      {"sandboxId", resource.sandbox_id ? nlohmann::json(*resource.sandbox_id) : nlohmann::json(nullptr)},
      {"displayIds", resource.display_ids},
      {"peripheralClaimIds", resource.peripheral_claim_ids},
      {"createdAt", resource.created_at},
      {"updatedAt", resource.updated_at},
      {"error", resource.error},
      {"revision", resource.revision},
    });
    return value;
  }
}  // namespace eclipse_workspaces
