/**
 * @file src/terra_virtual_display.cpp
 * @brief Terra virtual display lifecycle implementation.
 */

// standard includes
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// local includes
#include "logging.h"
#include "terra_virtual_display.h"
#include "utility.h"
#include "uuid.h"

namespace terra_virtual_display {
  namespace {
    constexpr int DOCUMENT_VERSION = 2;  ///< Persistent document schema version.
    constexpr int LEGACY_DOCUMENT_VERSION = 1;  ///< Pre-slot persistence schema version.

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
      return mode.width > 0 && mode.height > 0 && mode.width <= 16384 && mode.height <= 16384 && mode.refresh_numerator > 0 && mode.refresh_denominator > 0 && (mode.bit_depth == 8 || mode.bit_depth == 10);
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
     * @brief Validate workspace layout invariants before provider mutation.
     *
     * @param values Requested display specifications.
     * @return True for one to four non-overlapping displays with one origin primary.
     */
    bool valid_batch(const std::vector<specification_t> &values) {
      if (values.empty() || values.size() > 4 || std::ranges::count(values, true, &specification_t::primary) != 1) {
        return false;
      }
      const auto primary = std::ranges::find(values, true, &specification_t::primary);
      if (primary->position.x != 0 || primary->position.y != 0) {
        return false;
      }
      for (auto left = values.begin(); left != values.end(); ++left) {
        if (!valid_specification(*left) || left->hdr != left->mode.hdr) {
          return false;
        }
        const std::int64_t left_width = left->rotation % 180 == 0 ? left->mode.width : left->mode.height;
        const std::int64_t left_height = left->rotation % 180 == 0 ? left->mode.height : left->mode.width;
        for (auto right = std::next(left); right != values.end(); ++right) {
          const std::int64_t right_width = right->rotation % 180 == 0 ? right->mode.width : right->mode.height;
          const std::int64_t right_height = right->rotation % 180 == 0 ? right->mode.height : right->mode.width;
          const bool overlap = std::int64_t {left->position.x} < std::int64_t {right->position.x} + right_width && std::int64_t {right->position.x} < std::int64_t {left->position.x} + left_width && std::int64_t {left->position.y} < std::int64_t {right->position.y} + right_height && std::int64_t {right->position.y} < std::int64_t {left->position.y} + left_height;
          if (overlap) {
            return false;
          }
        }
      }
      return true;
    }

    /**
     * @brief Compare provider-confirmed mode with requested mode by normalized rational refresh.
     *
     * @param requested Requested mode.
     * @param actual Provider-confirmed mode.
     * @return True when resolution, refresh ratio, bit depth, and HDR match exactly.
     */
    bool mode_matches(const mode_t &requested, const actual_mode_t &actual) {
      return requested.width == actual.width && requested.height == actual.height && requested.bit_depth == actual.bit_depth && requested.hdr == actual.hdr && static_cast<std::uint64_t>(requested.refresh_numerator) * actual.refresh_denominator == static_cast<std::uint64_t>(actual.refresh_numerator) * requested.refresh_denominator;
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

    /**
     * @brief Parse one version-two persisted resource.
     *
     * @param value Persisted resource object.
     * @param max_count Maximum provider connector count.
     * @return Parsed resource, or no value when invalid.
     */
    std::optional<resource_t> parse_resource_v2(const nlohmann::json &value, const std::uint32_t max_count) {
      const auto parsed_state = parse_state(value.at("state").get<std::string>());
      if (!parsed_state) {
        return std::nullopt;
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
        value.at("slot").get<std::uint32_t>(),
        {}
      };
      resource.actual_mode = parse_actual_mode(value.at("actualMode"));
      resource.actual_mode.position = resource.position;
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
      if (!valid_uuid(resource.id) || (resource.owner_client_uuid && !valid_uuid(*resource.owner_client_uuid)) || !valid_specification(specification(resource)) || resource.actual_mode.id.empty() || !valid_mode(resource.actual_mode) || resource.revision == 0 || resource.slot >= max_count || resource.state == state_t::provisioning || resource.state == state_t::deleting || invalid_attachment || (resource.session_id && !valid_uuid(*resource.session_id))) {
        return std::nullopt;
      }
      return resource;
    }

    /**
     * @brief Parse one legacy persisted resource and assign a free slot.
     *
     * @param value Legacy persisted resource object.
     * @param slot Slot assigned to the migrated resource.
     * @param max_count Maximum provider connector count.
     * @return Parsed resource, or no value when invalid.
     */
    std::optional<resource_t> parse_resource_v1(const nlohmann::json &value, const std::uint32_t slot, const std::uint32_t max_count) {
      auto migrated = value;
      migrated["slot"] = slot;
      return parse_resource_v2(migrated, max_count);
    }

    /**
     * @brief Build a comparable JSON snapshot of a resource collection.
     *
     * @param values Resources to serialize.
     * @return Sorted JSON array.
     */
    nlohmann::json collection_json(const std::map<std::string, resource_t, std::less<>> &values) {
      nlohmann::json records = nlohmann::json::array();
      for (const auto &[id, resource] : values) {
        auto record = to_json(resource);
        record["slot"] = resource.slot;
        records.push_back(std::move(record));
      }
      return records;
    }
  }  // namespace

