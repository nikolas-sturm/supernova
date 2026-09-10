/**
 * @file src/terra_profiles.cpp
 * @brief Storage and validation for Terra profiles-v1 resources.
 */
#include "terra_profiles.h"

#include "terra_sandboxes.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <utility>

namespace terra::profiles {
  namespace {
    using json = nlohmann::json;
    const std::regex UUID_RE {"^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"};

    bool uuid(const std::string &value) {
      return std::regex_match(value, UUID_RE);
    }

    bool exact(const json &j, std::initializer_list<const char *> names) {
      if (!j.is_object()) {
        return false;
      }
      std::set<std::string> allowed;
      for (const auto *name : names) {
        allowed.emplace(name);
      }
      return std::all_of(j.items().begin(), j.items().end(), [&](const auto &item) {
        return allowed.contains(item.key());
      });
    }

    bool strings(const json &j) {
      return j.is_array() && std::all_of(j.begin(), j.end(), [](const auto &v) {
               return v.is_string();
             });
    }

    bool one_of(const json &j, std::initializer_list<const char *> values) {
      if (!j.is_string()) {
        return false;
      }
      return std::any_of(values.begin(), values.end(), [&](const auto *v) {
        return j == v;
      });
    }

    bool integer(const json &j, std::uint64_t low, std::uint64_t high) {
      if (!j.is_number_integer()) {
        return false;
      }
      if (j.is_number_unsigned()) {
        const auto value = j.get<std::uint64_t>();
        return value >= low && value <= high;
      }
      const auto value = j.get<std::int64_t>();
      return value >= 0 && static_cast<std::uint64_t>(value) >= low && static_cast<std::uint64_t>(value) <= high;
    }

    bool nullable_uuid(const json &j) {
      return j.is_null() || (j.is_string() && uuid(j.get<std::string>()));
    }

    bool nullable_text(const json &j) {
      return j.is_null() || (j.is_string() && !j.get_ref<const std::string &>().empty());
    }

    bool secret_value(const json &j) {
      return j.is_string() || (exact(j, {"secretRef"}) && j.contains("secretRef") && j["secretRef"].is_string() && !j["secretRef"].get_ref<const std::string &>().empty());
    }

    bool environment(const json &j) {
      return j.is_object() && std::all_of(j.items().begin(), j.items().end(), [](const auto &v) {
               return !v.key().empty() && secret_value(v.value());
             });
    }

    bool action(const json &j) {
      return exact(j, {"command", "undoCommand", "elevated", "timeoutMs", "failurePolicy"}) && j.contains("command") && j["command"].is_string() && j.contains("undoCommand") && (j["undoCommand"].is_null() || j["undoCommand"].is_string()) && j.contains("elevated") && j["elevated"].is_boolean() && j.contains("timeoutMs") && integer(j["timeoutMs"], 0, std::numeric_limits<std::uint32_t>::max()) && j.contains("failurePolicy") && one_of(j["failurePolicy"], {"abort", "continue", "cleanup"});
    }

    bool actions(const json &j) {
      return j.is_array() && std::all_of(j.begin(), j.end(), action);
    }

    bool display_config(const json &c) {
      if (!exact(c, {"targetDisplayId", "topology", "modeId", "scale", "rotation", "hdr", "primaryPolicy", "restorePolicy", "virtualDisplays"})) {
        return false;
      }
      const std::set<int> rotations {0, 90, 180, 270};
      return c.size() == 9 && nullable_uuid(c["targetDisplayId"]) && (c["topology"].is_null() || c["topology"].is_object()) && nullable_text(c["modeId"]) && c["scale"].is_number() && std::isfinite(c["scale"].get<double>()) && c["scale"].get<double>() > 0 && c["rotation"].is_number_integer() && rotations.contains(c["rotation"].get<int>()) && (c["hdr"].is_null() || c["hdr"].is_boolean()) && one_of(c["primaryPolicy"], {"preserve", "make-target-primary", "restore-after-use"}) && one_of(c["restorePolicy"], {"always", "on-stop", "never"}) && c["virtualDisplays"].is_array();
    }

