/**
 * @file src/terra_virtual_display.cpp
 * @brief Terra virtual display lifecycle implementation.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <utility>

// local includes
#include "terra_virtual_display.h"
#include "utility.h"
#include "uuid.h"

namespace terra_virtual_display {
  namespace {
    constexpr int DOCUMENT_VERSION = 1;  ///< Persistent document schema version.

    /**
     * @brief Validate lowercase canonical UUID text.
     *
     * @param value Candidate UUID.
     * @return True for canonical lowercase UUID syntax.
     */
    bool valid_uuid(const std::string &value) {
      return uuid_util::is_valid(value) && std::none_of(value.begin(), value.end(), [](const unsigned char character) {
               return character >= 'A' && character <= 'F';
             });
    }

    /**
     * @brief Convert lifecycle state to persistence text.
     *
     * @param state Lifecycle state.
     * @return Stable state name.
     */
    std::string state_name(const state_t state) {
      switch (state) {
        case state_t::provisioning:
          return "provisioning";
        case state_t::ready:
          return "ready";
        case state_t::attached:
          return "attached";
        case state_t::error:
          return "error";
        case state_t::deleting:
          return "deleting";
      }
      return {};
    }

    /**
     * @brief Parse persisted lifecycle state.
     *
     * @param value Persisted state name.
     * @return Parsed state, or no value.
     */
    std::optional<state_t> parse_state(const std::string &value) {
      if (value == "ready") {
        return state_t::ready;
      }
      if (value == "attached") {
        return state_t::attached;
      }
      if (value == "error") {
        return state_t::error;
      }
      if (value == "provisioning") {
        return state_t::provisioning;
      }
      if (value == "deleting") {
        return state_t::deleting;
      }
      return std::nullopt;
    }

    /**
     * @brief Validate mode-like fields.
     *
     * @tparam T Requested or actual mode type.
     * @param mode Mode to validate.
     * @return True when fields are supported.
     */
    template<class T>
    bool valid_mode(const T &mode) {
      return mode.width > 0 && mode.height > 0 && mode.width <= 16384 && mode.height <= 16384 && mode.refresh_numerator > 0 && mode.refresh_denominator > 0 && (mode.bit_depth == 8 || mode.bit_depth == 10 || mode.bit_depth == 12);
    }

    /**
     * @brief Validate mutable resource fields.
     *
     * @param value Display specification.
     * @return True when all fields are valid.
     */
    bool valid_specification(const specification_t &value) {
      return !value.name.empty() && valid_mode(value.mode) && std::isfinite(value.scale) && value.scale >= 0.5 && value.scale <= 4.0 && (value.rotation == 0 || value.rotation == 90 || value.rotation == 180 || value.rotation == 270) && (!value.workspace_id || valid_uuid(*value.workspace_id));
    }

    /**
     * @brief Serialize mode-like fields.
     *
     * @tparam T Requested or actual mode type.
     * @param mode Mode to serialize.
     * @return Mode JSON object.
     */
    template<class T>
    nlohmann::json mode_json(const T &mode) {
      return {{"width", mode.width}, {"height", mode.height}, {"refreshNumerator", mode.refresh_numerator}, {"refreshDenominator", mode.refresh_denominator}, {"bitDepth", mode.bit_depth}, {"hdr", mode.hdr}};
    }

    /**
     * @brief Parse requested mode fields.
     *
     * @param value Mode JSON object.
     * @return Parsed mode.
     */
    mode_t parse_mode(const nlohmann::json &value) {
      return {value.at("width").get<int>(), value.at("height").get<int>(), value.at("refreshNumerator").get<std::uint32_t>(), value.at("refreshDenominator").get<std::uint32_t>(), value.at("bitDepth").get<int>(), value.at("hdr").get<bool>()};
    }

    /**
     * @brief Parse actual mode fields and stable identity.
     *
     * @param value Actual mode JSON object.
     * @return Parsed actual mode.
     */
    actual_mode_t parse_actual_mode(const nlohmann::json &value) {
      const auto mode = parse_mode(value);
      return {mode.width, mode.height, mode.refresh_numerator, mode.refresh_denominator, mode.bit_depth, mode.hdr, value.at("id").get<std::string>()};
    }

    /**
     * @brief Extract provider specification from resource.
     *
     * @param resource Published resource.
     * @return Provider specification.
     */
    specification_t specification(const resource_t &resource) {
      return {resource.name, resource.requested_mode, resource.position, resource.scale, resource.rotation, resource.primary, resource.hdr, resource.persistent, resource.workspace_id};
    }
  }  // namespace

  struct manager_t::impl_t {
    callbacks_t callbacks;  ///< Injected services.
    mutable std::mutex mutex;  ///< Serializes state and provider operations.
    bool usable = false;  ///< Whether initialization succeeded.
    std::uint32_t baseline_count = 0;  ///< Immutable provider count floor.
    std::set<std::string> baseline_ids;  ///< Immutable baseline connector inventory.
    std::map<std::string, resource_t, std::less<>> resources;  ///< Resources by API UUID.
    std::uint64_t collection_revision = 0;  ///< Monotonic collection revision.

    /**
     * @brief Serialize complete persistent state.
     *
     * @param values Candidate resources.
     * @param revision Candidate collection revision.
     * @return Versioned persistence document.
     */
    nlohmann::json document(const decltype(resources) &values, const std::uint64_t revision) const {
      nlohmann::json records = nlohmann::json::array();
      for (const auto &[id, resource] : values) {
        auto value = to_json(resource);
        value["platformId"] = resource.platform_id;
        records.push_back(std::move(value));
      }
      return {{"version", DOCUMENT_VERSION}, {"baselineCount", baseline_count}, {"baselineInventory", baseline_ids}, {"collectionRevision", revision}, {"resources", std::move(records)}};
    }

    /**
     * @brief Persist candidate state.
     *
     * @param values Candidate resources.
     * @param revision Candidate collection revision.
     * @return True when persistence accepted document.
     */
    bool save(const decltype(resources) &values, const std::uint64_t revision) const {
      try {
        return callbacks.save && callbacks.save(document(values, revision).dump());
      } catch (...) {
        return false;
      }
    }

    /**
     * @brief Read and validate provider count and inventory atomically under manager lock.
     *
     * @return Provider count and unique inventory, or no value on failure.
     */
    std::optional<std::pair<std::uint32_t, std::set<std::string>>> provider_state() const {
      try {
        if (!callbacks.provider_healthy || !callbacks.provider_healthy()) {
          return std::nullopt;
        }
        const auto count = callbacks.read_count ? callbacks.read_count() : std::nullopt;
        const auto inventory = callbacks.inventory ? callbacks.inventory() : std::nullopt;
        if (!count || !inventory || inventory->size() != *count) {
          return std::nullopt;
        }
        std::set<std::string> ids(inventory->begin(), inventory->end());
        if (ids.size() != inventory->size()) {
          return std::nullopt;
        }
        return std::pair {*count, std::move(ids)};
      } catch (...) {
        return std::nullopt;
      }
    }

    /**
     * @brief Check completed provider count and inventory consistency.
     *
     * @param count Expected count.
     * @return True when provider reports expected consistent state.
     */
    bool count_matches(const std::uint32_t count) const {
      const auto state = provider_state();
      return state && state->first == count;
    }

    /**
     * @brief Apply complete managed configuration and collect actual modes.
     *
     * @param values Candidate resources updated with actual modes.
     * @return True when provider applied and returned valid results.
     */
    bool apply(decltype(resources) &values) const {
      std::vector<platform_configuration_t> configurations;
      for (const auto &[id, resource] : values) {
        configurations.push_back({resource.platform_id, specification(resource), resource.actual_mode});
      }
      try {
        if (!callbacks.apply_configuration || !callbacks.apply_configuration(configurations) || configurations.size() != values.size()) {
          return false;
        }
      } catch (...) {
        return false;
      }
      std::size_t index = 0;
      for (auto &[id, resource] : values) {
        if (configurations[index].platform_id != resource.platform_id || configurations[index].actual_mode.id.empty() || !valid_mode(configurations[index].actual_mode)) {
          return false;
        }
        resource.actual_mode = configurations[index++].actual_mode;
      }
      return true;
    }

    /**
     * @brief Restore count and published provider configuration.
     *
     * @param count Count before failed mutation.
     * @return True when rollback completed.
     */
    bool rollback(const std::uint32_t count) const {
      try {
        if (!callbacks.set_count || !callbacks.set_count(count) || !count_matches(count)) {
          return false;
        }
        auto old = resources;
        return old.empty() || apply(old);
      } catch (...) {
        return false;
      }
    }

    /**
     * @brief Restore provider state or prevent further mutations after divergence.
     *
     * @param count Connector count before failed mutation.
     */
    void rollback_or_disable(const std::uint32_t count) {
      if (!rollback(count)) {
        usable = false;
      }
    }

    /**
     * @brief Reapply published configuration or prevent further mutations.
     */
    void reapply_or_disable() {
      auto old = resources;
      if (!old.empty() && !apply(old)) {
        usable = false;
      }
    }
  };

  manager_t::manager_t(callbacks_t callbacks):
      impl_(std::make_unique<impl_t>()) {
    impl_->callbacks = std::move(callbacks);
    const auto provider = impl_->provider_state();
    if (!provider) {
      return;
    }
    try {
      const auto text = impl_->callbacks.load ? impl_->callbacks.load() : std::nullopt;
      if (!text) {
        impl_->baseline_count = provider->first;
        impl_->baseline_ids = provider->second;
        impl_->usable = impl_->save(impl_->resources, 0);
        return;
      }
      const auto doc = nlohmann::json::parse(*text);
      if (!doc.is_object() || doc.at("version") != DOCUMENT_VERSION) {
        return;
      }
      impl_->baseline_count = doc.at("baselineCount").get<std::uint32_t>();
      impl_->baseline_ids = doc.at("baselineInventory").get<std::set<std::string>>();
      impl_->collection_revision = doc.at("collectionRevision").get<std::uint64_t>();
      if (impl_->baseline_ids.size() != impl_->baseline_count || !std::includes(provider->second.begin(), provider->second.end(), impl_->baseline_ids.begin(), impl_->baseline_ids.end())) {
        return;
      }
      for (const auto &value : doc.at("resources")) {
        const auto parsed_state = parse_state(value.at("state").get<std::string>());
        if (!parsed_state) {
          return;
        }
        resource_t resource {
          value.at("id").get<std::string>(),
          value.at("name").get<std::string>(),
          std::nullopt,
          parsed_state.value(),
          parse_mode(value.at("requestedMode")),
          {},
          {value.at("position").at("x").get<int>(), value.at("position").at("y").get<int>()},
          value.at("scale").get<double>(),
          value.at("rotation").get<int>(),
          value.at("primary").get<bool>(),
          value.at("hdr").get<bool>(),
          value.at("persistent").get<bool>(),
          std::nullopt,
          std::nullopt,
          value.at("error"),
          value.at("revision").get<std::uint64_t>(),
          value.at("platformId").get<std::string>()
        };
        resource.actual_mode = parse_actual_mode(value.at("actualMode"));
        if (!value.at("ownerClientUuid").is_null()) {
          resource.owner_client_uuid = value.at("ownerClientUuid").get<std::string>();
        }
        if (!value.at("workspaceId").is_null()) {
          resource.workspace_id = value.at("workspaceId").get<std::string>();
        }
        if (!value.at("sessionId").is_null()) {
          resource.session_id = value.at("sessionId").get<std::string>();
        }
        const bool invalid_attachment = resource.state == state_t::attached ? resource.session_id.has_value() == resource.workspace_id.has_value() : resource.session_id.has_value();
        if (!valid_uuid(resource.id) || (resource.owner_client_uuid && !valid_uuid(*resource.owner_client_uuid)) || !valid_specification(specification(resource)) || resource.actual_mode.id.empty() || !valid_mode(resource.actual_mode) || resource.revision == 0 || impl_->baseline_ids.contains(resource.platform_id) || !provider->second.contains(resource.platform_id) || resource.state == state_t::provisioning || resource.state == state_t::deleting || invalid_attachment || (resource.session_id && !valid_uuid(*resource.session_id)) || !impl_->resources.emplace(resource.id, resource).second) {
          return;
        }
      }
      std::set<std::string> managed;
      for (const auto &[id, resource] : impl_->resources) {
        if (!managed.emplace(resource.platform_id).second) {
          return;
        }
      }
      if (provider->first != impl_->baseline_count + impl_->resources.size()) {
        return;
      }
      // Ephemeral records are restart-owned cleanup, never published again.
      for (auto iterator = impl_->resources.begin(); iterator != impl_->resources.end();) {
        if (!iterator->second.persistent) {
          iterator = impl_->resources.erase(iterator);
        } else {
          ++iterator;
        }
      }
      for (auto &[id, resource] : impl_->resources) {
        if (resource.state == state_t::attached) {
          resource.state = state_t::ready;
          resource.session_id.reset();
          resource.workspace_id.reset();
          ++resource.revision;
        }
      }
      if (provider->first != impl_->baseline_count + impl_->resources.size()) {
        const auto target = static_cast<std::uint32_t>(impl_->baseline_count + impl_->resources.size());
        if (!impl_->callbacks.set_count || !impl_->callbacks.set_count(target)) {
          return;
        }
        const auto reconciled = impl_->provider_state();
        if (!reconciled || reconciled->first != target) {
          return;
        }
        std::set<std::string> assigned;
        for (const auto &[id, resource] : impl_->resources) {
          if (reconciled->second.contains(resource.platform_id)) {
            assigned.emplace(resource.platform_id);
          }
        }
        std::vector<std::string> replacements;
        std::set_difference(reconciled->second.begin(), reconciled->second.end(), impl_->baseline_ids.begin(), impl_->baseline_ids.end(), std::back_inserter(replacements));
        std::erase_if(replacements, [&](const auto &platform_id) {
          return assigned.contains(platform_id);
        });
        for (auto &[id, resource] : impl_->resources) {
          if (!reconciled->second.contains(resource.platform_id)) {
            if (replacements.empty()) {
              return;
            }
            resource.platform_id = replacements.back();
            replacements.pop_back();
            ++resource.revision;
          }
        }
      }
      if (!impl_->apply(impl_->resources)) {
        return;
      }
      ++impl_->collection_revision;
      if (!impl_->save(impl_->resources, impl_->collection_revision)) {
        return;
      }
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

  result_t manager_t::create(const std::string &owner, const specification_t &spec) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    if (!valid_uuid(owner) || !valid_specification(spec) || spec.hdr != spec.mode.hdr) {
      return {status_t::invalid, std::nullopt};
    }
    std::string resource_id;
    try {
      resource_id = impl_->callbacks.uuid ? impl_->callbacks.uuid() : std::string {};
      std::ranges::transform(resource_id, resource_id.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
    } catch (...) {
      return {status_t::invalid, std::nullopt};
    }
    if (!valid_uuid(resource_id) || impl_->resources.contains(resource_id)) {
      return {status_t::invalid, std::nullopt};
    }
    const auto before = impl_->provider_state();
    const auto expected = static_cast<std::uint32_t>(impl_->baseline_count + impl_->resources.size());
    if (!before || before->first != expected) {
      return {status_t::provider_error, std::nullopt};
    }
    try {
      if (!impl_->callbacks.set_count || !impl_->callbacks.set_count(expected + 1)) {
        impl_->rollback_or_disable(expected);
        return {status_t::provider_error, std::nullopt};
      }
    } catch (...) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    const auto after = impl_->provider_state();
    std::vector<std::string> added;
    if (after) {
      std::set_difference(after->second.begin(), after->second.end(), before->second.begin(), before->second.end(), std::back_inserter(added));
    }
    if (!after || after->first != expected + 1 || added.size() != 1) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    resource_t resource {resource_id, spec.name, owner, state_t::ready, spec.mode, {}, spec.position, spec.scale, spec.rotation, spec.primary, spec.hdr, spec.persistent, spec.workspace_id, std::nullopt, nullptr, 1, added.front()};
    auto candidate = impl_->resources;
    candidate.emplace(resource.id, resource);
    if (!impl_->apply(candidate)) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->rollback_or_disable(expected);
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, impl_->resources.at(resource.id)};
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

  result_t manager_t::patch(const std::string &id, const std::uint64_t expected_revision, const patch_t &patch_value) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state == state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    const auto provider = impl_->provider_state();
    if (!provider || provider->first != impl_->baseline_count + impl_->resources.size()) {
      return {status_t::provider_error, std::nullopt};
    }
    auto candidate = impl_->resources;
    auto &resource = candidate.at(id);
    if (patch_value.name) {
      resource.name = *patch_value.name;
    }
    if (patch_value.mode) {
      resource.requested_mode = *patch_value.mode;
    }
    if (patch_value.position) {
      resource.position = *patch_value.position;
    }
    if (patch_value.scale) {
      resource.scale = *patch_value.scale;
    }
    if (patch_value.rotation) {
      resource.rotation = *patch_value.rotation;
    }
    if (patch_value.primary) {
      resource.primary = *patch_value.primary;
    }
    if (patch_value.hdr) {
      resource.hdr = *patch_value.hdr;
    }
    if (patch_value.persistent) {
      resource.persistent = *patch_value.persistent;
    }
    if (patch_value.workspace_id) {
      resource.workspace_id = *patch_value.workspace_id;
    }
    if (!valid_specification(specification(resource)) || resource.hdr != resource.requested_mode.hdr) {
      return {status_t::invalid, std::nullopt};
    }
    ++resource.revision;
    if (!impl_->apply(candidate)) {
      impl_->reapply_or_disable();
      return {status_t::provider_error, std::nullopt};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->reapply_or_disable();
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, impl_->resources.at(id)};
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
    if (found->second.revision != expected_revision || found->second.state == state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    const auto before = impl_->provider_state();
    const auto expected = static_cast<std::uint32_t>(impl_->baseline_count + impl_->resources.size());
    if (!before || before->first != expected) {
      return {status_t::provider_error, std::nullopt};
    }
    try {
      if (!impl_->callbacks.set_count || !impl_->callbacks.set_count(expected - 1)) {
        impl_->rollback_or_disable(expected);
        return {status_t::provider_error, std::nullopt};
      }
    } catch (...) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    const auto after = impl_->provider_state();
    std::vector<std::string> removed;
    if (after) {
      std::set_difference(before->second.begin(), before->second.end(), after->second.begin(), after->second.end(), std::back_inserter(removed));
    }
    if (!after || after->first != expected - 1 || removed.size() != 1 || impl_->baseline_ids.contains(removed.front())) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    auto candidate = impl_->resources;
    const auto target_platform = candidate.at(id).platform_id;
    candidate.erase(id);
    if (removed.front() != target_platform) {
      auto survivor = std::find_if(candidate.begin(), candidate.end(), [&](const auto &item) {
        return item.second.platform_id == removed.front();
      });
      if (survivor == candidate.end() || !after->second.contains(target_platform)) {
        impl_->rollback_or_disable(expected);
        return {status_t::provider_error, std::nullopt};
      }
      survivor->second.platform_id = target_platform;
      ++survivor->second.revision;
    }
    if (!candidate.empty() && !impl_->apply(candidate)) {
      impl_->rollback_or_disable(expected);
      return {status_t::provider_error, std::nullopt};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->rollback_or_disable(expected);
      return {status_t::persistence_error, std::nullopt};
    }
    const auto deleted = found->second;
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, deleted};
  }

  result_t manager_t::attach(const std::string &id, const std::uint64_t expected_revision, const attachment_t &attachment) {
    std::scoped_lock lock {impl_->mutex};
    if (attachment.session_id.has_value() == attachment.workspace_id.has_value() || (attachment.session_id && !valid_uuid(*attachment.session_id)) || (attachment.workspace_id && !valid_uuid(*attachment.workspace_id))) {
      return {status_t::invalid, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::ready) {
      return {status_t::conflict, std::nullopt};
    }
    auto candidate = impl_->resources;
    auto &resource = candidate.at(id);
    resource.state = state_t::attached;
    resource.session_id = attachment.session_id;
    resource.workspace_id = attachment.workspace_id;
    ++resource.revision;
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, impl_->resources.at(id)};
  }

  result_t manager_t::detach(const std::string &id, const std::uint64_t expected_revision) {
    std::scoped_lock lock {impl_->mutex};
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (!impl_->usable) {
      return {status_t::unavailable, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    auto candidate = impl_->resources;
    auto &resource = candidate.at(id);
    resource.state = state_t::ready;
    resource.session_id.reset();
    resource.workspace_id.reset();
    ++resource.revision;
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, impl_->resources.at(id)};
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
    if (found->second.revision != expected_revision || found->second.owner_client_uuid || found->second.state == state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    auto candidate = impl_->resources;
    auto &resource = candidate.at(id);
    resource.owner_client_uuid = owner;
    ++resource.revision;
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return {status_t::success, impl_->resources.at(id)};
  }

  status_t manager_t::revoke_owner(const std::string &owner) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable) {
      return status_t::unavailable;
    }
    if (!valid_uuid(owner)) {
      return status_t::invalid;
    }
    auto candidate = impl_->resources;
    bool changed = false;
    std::vector<std::string> ephemeral_ids;
    for (auto &[id, resource] : candidate) {
      if (resource.owner_client_uuid && *resource.owner_client_uuid == owner) {
        changed = true;
        if (!resource.persistent) {
          ephemeral_ids.push_back(id);
          continue;
        }
        resource.state = state_t::ready;
        resource.session_id.reset();
        resource.workspace_id.reset();
        resource.owner_client_uuid.reset();
        ++resource.revision;
      }
    }
    if (!changed) {
      return status_t::success;
    }
    const auto expected = static_cast<std::uint32_t>(impl_->baseline_count + impl_->resources.size());
    if (!ephemeral_ids.empty()) {
      const auto before = impl_->provider_state();
      if (!before || before->first != expected) {
        return status_t::provider_error;
      }
      const auto target = expected - static_cast<std::uint32_t>(ephemeral_ids.size());
      try {
        if (!impl_->callbacks.set_count || !impl_->callbacks.set_count(target)) {
          impl_->rollback_or_disable(expected);
          return status_t::provider_error;
        }
      } catch (...) {
        impl_->rollback_or_disable(expected);
        return status_t::provider_error;
      }
      const auto after = impl_->provider_state();
      std::vector<std::string> removed;
      if (after) {
        std::set_difference(before->second.begin(), before->second.end(), after->second.begin(), after->second.end(), std::back_inserter(removed));
      }
      if (!after || after->first != target || removed.size() != ephemeral_ids.size() || std::ranges::any_of(removed, [&](const auto &platform_id) {
            return impl_->baseline_ids.contains(platform_id);
          })) {
        impl_->rollback_or_disable(expected);
        return status_t::provider_error;
      }
      for (const auto &id : ephemeral_ids) {
        candidate.erase(id);
      }
      std::set<std::string> assigned;
      for (const auto &[id, resource] : candidate) {
        if (after->second.contains(resource.platform_id)) {
          assigned.emplace(resource.platform_id);
        }
      }
      std::vector<std::string> replacements;
      std::set_difference(after->second.begin(), after->second.end(), impl_->baseline_ids.begin(), impl_->baseline_ids.end(), std::back_inserter(replacements));
      std::erase_if(replacements, [&](const auto &platform_id) {
        return assigned.contains(platform_id);
      });
      for (auto &[id, resource] : candidate) {
        if (!after->second.contains(resource.platform_id)) {
          if (replacements.empty()) {
            impl_->rollback_or_disable(expected);
            return status_t::provider_error;
          }
          resource.platform_id = replacements.back();
          replacements.pop_back();
          ++resource.revision;
        }
      }
      if (!replacements.empty() || (!candidate.empty() && !impl_->apply(candidate))) {
        impl_->rollback_or_disable(expected);
        return status_t::provider_error;
      }
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      if (!ephemeral_ids.empty()) {
        impl_->rollback_or_disable(expected);
      }
      return status_t::persistence_error;
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    return status_t::success;
  }

  nlohmann::json to_json(const resource_t &resource) {
    auto actual = mode_json(resource.actual_mode);
    actual["id"] = resource.actual_mode.id;
    return {{"id", resource.id}, {"name", resource.name}, {"ownerClientUuid", resource.owner_client_uuid ? nlohmann::json(*resource.owner_client_uuid) : nlohmann::json(nullptr)}, {"state", state_name(resource.state)}, {"requestedMode", mode_json(resource.requested_mode)}, {"actualMode", std::move(actual)}, {"position", {{"x", resource.position.x}, {"y", resource.position.y}}}, {"scale", resource.scale}, {"rotation", resource.rotation}, {"primary", resource.primary}, {"hdr", resource.hdr}, {"persistent", resource.persistent}, {"workspaceId", resource.workspace_id ? nlohmann::json(*resource.workspace_id) : nlohmann::json(nullptr)}, {"sessionId", resource.session_id ? nlohmann::json(*resource.session_id) : nlohmann::json(nullptr)}, {"error", resource.error}, {"revision", resource.revision}};
  }
}  // namespace terra_virtual_display