  struct manager_t::impl_t {
    callbacks_t callbacks;  ///< Injected services.
    mutable std::mutex mutex;  ///< Serializes state and provider operations.
    bool usable = false;  ///< Whether initialization succeeded.
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
        value["slot"] = resource.slot;
        records.push_back(std::move(value));
      }
      return {{"version", DOCUMENT_VERSION}, {"collectionRevision", revision}, {"resources", std::move(records)}};
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
     * @brief Read and validate provider inventory under manager lock.
     *
     * @return Provider connectors, or no value on failure.
     */
    std::optional<std::vector<connector_t>> provider_state() const {
      try {
        if (!callbacks.provider_healthy || !callbacks.provider_healthy()) {
          BOOST_LOG(error) << "Terra virtual display provider_state: provider unhealthy";
          return std::nullopt;
        }
        const auto inventory = callbacks.inventory ? callbacks.inventory() : std::nullopt;
        if (!inventory || inventory->size() > callbacks.max_count) {
          BOOST_LOG(error) << "Terra virtual display provider_state: inventory unavailable or oversized";
          return std::nullopt;
        }
        std::set<std::uint32_t> slots;
        std::set<std::string> ids;
        for (const auto &connector : *inventory) {
          if (connector.slot >= callbacks.max_count || connector.platform_id.empty() || !slots.emplace(connector.slot).second || !ids.emplace(connector.platform_id).second) {
            BOOST_LOG(error) << "Terra virtual display provider_state: duplicate or invalid connector";
            return std::nullopt;
          }
        }
        return inventory;
      } catch (...) {
        BOOST_LOG(error) << "Terra virtual display provider_state: threw";
        return std::nullopt;
      }
    }

    /**
     * @brief Check that provider inventory contains exactly the managed slots.
     *
     * @return True when provider state matches the published resources.
     */
    bool provider_slots_match() const {
      const auto provider = provider_state();
      if (!provider || provider->size() != resources.size()) {
        return false;
      }
      for (const auto &connector : *provider) {
        const auto found = std::ranges::find_if(resources, [&](const auto &entry) {
          return entry.second.slot == connector.slot;
        });
        if (found == resources.end()) {
          return false;
        }
      }
      return true;
    }

    /**
     * @brief Return the lowest free connector slot.
     *
     * @return Free slot, or no value when capacity is exhausted.
     */
    std::optional<std::uint32_t> free_slot() const {
      for (std::uint32_t slot = 0; slot < callbacks.max_count; ++slot) {
        if (std::ranges::none_of(resources, [&](const auto &entry) {
              return entry.second.slot == slot;
            })) {
          return slot;
        }
      }
      return std::nullopt;
    }