    bool stream_config(const json &c) {
      if (!exact(c, {"width", "height", "fps", "bitrateKbps", "codec", "hdr", "yuv444", "audioChannels", "hostAudio", "inputMode", "requiredInputClasses", "controllerLimit", "encryptionRequired", "gameOptimizations"}) || c.size() != 14) {
        return false;
      }
      static const std::set<std::string> classes {"keyboard", "mouse", "touch", "pen", "controller"};
      return integer(c["width"], 640, 7680) && integer(c["height"], 360, 4320) && integer(c["fps"], 1, 240) && integer(c["bitrateKbps"], 500, 500000) && one_of(c["codec"], {"automatic", "h264", "hevc", "av1"}) && c["hdr"].is_boolean() && c["yuv444"].is_boolean() && one_of(c["audioChannels"], {"stereo", "5.1", "7.1"}) && c["hostAudio"].is_boolean() && one_of(c["inputMode"], {"relative", "absolute"}) && strings(c["requiredInputClasses"]) && std::all_of(c["requiredInputClasses"].begin(), c["requiredInputClasses"].end(), [&](const json &v) {
               return classes.contains(v.get<std::string>());
             }) &&
             integer(c["controllerLimit"], 0, 16) && c["encryptionRequired"].is_boolean() && c["gameOptimizations"].is_boolean();
    }

    bool launch_config(const json &c) {
      if (!exact(c, {"appUuid", "displayProfileId", "streamProfileId", "sandboxProfileId", "arguments", "environment", "workingDirectory", "elevated", "preLaunchPolicy", "postExitPolicy", "cleanupPolicy", "resumePolicy", "concurrentLaunchPolicy"}) || c.size() != 13) {
        return false;
      }
      return c["appUuid"].is_string() && uuid(c["appUuid"]) && nullable_uuid(c["displayProfileId"]) && nullable_uuid(c["streamProfileId"]) && nullable_uuid(c["sandboxProfileId"]) && strings(c["arguments"]) && environment(c["environment"]) && (c["workingDirectory"].is_null() || c["workingDirectory"].is_string()) && c["elevated"].is_boolean() && actions(c["preLaunchPolicy"]) && actions(c["postExitPolicy"]) && one_of(c["cleanupPolicy"], {"on-failure", "on-stop", "retain"}) && one_of(c["resumePolicy"], {"allow", "deny"}) && one_of(c["concurrentLaunchPolicy"], {"deny", "reuse", "parallel"});
    }

    bool sandbox_config(json &c) {
      json normalized;
      if (!terra_sandboxes::normalize_policy(c, normalized)) {
        return false;
      }
      c = std::move(normalized);
      return true;
    }

    std::set<std::string> apps(const profile_t &p) {
      std::set<std::string> result;
      if (p.type == "launch") {
        result.emplace(p.configuration["appUuid"].get<std::string>());
      } else if (p.type == "sandbox") {
        for (const auto &id : p.configuration["allowedAppUuids"]) {
          result.emplace(id.get<std::string>());
        }
      }
      return result;
    }
  }  // namespace

  struct manager_t::impl_t {
    /** @brief Caller-visible collection revision state. */
    struct projection_t {
      std::string fingerprint;  ///< Last serialized visible collection.
      std::uint64_t revision {};  ///< Monotonic revision for this projection.
    };

    callbacks_t cb;
    mutable std::mutex mutex;
    std::map<std::string, profile_t> profiles;
    std::map<std::string, projection_t, std::less<>> projections;
    std::uint64_t revision {};
    status_t available {status_t::success};

