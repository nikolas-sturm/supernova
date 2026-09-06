/**
 * @file src/eclipse_sandboxes.cpp
 * @brief Standalone Eclipse sandbox policy and lifecycle implementation.
 */

// standard includes
#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <utility>

// local includes
#include "eclipse_sandboxes.h"
#include "utility.h"
#include "uuid.h"

namespace eclipse_sandboxes {
  namespace {
    constexpr int DOCUMENT_VERSION = 1;  ///< Persistence schema version.
    constexpr std::size_t MAX_TEXT = 4096;  ///< Maximum non-secret policy string size.

    /** @brief Return true for canonical lowercase UUID text. */
    bool valid_uuid(const std::string &value) {
      return uuid_util::is_valid(value) && std::none_of(value.begin(), value.end(), [](const unsigned char character) {
               return character >= 'A' && character <= 'F';
             });
    }

    /** @brief Return true for bounded text without control or NUL characters. */
    bool valid_text(const std::string &value, const bool allow_empty = false) {
      return (allow_empty || !value.empty()) && value.size() <= MAX_TEXT && std::none_of(value.begin(), value.end(), [](const unsigned char character) {
               return character < 0x20 || character == 0x7f;
             });
    }

    /** @brief Return true when object contains only listed keys. */
    bool exact_keys(const nlohmann::json &value, const std::initializer_list<const char *> keys) {
      if (!value.is_object()) {
        return false;
      }
      const std::set<std::string_view> allowed(keys.begin(), keys.end());
      return std::ranges::all_of(value.items(), [&](const auto &item) {
        return allowed.contains(item.key());
      });
    }

    /** @brief Validate unique bounded string array, optionally requiring UUIDs. */
    bool string_array(const nlohmann::json &value, const bool uuids = false) {
      if (!value.is_array() || value.size() > 1024) {
        return false;
      }
      std::set<std::string> unique;
      for (const auto &entry : value) {
        if (!entry.is_string()) {
          return false;
        }
        const auto text = entry.get<std::string>();
        if ((uuids ? !valid_uuid(text) : !valid_text(text)) || !unique.emplace(text).second) {
          return false;
        }
      }
      return true;
    }

    /** @brief Validate a unique vector of canonical lowercase UUIDs. */
    bool uuid_vector(const std::vector<std::string> &values) {
      std::set<std::string> unique;
      return values.size() <= 1024 && std::ranges::all_of(values, [&](const auto &value) {
               return valid_uuid(value) && unique.emplace(value).second;
             });
    }

    /** @brief Validate exact enum string. */
    bool enum_value(const nlohmann::json &value, const std::initializer_list<std::string_view> values) {
      return value.is_string() && std::ranges::find(values, value.get<std::string>()) != values.end();
    }

    /** @brief Convert lifecycle state to stable text. */
    std::string state_name(const state_t state) {
      switch (state) {
        case state_t::created:
          return "created";
        case state_t::starting:
          return "starting";
        case state_t::running:
          return "running";
        case state_t::stopping:
          return "stopping";
        case state_t::stopped:
          return "stopped";
        case state_t::failed:
          return "failed";
        case state_t::deleting:
          return "deleting";
      }
      return {};
    }

    /** @brief Parse stable lifecycle state text. */
    std::optional<state_t> parse_state(const std::string &value) {
      static const std::map<std::string, state_t, std::less<>> states {
        {"created", state_t::created},
        {"starting", state_t::starting},
        {"running", state_t::running},
        {"stopping", state_t::stopping},
        {"stopped", state_t::stopped},
        {"failed", state_t::failed},
        {"deleting", state_t::deleting}
      };
      const auto found = states.find(value);
      return found == states.end() ? std::nullopt : std::optional<state_t> {found->second};
    }

    /** @brief Build standard non-sensitive lifecycle error. */
    error_t make_error(std::string code, std::string message) {
      return {std::move(code), std::move(message)};
    }

    /** @brief Serialize optional error. */
    nlohmann::json error_json(const std::optional<error_t> &error) {
      return error ? nlohmann::json {{"code", error->code}, {"message", error->message}} : nlohmann::json(nullptr);
    }