    /**
     * @brief Apply complete managed topology and collect actual modes.
     *
     * @param values Candidate resources updated with connector identifiers and actual modes.
     * @param published Published resources before the transaction, or null.
     * @return True when provider applied and returned valid results.
     */
    bool apply(decltype(resources) &values, const decltype(resources) *published = nullptr) const {
      std::vector<platform_configuration_t> configurations;
      configurations.reserve(values.size());
      for (const auto &[id, resource] : values) {
        configurations.push_back({resource.slot, resource.platform_id, specification(resource), resource.actual_mode});
      }
      try {
        if (!callbacks.apply_configuration || !callbacks.apply_configuration(configurations) || configurations.size() != values.size()) {
          return false;
        }
      } catch (...) {
        return false;
      }

      std::map<std::uint32_t, const platform_configuration_t *, std::less<>> by_slot;
      for (const auto &configuration : configurations) {
        if (configuration.platform_id.empty() || configuration.actual_mode.id.empty() || !valid_mode(configuration.actual_mode) || !by_slot.emplace(configuration.slot, &configuration).second) {
          return false;
        }
      }
      const auto provider = provider_state();
      if (!provider || provider->size() != configurations.size()) {
        return false;
      }
      for (const auto &connector : *provider) {
        if (!by_slot.contains(connector.slot)) {
          return false;
        }
      }

      for (auto &[id, resource] : values) {
        const auto found = by_slot.find(resource.slot);
        if (found == by_slot.end()) {
          return false;
        }
        const auto &configuration = *found->second;
        if (!mode_matches(resource.requested_mode, configuration.actual_mode)) {
          return false;
        }
        if (published) {
          const auto published_resource = published->find(id);
          if (published_resource != published->end() && resource.revision == published_resource->second.revision && (resource.position.x != configuration.actual_mode.position.x || resource.position.y != configuration.actual_mode.position.y || (!resource.platform_id.empty() && resource.platform_id != configuration.platform_id))) {
            ++resource.revision;
          }
        }
        resource.platform_id = configuration.platform_id;
        resource.actual_mode = configuration.actual_mode;
        resource.position = resource.actual_mode.position;
      }
      return true;
    }

    /**
     * @brief Report every resource changed by one committed manager transaction.
     *
     * @param before Published resources before transaction.
     * @param after Published resources after transaction.
     */
    void notify(const decltype(resources) &before, const decltype(resources) &after) const {
      if (!callbacks.changed) {
        return;
      }
      try {
        for (const auto &[id, previous] : before) {
          const auto current = after.find(id);
          if (current == after.end()) {
            callbacks.changed(previous, std::nullopt);
          } else if (previous.platform_id != current->second.platform_id || to_json(previous) != to_json(current->second)) {
            callbacks.changed(previous, current->second);
          }
        }
        for (const auto &[id, current] : after) {
          if (!before.contains(id)) {
            callbacks.changed(std::nullopt, current);
          }
        }
      } catch (...) {
      }
    }

    /**
     * @brief Reapply a previous resource collection and restore host configuration.
     *
     * @param previous Resources to reapply.
     * @return True when rollback completed.
     */
    bool rollback(const decltype(resources) &previous) const {
      bool reapplied = false;
      try {
        auto old = previous;
        reapplied = callbacks.apply_configuration && apply(old);
      } catch (...) {
      }
      bool restored = false;
      try {
        restored = !callbacks.restore_configuration || callbacks.restore_configuration();
      } catch (...) {
      }
      return reapplied && restored;
    }

    /**
     * @brief Restore provider state or prevent further mutations after divergence.
     *
     * @param previous Resources before failed mutation.
     */
    void rollback_or_disable(const decltype(resources) &previous) {
      if (!rollback(previous)) {
        usable = false;
      }
    }

    /**
     * @brief Reapply published configuration or prevent further mutations.
     */
    void reapply_or_disable() {
      if (!rollback(resources)) {
        usable = false;
      }
    }

    /** @brief Query live provider health without changing sticky manager state. @return True when provider accepts mutations. */
    bool healthy() const {
      try {
        return !callbacks.provider_healthy || callbacks.provider_healthy();
      } catch (...) {
        return false;
      }
    }

    /** @brief Capture provider rollback state before a mutation. @return True when rollback state is available. */
    bool capture_configuration() const {
      try {
        return !callbacks.capture_configuration || callbacks.capture_configuration();
      } catch (...) {
        return false;
      }
    }