    explicit impl_t(callbacks_t callbacks):
        cb(std::move(callbacks)) {
      if (!cb.load) {
        return;
      }
      try {
        const auto text = cb.load();
        if (!text) {
          if (!save(profiles, revision)) {
            available = status_t::unavailable;
          }
          return;
        }
        const auto doc = json::parse(*text);
        if (!exact(doc, {"version", "revision", "profiles"}) || doc["version"] != 1 || !doc["revision"].is_number_unsigned() || !doc["profiles"].is_array()) {
          available = status_t::unavailable;
          return;
        }
        revision = doc["revision"];
        for (const auto &v : doc["profiles"]) {
          try {
            if (!exact(v, {"id", "type", "name", "ownerClientUuid", "shared", "revision", "configuration"}) || v.size() != 7 || !v.contains("id") || !v.at("id").is_string() || !v.contains("type") || !v.at("type").is_string() || !v.contains("name") || !v.at("name").is_string() || v.at("name").get_ref<const std::string &>().empty() || !v.contains("ownerClientUuid") || (!v.at("ownerClientUuid").is_null() && !v.at("ownerClientUuid").is_string()) || !v.contains("shared") || !v.at("shared").is_boolean() || !v.contains("revision") || !integer(v.at("revision"), 1, std::numeric_limits<std::uint64_t>::max()) || !v.contains("configuration")) {
              throw std::invalid_argument("profile record shape");
            }
            profile_t p {v.at("id"), v.at("type"), v.at("name"), v.at("ownerClientUuid").is_null() ? std::nullopt : std::optional<std::string>(v.at("ownerClientUuid")), v.at("shared"), v.at("revision"), v.at("configuration")};
            json normalized = p.configuration;
            if (!uuid(p.id) || (p.owner_client_uuid && !uuid(*p.owner_client_uuid)) || !validate(p.type, normalized)) {
              throw std::invalid_argument("profile record");
            }
            p.configuration = std::move(normalized);
            if (!profiles.emplace(p.id, std::move(p)).second) {
              throw std::invalid_argument("duplicate profile record");
            }
          } catch (...) {
            profiles.clear();
            available = status_t::unavailable;
            return;
          }
        }
      } catch (...) {
        profiles.clear();
        available = status_t::unavailable;
      }
    }

    static bool validate(const std::string &type, json &config) {
      return type == "display" ? display_config(config) : type == "stream" ? stream_config(config) :
                                                        type == "launch"   ? launch_config(config) :
                                                        type == "sandbox"  ? sandbox_config(config) :
                                                                             false;
    }

    bool scope(const actor_t &a, const std::string &type, bool mutate) const {
      const auto has = [&](const char *s) {
        return a.scopes.contains(s);
      };
      if (type == "display") {
        return has(mutate ? "display.manage" : "display.read");
      }
      if (type == "sandbox") {
        return has("sandbox.manage");
      }
      if (type == "stream") {
        return mutate ? has("host.control") : has("catalog.read");
      }
      if (type == "launch") {
        return mutate ? has("host.control") : has("catalog.read");
      }
      return false;
    }

    bool visible(const actor_t &a, const profile_t &p) const {
      if (!scope(a, p.type, false) || !p.owner_client_uuid || (!p.shared && *p.owner_client_uuid != a.client_uuid && !a.scopes.contains("host.control"))) {
        return false;
      }
      const auto associated = apps(p);
      return a.allowed_apps.empty() || std::all_of(associated.begin(), associated.end(), [&](const auto &id) {
               return a.allowed_apps.contains(id);
             });
    }

    bool authorized(const actor_t &a, const profile_t &p) const {
      return scope(a, p.type, true) && p.owner_client_uuid && (*p.owner_client_uuid == a.client_uuid || a.scopes.contains("host.control")) && (!p.shared || a.scopes.contains("host.control"));
    }

    bool mutable_visible(const actor_t &a, const profile_t &p) const {
      if (!p.owner_client_uuid || (*p.owner_client_uuid != a.client_uuid && !a.scopes.contains("host.control"))) {
        return false;
      }
      const auto associated = apps(p);
      return a.allowed_apps.empty() || std::all_of(associated.begin(), associated.end(), [&](const auto &id) {
               return a.allowed_apps.contains(id);
             });
    }