    /** @brief Validate environment variable name. */
    bool environment_name(const std::string &name) {
      if (name.empty() || name.size() > 256 || !(std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_')) {
        return false;
      }
      return std::ranges::all_of(name, [](const unsigned char character) {
        return std::isalnum(character) || character == '_';
      });
    }

    /** @brief Return policy contract defaults. */
    nlohmann::json default_policy() {
      return {
        {"allowedAppUuids", nlohmann::json::array()},
        {"executablePolicy", "configured-only"},
        {"filesystem", {{"readOnlyRoots", nlohmann::json::array()}, {"writableRoots", nlohmann::json::array()}, {"denyOther", true}}},
        {"environment", {{"allowedNames", nlohmann::json::array()}, {"values", nlohmann::json::object()}}},
        {"network", {{"mode", "none"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", nlohmann::json::array()}}},
        {"resources", {{"cpuPercent", nullptr}, {"memoryBytes", nullptr}, {"processCount", nullptr}, {"storageBytes", nullptr}}},
        {"gpu", {{"mode", "none"}, {"encoder", false}}},
        {"displays", {{"allowedIds", nlohmann::json::array()}, {"virtualOnly", true}}},
        {"input", {{"classes", nlohmann::json::array()}}},
        {"peripherals", {{"deviceIds", nlohmann::json::array()}, {"classes", nlohmann::json::array()}}},
        {"clipboard", "none"},
        {"hostIntegration", "none"},
        {"elevation", "deny"},
        {"persistentData", {{"enabled", false}, {"name", nullptr}}},
        {"timeoutMs", 0},
        {"cleanupPolicy", "delete"},
      };
    }

    /** @brief Parse optional UUID JSON field. */
    std::optional<std::string> optional_uuid(const nlohmann::json &value) {
      return value.is_null() ? std::nullopt : std::optional<std::string> {value.get<std::string>()};
    }

    /** @brief Parse optional integer JSON field. */
    std::optional<std::int64_t> optional_time(const nlohmann::json &value) {
      return value.is_null() ? std::nullopt : std::optional<std::int64_t> {value.get<std::int64_t>()};
    }

    /** @brief Parse optional process exit code. */
    std::optional<int> optional_exit(const nlohmann::json &value) {
      return value.is_null() ? std::nullopt : std::optional<int> {value.get<int>()};
    }

    /** @brief Parse optional lifecycle error. */
    std::optional<error_t> optional_error(const nlohmann::json &value) {
      if (value.is_null()) {
        return std::nullopt;
      }
      if (!exact_keys(value, {"code", "message"}) || !value.contains("code") || !value.contains("message")) {
        throw nlohmann::json::type_error::create(302, "invalid error", &value);
      }
      error_t result {value.at("code").get<std::string>(), value.at("message").get<std::string>()};
      if (!valid_text(result.code) || !valid_text(result.message, true)) {
        throw nlohmann::json::type_error::create(302, "invalid error text", &value);
      }
      return result;
    }
  }  // namespace

  bool normalize_policy(const nlohmann::json &configuration, nlohmann::json &effective) {
    try {
      const auto keys = {"allowedAppUuids", "executablePolicy", "filesystem", "environment", "network", "resources", "gpu", "displays", "input", "peripherals", "clipboard", "hostIntegration", "elevation", "persistentData", "timeoutMs", "cleanupPolicy"};
      if (!exact_keys(configuration, keys)) {
        return false;
      }
      effective = default_policy();
      for (const auto &[key, value] : configuration.items()) {
        effective[key] = value;
      }
      const auto &filesystem = effective.at("filesystem");
      const auto &environment = effective.at("environment");
      const auto &network = effective.at("network");
      const auto &resources = effective.at("resources");
      const auto &gpu = effective.at("gpu");
      const auto &displays = effective.at("displays");
      const auto &input = effective.at("input");
      const auto &peripherals = effective.at("peripherals");
      const auto &persistent = effective.at("persistentData");
      if (!string_array(effective.at("allowedAppUuids"), true) || !enum_value(effective.at("executablePolicy"), {"configured-only", "allowlist"}) || !exact_keys(filesystem, {"readOnlyRoots", "writableRoots", "denyOther"}) || !filesystem.contains("readOnlyRoots") || !filesystem.contains("writableRoots") || !filesystem.contains("denyOther") || !string_array(filesystem.at("readOnlyRoots")) || !string_array(filesystem.at("writableRoots")) || !filesystem.at("denyOther").is_boolean()) {
        return false;
      }
      if (!exact_keys(environment, {"allowedNames", "values"}) || !environment.contains("allowedNames") || !environment.contains("values") || !string_array(environment.at("allowedNames")) || !environment.at("values").is_object()) {
        return false;
      }
      std::set<std::string> allowed_names;
      for (const auto &value : environment.at("allowedNames")) {
        const auto name = value.get<std::string>();
        if (!environment_name(name)) {
          return false;
        }
        allowed_names.emplace(name);
      }
      for (const auto &[name, value] : environment.at("values").items()) {
        if (!environment_name(name) || !allowed_names.contains(name)) {
          return false;
        }
        if (value.is_string()) {
          if (!valid_text(value.get<std::string>(), true)) {
            return false;
          }
        } else if (!exact_keys(value, {"secretRef"}) || value.size() != 1 || !value.contains("secretRef") || !value.at("secretRef").is_string() || !valid_text(value.at("secretRef").get<std::string>())) {
          return false;
        }
      }
      if (!exact_keys(network, {"mode", "allowedHosts", "allowedPorts"}) || !network.contains("mode") || !network.contains("allowedHosts") || !network.contains("allowedPorts") || !enum_value(network.at("mode"), {"none", "outbound", "full"}) || !string_array(network.at("allowedHosts")) || !network.at("allowedPorts").is_array() || network.at("allowedPorts").size() > 65535) {
        return false;
      }
      std::set<int> ports;
      for (const auto &port : network.at("allowedPorts")) {
        if (!port.is_number_integer()) {
          return false;
        }
        const auto number = port.get<std::int64_t>();
        if (number < 1 || number > 65535 || !ports.emplace(static_cast<int>(number)).second) {
          return false;
        }
      }
      if (!exact_keys(resources, {"cpuPercent", "memoryBytes", "processCount", "storageBytes"}) || resources.size() != 4) {
        return false;
      }
      const auto positive_limit = [](const nlohmann::json &value, const std::uint64_t maximum) {
        return value.is_null() || (value.is_number_integer() && value.get<std::int64_t>() >= 1 && static_cast<std::uint64_t>(value.get<std::int64_t>()) <= maximum);
      };
      if (!positive_limit(resources.at("cpuPercent"), 100) || !positive_limit(resources.at("memoryBytes"), std::numeric_limits<std::uint64_t>::max()) || !positive_limit(resources.at("processCount"), std::numeric_limits<std::uint32_t>::max()) || !positive_limit(resources.at("storageBytes"), std::numeric_limits<std::uint64_t>::max())) {
        return false;
      }
      if (!exact_keys(gpu, {"mode", "encoder"}) || gpu.size() != 2 || !enum_value(gpu.at("mode"), {"none", "compute", "display", "full"}) || !gpu.at("encoder").is_boolean() || !exact_keys(displays, {"allowedIds", "virtualOnly"}) || displays.size() != 2 || !string_array(displays.at("allowedIds"), true) || !displays.at("virtualOnly").is_boolean() || !exact_keys(input, {"classes"}) || input.size() != 1 || !string_array(input.at("classes")) || !exact_keys(peripherals, {"deviceIds", "classes"}) || peripherals.size() != 2 || !string_array(peripherals.at("deviceIds")) || !string_array(peripherals.at("classes"))) {
        return false;
      }
      if (!enum_value(effective.at("clipboard"), {"none", "read", "write", "bidirectional"}) || !enum_value(effective.at("hostIntegration"), {"none", "restricted"}) || !enum_value(effective.at("elevation"), {"deny", "allow"}) || !exact_keys(persistent, {"enabled", "name"}) || persistent.size() != 2 || !persistent.at("enabled").is_boolean() || (!persistent.at("name").is_null() && (!persistent.at("name").is_string() || !valid_text(persistent.at("name").get<std::string>()))) || !effective.at("timeoutMs").is_number_integer() || effective.at("timeoutMs").get<std::int64_t>() < 0 || !enum_value(effective.at("cleanupPolicy"), {"reset", "retain", "delete"})) {
        return false;
      }
      if (persistent.at("enabled").get<bool>() != !persistent.at("name").is_null()) {
        return false;
      }
      return true;
    } catch (...) {
      return false;
    }
  }

  struct manager_t::impl_t {
    callbacks_t callbacks;  ///< Injected persistence and provider operations.
    mutable std::mutex mutex;  ///< Serializes all state and provider callbacks.
    bool usable = false;  ///< Whether manager accepts operations.
    std::map<std::string, resource_t, std::less<>> resources;  ///< Resources by sandbox UUID.
    std::uint64_t collection_revision = 0;  ///< Monotonic collection revision.

    /** @brief Return monotonic valid wall-clock value. */
    std::optional<std::int64_t> time() const {
      try {
        const auto value = callbacks.now ? callbacks.now() : -1;
        return value < 0 ? std::nullopt : std::optional<std::int64_t> {value};
      } catch (...) {
        return std::nullopt;
      }
    }

    /** @brief Serialize complete persistent state. */
    nlohmann::json document(const decltype(resources) &values, const std::uint64_t revision) const {
      nlohmann::json records = nlohmann::json::array();
      for (const auto &[id, resource] : values) {
        auto value = to_json(resource);
        value["runtimeId"] = resource.runtime_id ? nlohmann::json(*resource.runtime_id) : nlohmann::json(nullptr);
        records.push_back(std::move(value));
      }
      return {{"version", DOCUMENT_VERSION}, {"collectionRevision", revision}, {"resources", std::move(records)}};
    }

    /** @brief Persist candidate state without publishing it. */
    bool save(const decltype(resources) &values, const std::uint64_t revision) const {
      try {
        return callbacks.save && callbacks.save(document(values, revision).dump());
      } catch (...) {
        return false;
      }
    }

    /** @brief Publish one candidate resource transactionally. */
    bool publish(decltype(resources) candidate) {
      const auto revision = collection_revision + 1;
      if (!save(candidate, revision)) {
        return false;
      }
      auto previous = resources;
      resources = std::move(candidate);
      collection_revision = revision;
      if (callbacks.on_change) {
        auto old_resource = previous.begin();
        auto new_resource = resources.begin();
        while (old_resource != previous.end() || new_resource != resources.end()) {
          try {
            if (new_resource == resources.end() || (old_resource != previous.end() && old_resource->first < new_resource->first)) {
              callbacks.on_change(old_resource->second, std::nullopt);
              ++old_resource;
            } else if (old_resource == previous.end() || new_resource->first < old_resource->first) {
              callbacks.on_change(std::nullopt, new_resource->second);
              ++new_resource;
            } else {
              const auto changed = to_json(old_resource->second) != to_json(new_resource->second) || old_resource->second.runtime_id != new_resource->second.runtime_id;
              if (changed) {
                callbacks.on_change(old_resource->second, new_resource->second);
              }
              ++old_resource;
              ++new_resource;
            }
          } catch (...) {
          }
        }
      }
      return true;
    }

    /** @brief Invoke cleanup callback safely. */
    bool cleanup(const resource_t &resource) const {
      try {
        return callbacks.cleanup && callbacks.cleanup(resource.id, resource.effective_policy);
      } catch (...) {
        return false;
      }
    }

    /** @brief Invoke provider termination safely. */
    termination_t terminate(const resource_t &resource, const bool force) const {
      if (!resource.runtime_id) {
        return {true, resource.exit_code, std::nullopt};
      }
      try {
        return callbacks.terminate ? callbacks.terminate(*resource.runtime_id, force) : termination_t {false, std::nullopt, make_error("provider_unavailable", "Termination provider unavailable")};
      } catch (...) {
        return {false, std::nullopt, make_error("provider_error", "Termination provider failed")};
      }
    }

    /** @brief Clear runtime associations after confirmed termination. */
    void clear_runtime(resource_t &resource, const std::optional<int> exit_code, const std::int64_t now) const {
      resource.runtime_id.reset();
      resource.session_id.reset();
      resource.display_ids.clear();
      resource.peripheral_claim_ids.clear();
      resource.exit_code = exit_code;
      resource.stopped_at = now;
      resource.updated_at = std::max(resource.updated_at, now);
    }

    /** @brief Mark resource failed with monotonic revision and timestamp. */
    void fail(resource_t &resource, error_t error, const std::int64_t now) const {
      resource.state = state_t::failed;
      resource.error = std::move(error);
      resource.updated_at = std::max(resource.updated_at, now);
      ++resource.revision;
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
      if (!exact_keys(document, {"version", "collectionRevision", "resources"}) || document.size() != 3 || document.at("version") != DOCUMENT_VERSION || !document.at("collectionRevision").is_number_unsigned() || !document.at("resources").is_array()) {
        return;
      }
      impl_->collection_revision = document.at("collectionRevision").get<std::uint64_t>();
      bool invalid_record = false;
      for (const auto &value : document.at("resources")) {
        try {
          const auto fields = {"id", "name", "ownerClientUuid", "profileId", "workspaceId", "appUuid", "sessionId", "persistent", "state", "effectivePolicy", "displayIds", "peripheralClaimIds", "createdAt", "startedAt", "updatedAt", "stoppedAt", "exitCode", "error", "revision", "runtimeId"};
          if (!exact_keys(value, fields) || value.size() != fields.size()) {
            throw std::invalid_argument("sandbox record shape");
          }
          const auto state = parse_state(value.at("state").get<std::string>());
          if (!state) {
            throw std::invalid_argument("sandbox state");
          }
          resource_t resource {value.at("id").get<std::string>(), value.at("name").get<std::string>(), optional_uuid(value.at("ownerClientUuid")), value.at("profileId").get<std::string>(), optional_uuid(value.at("workspaceId")), optional_uuid(value.at("appUuid")), optional_uuid(value.at("sessionId")), value.at("persistent").get<bool>(), *state, value.at("effectivePolicy"), value.at("displayIds").get<std::vector<std::string>>(), value.at("peripheralClaimIds").get<std::vector<std::string>>(), value.at("createdAt").get<std::int64_t>(), optional_time(value.at("startedAt")), value.at("updatedAt").get<std::int64_t>(), optional_time(value.at("stoppedAt")), optional_exit(value.at("exitCode")), optional_error(value.at("error")), value.at("revision").get<std::uint64_t>(), value.at("runtimeId").is_null() ? std::nullopt : std::optional<std::string> {value.at("runtimeId").get<std::string>()}};
          nlohmann::json normalized;
          if (!valid_uuid(resource.id) || !valid_text(resource.name) || (resource.owner_client_uuid && !valid_uuid(*resource.owner_client_uuid)) || !valid_uuid(resource.profile_id) || (resource.workspace_id && !valid_uuid(*resource.workspace_id)) || (resource.app_uuid && !valid_uuid(*resource.app_uuid)) || (resource.session_id && !valid_uuid(*resource.session_id)) || !uuid_vector(resource.display_ids) || !uuid_vector(resource.peripheral_claim_ids) || resource.created_at < 0 || resource.updated_at < resource.created_at || (resource.started_at && *resource.started_at < resource.created_at) || (resource.stopped_at && *resource.stopped_at < resource.created_at) || resource.revision == 0 || !normalize_policy(resource.effective_policy, normalized) || normalized != resource.effective_policy || (resource.runtime_id && !valid_text(*resource.runtime_id)) || !impl_->resources.emplace(resource.id, resource).second) {
            throw std::invalid_argument("sandbox record");
          }
        } catch (...) {
          invalid_record = true;
          break;
        }
      }
      if (invalid_record) {
        impl_->resources.clear();
        return;
      }
      auto candidate = impl_->resources;
      const auto now = impl_->time();
      if (!now) {
        impl_->resources.clear();
        return;
      }
      for (auto iterator = candidate.begin(); iterator != candidate.end();) {
        auto &resource = iterator->second;
        if (!resource.persistent) {
          const auto termination = impl_->terminate(resource, true);
          if (!termination.terminated || !impl_->cleanup(resource)) {
            impl_->resources.clear();
            return;
          }
          iterator = candidate.erase(iterator);
          continue;
        }
        if (resource.runtime_id) {
          reconciliation_t reconciled {reconciliation_t::status_t::error, std::nullopt, make_error("provider_unavailable", "Reconciliation provider unavailable")};
          try {
            if (impl_->callbacks.reconcile) {
              reconciled = impl_->callbacks.reconcile(*resource.runtime_id);
            }
          } catch (...) {
          }
          if (reconciled.status == reconciliation_t::status_t::running) {
            resource.state = state_t::running;
            resource.error.reset();
          } else {
            impl_->clear_runtime(resource, reconciled.exit_code, *now);
            resource.state = reconciled.status == reconciliation_t::status_t::error ? state_t::failed : state_t::stopped;
            resource.error = reconciled.status == reconciliation_t::status_t::error ? (reconciled.error ? reconciled.error : std::optional<error_t> {make_error("reconcile_failed", "Runtime reconciliation failed")}) : std::nullopt;
            if (!impl_->cleanup(resource)) {
              impl_->resources.clear();
              return;
            }
          }
          resource.updated_at = std::max(resource.updated_at, *now);
          ++resource.revision;
        } else if (resource.state == state_t::starting || resource.state == state_t::stopping || resource.state == state_t::deleting || resource.state == state_t::running) {
          impl_->fail(resource, make_error("host_restarted", "Host restarted during sandbox lifecycle operation"), *now);
        }
        ++iterator;
      }
      ++impl_->collection_revision;
      if (!impl_->save(candidate, impl_->collection_revision)) {
        impl_->resources.clear();
        return;
      }
      impl_->resources = std::move(candidate);
      impl_->usable = true;
    } catch (...) {
      impl_->resources.clear();
    }
  }

  manager_t::~manager_t() = default;

  bool manager_t::available() const {
    std::scoped_lock lock {impl_->mutex};
    return impl_->usable;
  }

  result_t manager_t::create(const std::string &owner, const create_t &request) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    nlohmann::json policy;
    if (!valid_uuid(owner) || !valid_uuid(request.profile_id) || (request.workspace_id && !valid_uuid(*request.workspace_id)) || (request.app_uuid && !valid_uuid(*request.app_uuid)) || !valid_text(request.name) || !normalize_policy(request.profile_configuration, policy)) {
      return {status_t::invalid, std::nullopt};
    }
    std::string reason;
    try {
      launch_request_t capability {"", {}, policy, request.workspace_id, request.persistent};
      if (!impl_->callbacks.provider_capable || !impl_->callbacks.provider_capable(capability, reason)) {
        return {status_t::unsupported_configuration, std::nullopt};
      }
    } catch (...) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    std::string id;
    const auto now = impl_->time();
    try {
      id = impl_->callbacks.uuid ? impl_->callbacks.uuid() : std::string {};
      std::ranges::transform(id, id.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
    } catch (...) {
      return {status_t::invalid, std::nullopt};
    }
    if (!now || !valid_uuid(id) || impl_->resources.contains(id)) {
      return {status_t::invalid, std::nullopt};
    }
    resource_t resource {id, request.name, owner, request.profile_id, request.workspace_id, request.app_uuid, std::nullopt, request.persistent, state_t::created, std::move(policy), {}, {}, *now, std::nullopt, *now, std::nullopt, std::nullopt, std::nullopt, 1, std::nullopt};
    auto candidate = impl_->resources;
    candidate.emplace(id, resource);
    if (!impl_->publish(std::move(candidate))) {
      return {status_t::persistence_error, std::nullopt};
    }
    return {status_t::success, impl_->resources.at(id)};
  }

  std::optional<resource_t> manager_t::get(const std::string &id) const {
    std::scoped_lock lock {impl_->mutex};
    const auto found = impl_->resources.find(id);
    return found == impl_->resources.end() ? std::nullopt : std::optional<resource_t> {found->second};
  }

  list_t manager_t::list(const std::optional<std::uint64_t> since) const {
    std::scoped_lock lock {impl_->mutex};
    list_t result {impl_->collection_revision, !since || *since != impl_->collection_revision, {}};
    if (result.changed) {
      for (const auto &[id, resource] : impl_->resources) {
        result.resources.push_back(resource);
      }
    }
    return result;
  }

  result_t manager_t::start(const std::string &id, const std::uint64_t expected_revision, const start_t &request) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if ((request.app_uuid && !valid_uuid(*request.app_uuid)) || (request.launch_profile_id && !valid_uuid(*request.launch_profile_id))) {
      return {status_t::invalid, std::nullopt};
    }
    if (found->second.revision != expected_revision || (found->second.state != state_t::created && found->second.state != state_t::stopped && found->second.state != state_t::failed) || found->second.owner_client_uuid == std::nullopt || found->second.runtime_id || found->second.session_id || !found->second.display_ids.empty() || !found->second.peripheral_claim_ids.empty() || (found->second.error && found->second.error->code == "cleanup_failed")) {
      return {status_t::conflict, std::nullopt};
    }
    const auto before_start = found->second;
    const auto app_uuid = request.app_uuid ? request.app_uuid : found->second.app_uuid;
    if (!app_uuid) {
      return {status_t::application_not_found, std::nullopt};
    }
    std::optional<application_t> application;
    try {
      if (impl_->callbacks.resolve_application) {
        application = impl_->callbacks.resolve_application(*app_uuid, request.launch_profile_id);
      }
    } catch (...) {
    }
    if (!application || application->app_uuid != *app_uuid || !valid_uuid(application->app_uuid) || (application->launch_profile_id && !valid_uuid(*application->launch_profile_id)) || !valid_text(application->executable)) {
      return {status_t::application_not_found, std::nullopt};
    }
    if (request.launch_data) {
      application->launch_data = *request.launch_data;
    }
    const auto &allowed = found->second.effective_policy.at("allowedAppUuids");
    if (std::ranges::find_if(allowed, [&](const auto &value) {
          return value.template get<std::string>() == *app_uuid;
        }) == allowed.end()) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    launch_request_t launch_request {id, *application, found->second.effective_policy, found->second.workspace_id, found->second.persistent};
    std::string reason;
    try {
      if (!impl_->callbacks.provider_capable || !impl_->callbacks.provider_capable(launch_request, reason)) {
        return {status_t::unsupported_configuration, std::nullopt};
      }
    } catch (...) {
      return {status_t::unsupported_configuration, std::nullopt};
    }
    const auto now = impl_->time();
    if (!now) {
      return {status_t::provider_error, std::nullopt};
    }
    auto starting = impl_->resources;
    auto &starting_resource = starting.at(id);
    starting_resource.state = state_t::starting;
    starting_resource.app_uuid = app_uuid;
    starting_resource.error.reset();
    starting_resource.exit_code.reset();
    starting_resource.updated_at = std::max(starting_resource.updated_at, *now);
    ++starting_resource.revision;
    if (!impl_->publish(std::move(starting))) {
      return {status_t::persistence_error, std::nullopt};
    }
    error_t launch_error = make_error("launch_failed", "Sandbox provider did not launch application");
    std::optional<launch_result_t> launched;
    try {
      if (impl_->callbacks.launch) {
        launched = impl_->callbacks.launch(launch_request, launch_error);
      }
    } catch (...) {
      launch_error = make_error("provider_error", "Sandbox provider launch failed");
    }
    const bool valid_launch = launched && valid_text(launched->runtime_id) && (!launched->session_id || valid_uuid(*launched->session_id)) && uuid_vector(launched->display_ids) && uuid_vector(launched->peripheral_claim_ids);
    auto completed = impl_->resources;
    auto &resource = completed.at(id);
    if (!valid_launch) {
      if (launched) {
        resource.runtime_id = launched->runtime_id;
        const auto terminated = impl_->terminate(resource, true);
        if (!terminated.terminated || !impl_->cleanup(resource)) {
          impl_->usable = false;
          return {status_t::provider_error, std::nullopt};
        }
        impl_->clear_runtime(resource, terminated.exit_code, *now);
      }
      impl_->fail(resource, std::move(launch_error), *now);
      if (!impl_->publish(std::move(completed))) {
        return {status_t::persistence_error, std::nullopt};
      }
      return {status_t::provider_error, impl_->resources.at(id)};
    }
    resource.state = state_t::running;
    resource.runtime_id = launched->runtime_id;
    resource.session_id = launched->session_id;
    resource.display_ids = launched->display_ids;
    resource.peripheral_claim_ids = launched->peripheral_claim_ids;
    resource.started_at = *now;
    resource.stopped_at.reset();
    resource.updated_at = std::max(resource.updated_at, *now);
    resource.error.reset();
    ++resource.revision;
    const auto rollback_resource = resource;
    if (!impl_->publish(std::move(completed))) {
      const auto rollback = impl_->terminate(rollback_resource, true);
      if (!rollback.terminated || !impl_->cleanup(rollback_resource)) {
        impl_->usable = false;
      } else {
        auto rollback_state = impl_->resources;
        auto &restored = rollback_state.at(id);
        const auto revision = restored.revision + 1;
        restored = before_start;
        restored.revision = revision;
        restored.updated_at = std::max(restored.updated_at, *now);
        if (!impl_->publish(std::move(rollback_state))) {
          impl_->usable = false;
        }
      }
      return {status_t::persistence_error, impl_->usable ? std::optional<resource_t> {impl_->resources.at(id)} : std::nullopt};
    }
    return {status_t::success, impl_->resources.at(id)};
  }

  result_t manager_t::stop(const std::string &id, const std::uint64_t expected_revision, const bool force) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::running || !found->second.runtime_id) {
      return {status_t::conflict, std::nullopt};
    }
    const auto now = impl_->time();
    if (!now) {
      return {status_t::provider_error, std::nullopt};
    }
    auto stopping = impl_->resources;
    auto &stopping_resource = stopping.at(id);
    stopping_resource.state = state_t::stopping;
    stopping_resource.updated_at = std::max(stopping_resource.updated_at, *now);
    ++stopping_resource.revision;
    if (!impl_->publish(std::move(stopping))) {
      return {status_t::persistence_error, std::nullopt};
    }
    const auto terminated = impl_->terminate(impl_->resources.at(id), force);
    auto completed = impl_->resources;
    auto &resource = completed.at(id);
    if (!terminated.terminated) {
      impl_->fail(resource, terminated.error ? *terminated.error : make_error("termination_failed", "Sandbox termination failed"), *now);
    } else {
      impl_->clear_runtime(resource, terminated.exit_code, *now);
      if (!impl_->cleanup(resource)) {
        impl_->fail(resource, make_error("cleanup_failed", "Sandbox resource cleanup failed"), *now);
      } else {
        resource.state = state_t::stopped;
        resource.error.reset();
        ++resource.revision;
      }
    }
    const auto status = resource.state == state_t::stopped ? status_t::success : status_t::provider_error;
    if (!impl_->publish(std::move(completed))) {
      impl_->usable = false;
      return {status_t::persistence_error, std::nullopt};
    }
    return {status, impl_->resources.at(id)};
  }