    /** @brief Restore captured host display configuration. @return True when restoration completed. */
    bool restore_configuration() const {
      try {
        return !callbacks.restore_configuration || callbacks.restore_configuration();
      } catch (...) {
        return false;
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
        // No persisted resources: clear any stale driver connectors so Sol is authoritative.
        if (!impl_->capture_configuration()) {
          return;
        }
        decltype(impl_->resources) empty;
        if (!impl_->apply(empty)) {
          static_cast<void>(impl_->restore_configuration());
          return;
        }
        impl_->usable = impl_->save(empty, 0);
        return;
      }

      auto doc = nlohmann::json::parse(*text);
      if (!doc.is_object()) {
        return;
      }
      const auto version = doc.at("version").get<int>();
      std::optional<decltype(impl_->resources)> parsed;
      if (version == DOCUMENT_VERSION && doc.at("resources").is_array()) {
        decltype(impl_->resources) loaded;
        std::set<std::uint32_t> slots;
        for (const auto &value : doc.at("resources")) {
          const auto resource = parse_resource_v2(value, impl_->callbacks.max_count);
          if (!resource || !slots.emplace(resource->slot).second || !loaded.emplace(resource->id, *resource).second) {
            return;
          }
        }
        parsed = std::move(loaded);
      } else if (version == LEGACY_DOCUMENT_VERSION && doc.at("resources").is_array()) {
        // Migrate persisted resources in identifier order so slot assignment is deterministic.
        std::map<std::string, nlohmann::json, std::less<>> legacy;
        for (const auto &value : doc.at("resources")) {
          if (!value.is_object() || !value.contains("id") || !value.at("id").is_string() || !legacy.emplace(value.at("id").get<std::string>(), value).second) {
            return;
          }
        }
        decltype(impl_->resources) loaded;
        std::uint32_t next_slot = 0;
        for (const auto &[id, value] : legacy) {
          if (next_slot >= impl_->callbacks.max_count) {
            return;
          }
          const auto resource = parse_resource_v1(value, next_slot, impl_->callbacks.max_count);
          if (!resource) {
            return;
          }
          loaded.emplace(resource->id, *resource);
          ++next_slot;
        }
        parsed = std::move(loaded);
      } else {
        return;
      }
      impl_->collection_revision = doc.value("collectionRevision", std::uint64_t {0});
      impl_->resources = std::move(*parsed);

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

      const auto before = impl_->resources;
      bool reconciled = false;
      auto restore = util::fail_guard([&]() {
        if (reconciled) {
          return;
        }
        static_cast<void>(impl_->restore_configuration());
      });
      if (!impl_->capture_configuration() || !impl_->apply(impl_->resources, &before)) {
        impl_->resources.clear();
        return;
      }

      const bool changed = version != DOCUMENT_VERSION || collection_json(before) != collection_json(impl_->resources);
      ++impl_->collection_revision;
      if (!impl_->save(impl_->resources, impl_->collection_revision)) {
        impl_->resources.clear();
        return;
      }
      reconciled = true;
      impl_->usable = true;
      if (changed) {
        impl_->notify(before, impl_->resources);
      }
    } catch (...) {
      impl_->resources.clear();
    }
  }

  manager_t::~manager_t() = default;

  bool manager_t::available() const {
    std::scoped_lock lock {impl_->mutex};
    return impl_->usable && impl_->healthy();
  }

  std::uint32_t manager_t::max_active() const {
    std::scoped_lock lock {impl_->mutex};
    return impl_->callbacks.max_count > impl_->resources.size() ? static_cast<std::uint32_t>(impl_->callbacks.max_count - impl_->resources.size()) : 0;
  }

  result_t manager_t::create(const std::string &owner, const specification_t &spec) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
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
    if (!impl_->provider_slots_match()) {
      BOOST_LOG(error) << "Terra virtual display create: provider topology drift";
      return {status_t::provider_error, std::nullopt};
    }
    const auto slot = impl_->free_slot();
    if (!slot) {
      return {status_t::limit_reached, std::nullopt};
    }
    const auto previous = impl_->resources;
    auto candidate = impl_->resources;
    resource_t resource {resource_id, spec.name, owner, state_t::ready, spec.mode, {}, spec.position, spec.scale, spec.rotation, spec.primary, spec.hdr, spec.persistent, spec.workspace_id, std::nullopt, nullptr, 1, *slot, {}};
    candidate.emplace(resource.id, resource);
    if (!impl_->capture_configuration()) {
      BOOST_LOG(error) << "Terra virtual display create: rollback capture failed";
      return {status_t::provider_error, std::nullopt};
    }
    if (!impl_->apply(candidate, &previous)) {
      BOOST_LOG(error) << "Terra virtual display create: topology apply failed";
      impl_->rollback_or_disable(previous);
      return {status_t::provider_error, std::nullopt};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->rollback_or_disable(previous);
      return {status_t::persistence_error, std::nullopt};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    impl_->notify(previous, impl_->resources);
    return {status_t::success, impl_->resources.at(resource.id)};
  }