    status_t refs(const actor_t &actor, const profile_t &p) const {
      std::vector<std::pair<std::string, std::string>> values;
      if (p.type == "launch") {
        values.emplace_back("application", p.configuration["appUuid"]);
        for (const auto &[field, kind] : std::vector<std::pair<const char *, const char *>> {{"displayProfileId", "display"}, {"streamProfileId", "stream"}, {"sandboxProfileId", "sandbox"}}) {
          if (!p.configuration[field].is_null()) {
            values.emplace_back(kind, p.configuration[field]);
          }
        }
      } else if (p.type == "sandbox") {
        for (const auto &app_uuid : p.configuration["allowedAppUuids"]) {
          values.emplace_back("application", app_uuid.get<std::string>());
        }
      }
      for (const auto &[kind, id] : values) {
        if (kind == "application") {
          if (!actor.allowed_apps.empty() && !actor.allowed_apps.contains(id)) {
            return status_t::not_found;
          }
          if (cb.validate_reference) {
            const auto result = cb.validate_reference(kind, id);
            if (result != status_t::success) {
              return result;
            }
          }
          continue;
        }
        const auto referenced = profiles.find(id);
        if (referenced == profiles.end() || referenced->second.type != kind || !visible(actor, referenced->second)) {
          return status_t::not_found;
        }
      }
      return status_t::success;
    }

    bool save(const std::map<std::string, profile_t> &candidate, std::uint64_t next) const {
      if (!cb.save) {
        return true;
      }
      json records = json::array();
      for (const auto &[id, p] : candidate) {
        records.push_back(to_json(p));
      }
      return cb.save(json({{"version", 1}, {"revision", next}, {"profiles", std::move(records)}}).dump());
    }
  };

  manager_t::manager_t(callbacks_t callbacks):
      impl_(std::make_unique<impl_t>(std::move(callbacks))) {}

  manager_t::~manager_t() = default;
  manager_t::manager_t(manager_t &&) noexcept = default;
  manager_t &manager_t::operator=(manager_t &&) noexcept = default;