  result_t manager_t::restart(const std::string &id, const std::uint64_t expected_revision, const bool force, const start_t &request) {
    const auto stopped = stop(id, expected_revision, force);
    if (stopped.status != status_t::success) {
      return stopped;
    }
    return start(id, stopped.resource->revision, request);
  }

  result_t manager_t::remove(const std::string &id, const std::uint64_t expected_revision) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state == state_t::starting || found->second.state == state_t::stopping || found->second.state == state_t::deleting) {
      return {status_t::conflict, std::nullopt};
    }
    const auto now = impl_->time();
    if (!now) {
      return {status_t::provider_error, std::nullopt};
    }
    auto transitional = impl_->resources;
    auto &deleting = transitional.at(id);
    deleting.state = state_t::deleting;
    deleting.error.reset();
    deleting.updated_at = std::max(deleting.updated_at, *now);
    ++deleting.revision;
    if (!impl_->publish(std::move(transitional))) {
      return {status_t::persistence_error, std::nullopt};
    }
    const auto terminated = impl_->terminate(impl_->resources.at(id), true);
    if (!terminated.terminated || !impl_->cleanup(impl_->resources.at(id))) {
      auto failed = impl_->resources;
      auto &resource = failed.at(id);
      if (terminated.terminated) {
        impl_->clear_runtime(resource, terminated.exit_code, *now);
      }
      impl_->fail(resource, terminated.terminated ? make_error("cleanup_failed", "Sandbox resource cleanup failed") : (terminated.error ? *terminated.error : make_error("termination_failed", "Sandbox termination failed")), *now);
      if (!impl_->publish(std::move(failed))) {
        impl_->usable = false;
        return {status_t::persistence_error, std::nullopt};
      }
      return {status_t::provider_error, impl_->resources.at(id)};
    }
    const auto deleted = impl_->resources.at(id);
    auto completed = impl_->resources;
    completed.erase(id);
    if (!impl_->publish(std::move(completed))) {
      impl_->usable = false;
      return {status_t::persistence_error, impl_->resources.at(id)};
    }
    return {status_t::success, deleted};
  }

  result_t manager_t::adopt(const std::string &id, const std::uint64_t expected_revision, const std::string &owner) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    if (!valid_uuid(owner)) {
      return {status_t::invalid, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.owner_client_uuid || !found->second.persistent || found->second.state == state_t::running) {
      return {status_t::conflict, std::nullopt};
    }
    const auto now = impl_->time();
    if (!now) {
      return {status_t::provider_error, std::nullopt};
    }
    auto candidate = impl_->resources;
    auto &resource = candidate.at(id);
    resource.owner_client_uuid = owner;
    resource.updated_at = std::max(resource.updated_at, *now);
    ++resource.revision;
    if (!impl_->publish(std::move(candidate))) {
      return {status_t::persistence_error, std::nullopt};
    }
    return {status_t::success, impl_->resources.at(id)};
  }

  status_t manager_t::reconcile() {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return status_t::unavailable;
    }
    const auto now = impl_->time();
    if (!now) {
      return status_t::provider_error;
    }
    auto candidate = impl_->resources;
    bool changed = false;
    for (auto &[id, resource] : candidate) {
      static_cast<void>(id);
      if (resource.state != state_t::running || !resource.runtime_id) {
        continue;
      }
      reconciliation_t reconciled {reconciliation_t::status_t::error, std::nullopt, make_error("provider_unavailable", "Reconciliation provider unavailable")};
      try {
        if (impl_->callbacks.reconcile) {
          reconciled = impl_->callbacks.reconcile(*resource.runtime_id);
        }
      } catch (...) {
      }
      if (reconciled.status == reconciliation_t::status_t::running) {
        continue;
      }
      if (reconciled.status == reconciliation_t::status_t::error) {
        impl_->fail(resource, reconciled.error ? *reconciled.error : make_error("reconcile_failed", "Runtime reconciliation failed"), *now);
      } else {
        impl_->clear_runtime(resource, reconciled.exit_code, *now);
        if (!impl_->cleanup(resource)) {
          impl_->fail(resource, make_error("cleanup_failed", "Sandbox resource cleanup failed"), *now);
        } else {
          resource.state = state_t::stopped;
          resource.error.reset();
          ++resource.revision;
        }
      }
      changed = true;
    }
    if (!changed) {
      return status_t::success;
    }
    if (!impl_->publish(std::move(candidate))) {
      impl_->usable = false;
      return status_t::persistence_error;
    }
    return status_t::success;
  }

  status_t manager_t::revoke_owner(const std::string &owner) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return status_t::unavailable;
    }
    if (!valid_uuid(owner)) {
      return status_t::invalid;
    }
    std::vector<std::string> affected;
    for (const auto &[id, resource] : impl_->resources) {
      if (resource.owner_client_uuid && *resource.owner_client_uuid == owner) {
        affected.push_back(id);
      }
    }
    if (affected.empty()) {
      return status_t::success;
    }
    const auto now = impl_->time();
    if (!now) {
      return status_t::provider_error;
    }
    auto transitional = impl_->resources;
    for (const auto &id : affected) {
      auto &resource = transitional.at(id);
      resource.state = resource.persistent ? state_t::stopping : state_t::deleting;
      resource.error.reset();
      resource.updated_at = std::max(resource.updated_at, *now);
      ++resource.revision;
    }
    if (!impl_->publish(std::move(transitional))) {
      return status_t::persistence_error;
    }
    auto completed = impl_->resources;
    for (const auto &id : affected) {
      auto &resource = completed.at(id);
      const auto terminated = impl_->terminate(resource, true);
      if (!terminated.terminated || !impl_->cleanup(resource)) {
        if (terminated.terminated) {
          impl_->clear_runtime(resource, terminated.exit_code, *now);
        }
        impl_->fail(resource, terminated.terminated ? make_error("cleanup_failed", "Sandbox resource cleanup failed") : (terminated.error ? *terminated.error : make_error("termination_failed", "Sandbox termination failed")), *now);
        if (!impl_->publish(std::move(completed))) {
          impl_->usable = false;
          return status_t::persistence_error;
        }
        impl_->usable = false;
        return status_t::provider_error;
      }
      if (!resource.persistent) {
        completed.erase(id);
        continue;
      }
      impl_->clear_runtime(resource, terminated.exit_code, *now);
      resource.owner_client_uuid.reset();
      resource.state = state_t::stopped;
      resource.error.reset();
      ++resource.revision;
    }
    if (!impl_->publish(std::move(completed))) {
      impl_->usable = false;
      return status_t::persistence_error;
    }
    return status_t::success;
  }

  nlohmann::json to_json(const resource_t &resource) {
    return {
      {"id", resource.id},
      {"name", resource.name},
      {"ownerClientUuid", resource.owner_client_uuid ? nlohmann::json(*resource.owner_client_uuid) : nlohmann::json(nullptr)},
      {"profileId", resource.profile_id},
      {"workspaceId", resource.workspace_id ? nlohmann::json(*resource.workspace_id) : nlohmann::json(nullptr)},
      {"appUuid", resource.app_uuid ? nlohmann::json(*resource.app_uuid) : nlohmann::json(nullptr)},
      {"sessionId", resource.session_id ? nlohmann::json(*resource.session_id) : nlohmann::json(nullptr)},
      {"persistent", resource.persistent},
      {"state", state_name(resource.state)},
      {"effectivePolicy", resource.effective_policy},
      {"displayIds", resource.display_ids},
      {"peripheralClaimIds", resource.peripheral_claim_ids},
      {"createdAt", resource.created_at},
      {"startedAt", resource.started_at ? nlohmann::json(*resource.started_at) : nlohmann::json(nullptr)},
      {"updatedAt", resource.updated_at},
      {"stoppedAt", resource.stopped_at ? nlohmann::json(*resource.stopped_at) : nlohmann::json(nullptr)},
      {"exitCode", resource.exit_code ? nlohmann::json(*resource.exit_code) : nlohmann::json(nullptr)},
      {"error", error_json(resource.error)},
      {"revision", resource.revision},
    };
  }
}  // namespace eclipse_sandboxes