  batch_result_t manager_t::create_batch(const std::string &owner, const std::vector<specification_t> &specifications) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
      return {status_t::unavailable, {}};
    }
    if (!valid_uuid(owner) || !valid_batch(specifications)) {
      return {status_t::invalid, {}};
    }
    std::vector<std::string> resource_ids;
    try {
      for (std::size_t index = 0; index < specifications.size(); ++index) {
        auto id = impl_->callbacks.uuid ? impl_->callbacks.uuid() : std::string {};
        std::ranges::transform(id, id.begin(), [](const unsigned char character) {
          return static_cast<char>(std::tolower(character));
        });
        if (!valid_uuid(id) || impl_->resources.contains(id) || std::ranges::contains(resource_ids, id)) {
          return {status_t::invalid, {}};
        }
        resource_ids.push_back(std::move(id));
      }
    } catch (...) {
      return {status_t::invalid, {}};
    }
    if (!impl_->provider_slots_match()) {
      return {status_t::provider_error, {}};
    }
    std::set<std::uint32_t> used_slots;
    for (const auto &[id, resource] : impl_->resources) {
      used_slots.emplace(resource.slot);
    }
    std::vector<std::uint32_t> slots;
    slots.reserve(specifications.size());
    for (std::size_t index = 0; index < specifications.size(); ++index) {
      std::optional<std::uint32_t> slot;
      for (std::uint32_t candidate = 0; candidate < impl_->callbacks.max_count; ++candidate) {
        if (!used_slots.contains(candidate)) {
          slot = candidate;
          break;
        }
      }
      if (!slot) {
        return {status_t::limit_reached, {}};
      }
      used_slots.emplace(*slot);
      slots.push_back(*slot);
    }
    const auto previous = impl_->resources;
    auto candidate = impl_->resources;
    std::vector<resource_t> resources;
    resources.reserve(specifications.size());
    for (std::size_t index = 0; index < specifications.size(); ++index) {
      const auto &spec = specifications[index];
      resource_t resource {resource_ids[index], spec.name, owner, spec.workspace_id ? state_t::attached : state_t::ready, spec.mode, {}, spec.position, spec.scale, spec.rotation, spec.primary, spec.hdr, spec.persistent, spec.workspace_id, std::nullopt, nullptr, 1, slots[index], {}};
      candidate.emplace(resource.id, resource);
      resources.push_back(std::move(resource));
    }
    if (!impl_->capture_configuration()) {
      return {status_t::provider_error, {}};
    }
    if (!impl_->apply(candidate, &previous)) {
      impl_->rollback_or_disable(previous);
      return {status_t::provider_error, {}};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->rollback_or_disable(previous);
      return {status_t::persistence_error, {}};
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    for (auto &resource : resources) {
      resource = impl_->resources.at(resource.id);
    }
    impl_->notify(previous, impl_->resources);
    return {status_t::success, std::move(resources)};
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
    if (!impl_->usable || !impl_->healthy()) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state == state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    if (!impl_->provider_slots_match()) {
      return {status_t::provider_error, std::nullopt};
    }
    const auto previous = impl_->resources;
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
    if (!impl_->capture_configuration()) {
      return {status_t::provider_error, std::nullopt};
    }
    if (!impl_->apply(candidate, &previous)) {
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
    impl_->notify(previous, impl_->resources);
    return {status_t::success, impl_->resources.at(id)};
  }

  result_t manager_t::remove(const std::string &id, const std::uint64_t expected_revision) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state == state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    if (!impl_->provider_slots_match()) {
      return {status_t::provider_error, std::nullopt};
    }
    const auto previous = impl_->resources;
    auto candidate = impl_->resources;
    candidate.erase(id);
    if (!impl_->capture_configuration()) {
      return {status_t::provider_error, std::nullopt};
    }
    if (!impl_->apply(candidate, &previous)) {
      impl_->rollback_or_disable(previous);
      return {status_t::provider_error, std::nullopt};
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      impl_->rollback_or_disable(previous);
      return {status_t::persistence_error, std::nullopt};
    }
    const auto deleted = found->second;
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    impl_->notify(previous, impl_->resources);
    return {status_t::success, deleted};
  }

  result_t manager_t::attach(const std::string &id, const std::uint64_t expected_revision, const attachment_t &attachment) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
      return {status_t::unavailable, std::nullopt};
    }
    if (attachment.session_id.has_value() == attachment.workspace_id.has_value() || (attachment.session_id && !valid_uuid(*attachment.session_id)) || (attachment.workspace_id && !valid_uuid(*attachment.workspace_id))) {
      return {status_t::invalid, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::ready) {
      return {status_t::conflict, std::nullopt};
    }
    const auto previous = impl_->resources;
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
    impl_->notify(previous, impl_->resources);
    return {status_t::success, impl_->resources.at(id)};
  }

  result_t manager_t::detach(const std::string &id, const std::uint64_t expected_revision) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
      return {status_t::unavailable, std::nullopt};
    }
    const auto found = impl_->resources.find(id);
    if (found == impl_->resources.end()) {
      return {status_t::not_found, std::nullopt};
    }
    if (found->second.revision != expected_revision || found->second.state != state_t::attached) {
      return {status_t::conflict, std::nullopt};
    }
    const auto previous = impl_->resources;
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
    impl_->notify(previous, impl_->resources);
    return {status_t::success, impl_->resources.at(id)};
  }

  result_t manager_t::adopt(const std::string &id, const std::uint64_t expected_revision, const std::string &owner) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
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
    const auto previous = impl_->resources;
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
    impl_->notify(previous, impl_->resources);
    return {status_t::success, impl_->resources.at(id)};
  }

  status_t manager_t::revoke_owner(const std::string &owner) {
    std::scoped_lock lock {impl_->mutex};
    if (!impl_->usable || !impl_->healthy()) {
      return status_t::unavailable;
    }
    if (!valid_uuid(owner)) {
      return status_t::invalid;
    }
    const auto previous = impl_->resources;
    auto candidate = impl_->resources;
    bool changed = false;
    bool removed_connector = false;
    for (auto &[id, resource] : candidate) {
      if (resource.owner_client_uuid && *resource.owner_client_uuid == owner) {
        changed = true;
        if (!resource.persistent) {
          removed_connector = true;
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
    std::erase_if(candidate, [&](const auto &entry) {
      const auto &resource = entry.second;
      return resource.owner_client_uuid && *resource.owner_client_uuid == owner && !resource.persistent;
    });
    if (removed_connector) {
      if (!impl_->provider_slots_match() || !impl_->capture_configuration()) {
        return status_t::provider_error;
      }
      if (!impl_->apply(candidate, &previous)) {
        impl_->rollback_or_disable(previous);
        return status_t::provider_error;
      }
    }
    const auto revision = impl_->collection_revision + 1;
    if (!impl_->save(candidate, revision)) {
      if (removed_connector) {
        impl_->rollback_or_disable(previous);
      }
      return status_t::persistence_error;
    }
    impl_->resources = std::move(candidate);
    impl_->collection_revision = revision;
    impl_->notify(previous, impl_->resources);
    return status_t::success;
  }

  nlohmann::json to_json(const resource_t &resource) {
    auto actual = mode_json(resource.actual_mode);
    actual["id"] = resource.actual_mode.id;
    return {{"id", resource.id}, {"name", resource.name}, {"ownerClientUuid", resource.owner_client_uuid ? nlohmann::json(*resource.owner_client_uuid) : nlohmann::json(nullptr)}, {"state", state_name(resource.state)}, {"requestedMode", mode_json(resource.requested_mode)}, {"actualMode", std::move(actual)}, {"position", {{"x", resource.position.x}, {"y", resource.position.y}}}, {"scale", resource.scale}, {"rotation", resource.rotation}, {"primary", resource.primary}, {"hdr", resource.hdr}, {"persistent", resource.persistent}, {"workspaceId", resource.workspace_id ? nlohmann::json(*resource.workspace_id) : nlohmann::json(nullptr)}, {"sessionId", resource.session_id ? nlohmann::json(*resource.session_id) : nlohmann::json(nullptr)}, {"error", resource.error}, {"revision", resource.revision}};
  }
}  // namespace terra_virtual_display