  result_t manager_t::create(const actor_t &actor, const json &request) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->available != status_t::success) {
      return {impl_->available, "profile store unavailable"};
    }
    if (!uuid(actor.client_uuid) || !exact(request, {"type", "name", "shared", "configuration"}) || request.size() != 4 || !request.contains("type") || !request["type"].is_string() || !request.contains("name") || !request["name"].is_string() || request["name"].get_ref<const std::string &>().empty() || !request.contains("shared") || !request["shared"].is_boolean() || !request.contains("configuration")) {
      return {status_t::invalid, "invalid profile creation object"};
    }
    profile_t p {impl_->cb.uuid ? impl_->cb.uuid() : std::string(), request["type"], request["name"], actor.client_uuid, request["shared"], 1, request["configuration"]};
    if (!uuid(p.id) || !impl_->validate(p.type, p.configuration)) {
      return {status_t::invalid, "invalid profile configuration"};
    }
    if (!impl_->scope(actor, p.type, true) || (p.shared && !actor.scopes.contains("host.control"))) {
      return {status_t::forbidden, "profile mutation forbidden"};
    }
    const auto ref = impl_->refs(actor, p);
    if (ref != status_t::success) {
      return {ref, "profile reference unavailable"};
    }
    if (impl_->cb.provider_supports && !impl_->cb.provider_supports(p.type, p.configuration)) {
      return {status_t::unsupported, "unsupported profile configuration"};
    }
    auto candidate = impl_->profiles;
    if (!candidate.emplace(p.id, p).second) {
      return {status_t::conflict, "profile UUID collision"};
    }
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, p, {}, impl_->revision};
  }

  result_t manager_t::get(const actor_t &actor, const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id)) {
      return {status_t::invalid, "invalid profile UUID"};
    }
    const auto it = impl_->profiles.find(id);
    if (it == impl_->profiles.end() || !impl_->visible(actor, it->second)) {
      return {status_t::not_found, "profile not found"};
    }
    return {status_t::success, {}, it->second, {}, impl_->revision};
  }

  result_t manager_t::list(const actor_t &actor, const std::optional<std::string> &type) const {
    std::lock_guard lock(impl_->mutex);
    if (!actor.scopes.contains("catalog.read") && !actor.scopes.contains("display.read") && !actor.scopes.contains("sandbox.manage")) {
      return {status_t::forbidden, "profile listing forbidden"};
    }
    if (type && *type != "display" && *type != "stream" && *type != "launch" && *type != "sandbox") {
      return {status_t::invalid, "invalid profile type"};
    }
    result_t result;
    for (const auto &[id, p] : impl_->profiles) {
      if ((!type || p.type == *type) && impl_->visible(actor, p)) {
        result.profiles.push_back(p);
      }
    }
    nlohmann::json visible = nlohmann::json::array();
    for (const auto &profile : result.profiles) {
      visible.push_back(to_json(profile));
    }
    auto &projection = impl_->projections[actor.client_uuid + '\0' + type.value_or(std::string {})];
    const auto fingerprint = visible.dump();
    if (projection.revision == 0) {
      projection.revision = impl_->available == status_t::success ? impl_->revision + 1 : 1;
      if (impl_->cb.clock) {
        try {
          projection.revision = std::max(projection.revision, impl_->cb.clock());
        } catch (...) {
        }
      }
      projection.fingerprint = fingerprint;
    } else if (projection.fingerprint != fingerprint) {
      projection.fingerprint = fingerprint;
      ++projection.revision;
    }
    result.collection_revision = projection.revision;
    return result;
  }

  std::optional<std::string> manager_t::type(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id)) {
      return std::nullopt;
    }
    const auto found = impl_->profiles.find(id);
    return found == impl_->profiles.end() ? std::nullopt : std::optional<std::string> {found->second.type};
  }

  std::optional<profile_t> manager_t::inspect(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id)) {
      return std::nullopt;
    }
    const auto found = impl_->profiles.find(id);
    return found == impl_->profiles.end() ? std::nullopt : std::optional<profile_t> {found->second};
  }

  bool manager_t::launch_profile_references(const std::string &id) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto &[profile_id, profile] : impl_->profiles) {
      if (profile.type != "launch") {
        continue;
      }
      const auto &configuration = profile.configuration;
      if (!configuration.is_object()) {
        continue;
      }
      for (const char *field : {"displayProfileId", "streamProfileId", "sandboxProfileId"}) {
        const auto found = configuration.find(field);
        if (found != configuration.end() && found->is_string() && found->get<std::string>() == id) {
          return true;
        }
      }
    }
    return false;
  }

  status_t manager_t::validate_configuration(const std::string &type, const json &configuration) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->available != status_t::success) {
      return impl_->available;
    }
    auto normalized = configuration;
    if (!impl_->validate(type, normalized)) {
      return status_t::invalid;
    }
    if (impl_->cb.provider_supports && !impl_->cb.provider_supports(type, normalized)) {
      return status_t::unsupported;
    }
    return status_t::success;
  }

  result_t manager_t::patch(const actor_t &actor, const std::string &id, std::uint64_t expected, const json &request) {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id) || !exact(request, {"name", "shared", "configuration"}) || request.empty()) {
      return {status_t::invalid, "invalid profile patch"};
    }
    auto it = impl_->profiles.find(id);
    if (it == impl_->profiles.end() || !impl_->mutable_visible(actor, it->second)) {
      return {status_t::not_found, "profile not found"};
    }
    if (!impl_->authorized(actor, it->second)) {
      return {status_t::forbidden, "profile mutation forbidden"};
    }
    if (it->second.revision != expected) {
      return {status_t::conflict, "profile revision conflict"};
    }
    auto p = it->second;
    if (request.contains("name")) {
      if (!request["name"].is_string() || request["name"].get_ref<const std::string &>().empty()) {
        return {status_t::invalid, "invalid profile name"};
      }
      p.name = request["name"];
    }
    if (request.contains("shared")) {
      if (!request["shared"].is_boolean()) {
        return {status_t::invalid, "invalid shared value"};
      }
      p.shared = request["shared"];
      if (p.shared && !actor.scopes.contains("host.control")) {
        return {status_t::forbidden, "shared mutation forbidden"};
      }
    }
    if (request.contains("configuration")) {
      p.configuration = request["configuration"];
      if (!impl_->validate(p.type, p.configuration)) {
        return {status_t::invalid, "invalid profile configuration"};
      }
    }
    const auto ref = impl_->refs(actor, p);
    if (ref != status_t::success) {
      return {ref, "profile reference unavailable"};
    }
    if (impl_->cb.provider_supports && !impl_->cb.provider_supports(p.type, p.configuration)) {
      return {status_t::unsupported, "unsupported profile configuration"};
    }
    ++p.revision;
    auto candidate = impl_->profiles;
    candidate[id] = p;
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, p, {}, impl_->revision};
  }

  result_t manager_t::erase(const actor_t &actor, const std::string &id, std::uint64_t expected) {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id)) {
      return {status_t::invalid, "invalid profile UUID"};
    }
    auto it = impl_->profiles.find(id);
    if (it == impl_->profiles.end() || !impl_->mutable_visible(actor, it->second)) {
      return {status_t::not_found, "profile not found"};
    }
    if (!impl_->authorized(actor, it->second)) {
      return {status_t::forbidden, "profile deletion forbidden"};
    }
    if (it->second.revision != expected) {
      return {status_t::conflict, "profile revision conflict"};
    }
    if (impl_->cb.reference_busy && impl_->cb.reference_busy(it->second)) {
      return {status_t::resource_busy, "profile is referenced"};
    }
    auto candidate = impl_->profiles;
    candidate.erase(id);
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, std::nullopt, {}, impl_->revision};
  }

  result_t manager_t::adopt(const actor_t &actor, const std::string &id, std::uint64_t expected) {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(id) || !uuid(actor.client_uuid)) {
      return {status_t::invalid, "invalid UUID"};
    }
    auto it = impl_->profiles.find(id);
    if (it == impl_->profiles.end()) {
      return {status_t::not_found, "profile not found"};
    }
    if (it->second.owner_client_uuid) {
      return {status_t::resource_busy, "profile already owned"};
    }
    if (!actor.scopes.contains("host.control") || !impl_->scope(actor, it->second.type, true)) {
      return {status_t::forbidden, "profile adoption forbidden"};
    }
    if (const auto refs = impl_->refs(actor, it->second); refs != status_t::success) {
      return {refs, "profile reference unavailable"};
    }
    if (impl_->cb.provider_supports && !impl_->cb.provider_supports(it->second.type, it->second.configuration)) {
      return {status_t::unsupported, "unsupported profile configuration"};
    }
    if (it->second.revision != expected) {
      return {status_t::conflict, "profile revision conflict"};
    }
    auto p = it->second;
    p.owner_client_uuid = actor.client_uuid;
    ++p.revision;
    auto candidate = impl_->profiles;
    candidate[id] = p;
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, p, {}, impl_->revision};
  }

  result_t manager_t::revoke_owner(const std::string &owner) {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(owner)) {
      return {status_t::invalid, "invalid owner UUID"};
    }
    auto candidate = impl_->profiles;
    std::vector<profile_t> changed;
    for (auto &[id, profile] : candidate) {
      if (profile.owner_client_uuid == owner) {
        profile.owner_client_uuid.reset();
        ++profile.revision;
        changed.push_back(profile);
      }
    }
    if (changed.empty()) {
      return {status_t::success, {}, std::nullopt, {}, impl_->revision};
    }
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, std::nullopt, std::move(changed), impl_->revision};
  }

  result_t manager_t::revoke_unauthorized(const actor_t &actor) {
    std::lock_guard lock(impl_->mutex);
    if (!uuid(actor.client_uuid)) {
      return {status_t::invalid, "invalid owner UUID"};
    }
    auto candidate = impl_->profiles;
    std::vector<profile_t> changed;
    for (auto &[id, profile] : candidate) {
      if (profile.owner_client_uuid == actor.client_uuid && (!impl_->authorized(actor, profile) || !impl_->mutable_visible(actor, profile))) {
        profile.owner_client_uuid.reset();
        ++profile.revision;
        changed.push_back(profile);
      }
    }
    if (changed.empty()) {
      return {status_t::success, {}, std::nullopt, {}, impl_->revision};
    }
    if (!impl_->save(candidate, impl_->revision + 1)) {
      return {status_t::persistence, "profile persistence failed"};
    }
    impl_->profiles = std::move(candidate);
    ++impl_->revision;
    return {status_t::success, {}, std::nullopt, std::move(changed), impl_->revision};
  }

  status_t manager_t::availability() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->available;
  }

  json to_json(const profile_t &p) {
    return {{"id", p.id}, {"type", p.type}, {"name", p.name}, {"ownerClientUuid", p.owner_client_uuid ? json(*p.owner_client_uuid) : json(nullptr)}, {"shared", p.shared}, {"revision", p.revision}, {"configuration", p.configuration}};
  }
}  // namespace terra::profiles
