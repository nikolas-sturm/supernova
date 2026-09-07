/**
 * @file src/terra_operations.cpp
 * @brief Terra V1 operation and idempotency storage implementation.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

// local includes
#include "crypto.h"
#include "terra_operations.h"
#include "uuid.h"

namespace terra_operations {
  namespace {
    constexpr int DOCUMENT_VERSION = 1;  ///< Persistent document schema version.

    /**
     * @brief Stored idempotency entry.
     */
    struct idempotency_t {
      std::string body_hash;  ///< SHA-256 of canonical request JSON.
      std::string operation_id;  ///< Associated operation UUID.
      nlohmann::json response;  ///< Exact original response.
    };

    /**
     * @brief Return stable textual state.
     *
     * @param state State value.
     * @return Terra state string.
     */
    std::string state_name(const state_t state) {
      switch (state) {
        case state_t::pending:
          return "pending";
        case state_t::running:
          return "running";
        case state_t::succeeded:
          return "succeeded";
        case state_t::failed:
          return "failed";
      }
      return {};
    }

    /**
     * @brief Parse textual state.
     *
     * @param value Candidate state string.
     * @return Parsed state, or no value.
     */
    std::optional<state_t> parse_state(const std::string &value) {
      if (value == "pending") {
        return state_t::pending;
      }
      if (value == "running") {
        return state_t::running;
      }
      if (value == "succeeded") {
        return state_t::succeeded;
      }
      if (value == "failed") {
        return state_t::failed;
      }
      return std::nullopt;
    }

    /**
     * @brief Test whether state is terminal.
     *
     * @param state State value.
     * @return True for succeeded or failed.
     */
    bool terminal(const state_t state) {
      return state == state_t::succeeded || state == state_t::failed;
    }

    /**
     * @brief Produce lowercase hexadecimal SHA-256.
     *
     * @param body Parsed request body.
     * @return Canonical JSON digest.
     */
    std::string body_hash(const nlohmann::json &body) {
      const auto digest = crypto::hash(body.dump());
      std::ostringstream stream;
      stream << std::hex << std::setfill('0');
      for (const auto byte : digest) {
        stream << std::setw(2) << static_cast<unsigned int>(byte);
      }
      return stream.str();
    }

    /**
     * @brief Build unambiguous idempotency scope key.
     *
     * @param client_uuid Client UUID.
     * @param action Action name.
     * @param key Idempotency key.
     * @return Internal composite key.
     */
    std::string scope_key(const std::string &client_uuid, const std::string &action, const std::string &key) {
      return client_uuid + '\0' + action + '\0' + key;
    }
  }  // namespace

  struct store_t::impl_t {
    callbacks_t callbacks;  ///< External services.
    std::int64_t retention;  ///< Terminal retention in milliseconds.
    mutable std::mutex mutex;  ///< Protects all maps and persistence.
    std::map<std::string, operation_t, std::less<>> operations;  ///< Operations indexed by UUID.
    std::map<std::string, idempotency_t, std::less<>> idempotency;  ///< Entries indexed by scope.

    /**
     * @brief Serialize complete persistent state.
     *
     * @param source_operations Operations to serialize.
     * @param source_idempotency Idempotency entries to serialize.
     * @return Versioned JSON document.
     */
    static nlohmann::json document(const decltype(operations) &source_operations, const decltype(idempotency) &source_idempotency) {
      nlohmann::json result {{"version", DOCUMENT_VERSION}, {"operations", nlohmann::json::array()}, {"idempotency", nlohmann::json::array()}};
      for (const auto &[id, operation] : source_operations) {
        auto value = to_json(operation);
        value["clientUuid"] = operation.client_uuid;
        value["action"] = operation.action;
        result["operations"].push_back(std::move(value));
      }
      for (const auto &[scope, entry] : source_idempotency) {
        result["idempotency"].push_back({{"scope", scope}, {"bodyHash", entry.body_hash}, {"operationId", entry.operation_id}, {"response", entry.response}});
      }
      return result;
    }

    /**
     * @brief Persist candidate state.
     *
     * @param candidate_operations Candidate operations.
     * @param candidate_idempotency Candidate idempotency entries.
     * @return True when callback accepted document.
     */
    bool save(const decltype(operations) &candidate_operations, const decltype(idempotency) &candidate_idempotency) const {
      try {
        return callbacks.save(document(candidate_operations, candidate_idempotency).dump());
      } catch (...) {
        return false;
      }
    }
  };

  store_t::store_t(callbacks_t callbacks, const std::int64_t terminal_retention_ms):
      impl_(std::make_unique<impl_t>()) {
    impl_->callbacks = std::move(callbacks);
    impl_->retention = std::max(terminal_retention_ms, MIN_TERMINAL_RETENTION_MS);
    if (!impl_->callbacks.now) {
      impl_->callbacks.now = []() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
      };
    }
    if (!impl_->callbacks.uuid) {
      impl_->callbacks.uuid = []() {
        return uuid_util::uuid_t::generate().string();
      };
    }
    if (!impl_->callbacks.save) {
      impl_->callbacks.save = [](const std::string &) {
        return false;
      };
    }
    if (!impl_->callbacks.load) {
      return;
    }

    try {
      const auto text = impl_->callbacks.load();
      if (!text) {
        return;
      }
      const auto document = nlohmann::json::parse(*text);
      if (!document.is_object() || document.value("version", 0) != DOCUMENT_VERSION) {
        return;
      }
      for (const auto &value : document.value("operations", nlohmann::json::array())) {
        try {
          const auto state = parse_state(value.at("state").get<std::string>());
          if (!state) {
            continue;
          }
          operation_t operation {
            value.at("id").get<std::string>(),
            value.at("clientUuid").get<std::string>(),
            value.at("action").get<std::string>(),
            *state,
            std::nullopt,
            value.at("createdAt").get<std::int64_t>(),
            value.at("updatedAt").get<std::int64_t>(),
            value.at("revision").get<std::uint64_t>(),
            value.at("result"),
            value.at("error")
          };
          if (!uuid_util::is_valid(operation.id) || !uuid_util::is_valid(operation.client_uuid) || operation.action.empty() || operation.created_at < 0 || operation.updated_at < operation.created_at || operation.revision == 0 || (!value.at("resourceId").is_null() && !value.at("resourceId").is_string())) {
            continue;
          }
          if (value.at("resourceId").is_string()) {
            operation.resource_id = value.at("resourceId").get<std::string>();
          }
          if ((!terminal(operation.state) && (!operation.result.is_null() || !operation.error.is_null())) || (operation.state == state_t::succeeded && (operation.result.is_null() || !operation.error.is_null())) || (operation.state == state_t::failed && (!operation.result.is_null() || operation.error.is_null()))) {
            continue;
          }
          impl_->operations.emplace(operation.id, std::move(operation));
        } catch (...) {}
      }
      for (const auto &value : document.value("idempotency", nlohmann::json::array())) {
        try {
          idempotency_t entry {value.at("bodyHash").get<std::string>(), value.at("operationId").get<std::string>(), value.at("response")};
          const auto scope = value.at("scope").get<std::string>();
          const auto operation = impl_->operations.find(entry.operation_id);
          const auto expected_scope_prefix = operation == impl_->operations.end() ? std::string {} : scope_key(operation->second.client_uuid, operation->second.action, {});
          if (entry.body_hash.size() != 64 || operation == impl_->operations.end() || !scope.starts_with(expected_scope_prefix) || scope.size() == expected_scope_prefix.size()) {
            continue;
          }
          impl_->idempotency.emplace(scope, std::move(entry));
        } catch (...) {}
      }

      auto recovered_operations = impl_->operations;
      bool recovered = false;
      const auto now = impl_->callbacks.now();
      for (auto &[id, operation] : recovered_operations) {
        if (terminal(operation.state)) {
          continue;
        }
        operation.state = state_t::failed;
        operation.resource_id.reset();
        operation.updated_at = std::max(now, operation.updated_at);
        ++operation.revision;
        operation.result = nullptr;
        operation.error = {
          {"code", "host_restarted"},
          {"message", "Host restarted before operation completed"},
        };
        recovered = true;
      }
      if (recovered) {
        static_cast<void>(impl_->save(recovered_operations, impl_->idempotency));
        impl_->operations = std::move(recovered_operations);
      }
    } catch (...) {}
    prune();
  }

  store_t::~store_t() = default;

  submission_t store_t::submit(const std::string &client_uuid, const std::string &action, const std::string &idempotency_key, const nlohmann::json &body, const std::function<nlohmann::json(const operation_t &)> &response_builder) {
    std::scoped_lock lock {impl_->mutex};
    if (!uuid_util::is_valid(client_uuid) || action.empty() || action.size() > 64 || action.contains('\0') || idempotency_key.empty() || idempotency_key.size() > 128 || idempotency_key.contains('\0')) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    const auto scope = scope_key(client_uuid, action, idempotency_key);
    const auto hash = body_hash(body);
    if (const auto found = impl_->idempotency.find(scope); found != impl_->idempotency.end()) {
      if (found->second.body_hash != hash) {
        return {submission_status_t::conflict, std::nullopt, nullptr};
      }
      const auto operation = impl_->operations.find(found->second.operation_id);
      if (operation == impl_->operations.end()) {
        return {submission_status_t::conflict, std::nullopt, nullptr};
      }
      return {submission_status_t::replayed, operation->second, found->second.response};
    }
    std::int64_t now;
    std::string operation_id;
    try {
      now = impl_->callbacks.now();
      operation_id = impl_->callbacks.uuid();
      std::ranges::transform(operation_id, operation_id.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
      });
    } catch (...) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    operation_t operation {std::move(operation_id), client_uuid, action, state_t::pending, std::nullopt, now, now, 1, nullptr, nullptr};
    if (!uuid_util::is_valid(operation.id) || now < 0) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    nlohmann::json response;
    try {
      response = response_builder(operation);
    } catch (...) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    auto operations = impl_->operations;
    auto idempotency = impl_->idempotency;
    if (!operations.emplace(operation.id, operation).second) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    idempotency.emplace(scope, idempotency_t {hash, operation.id, response});
    if (!impl_->save(operations, idempotency)) {
      return {submission_status_t::persistence_failed, std::nullopt, nullptr};
    }
    impl_->operations = std::move(operations);
    impl_->idempotency = std::move(idempotency);
    return {submission_status_t::created, operation, response};
  }

  std::optional<operation_t> store_t::transition(const std::string &operation_id, const state_t state, std::optional<std::string> resource_id, nlohmann::json result, nlohmann::json error) {
    std::optional<operation_t> updated;
    {
      std::scoped_lock lock {impl_->mutex};
      const auto found = impl_->operations.find(operation_id);
      if (found == impl_->operations.end() || terminal(found->second.state)) {
        return std::nullopt;
      }
      if (state == state_t::pending || (found->second.state == state_t::running && !terminal(state))) {
        return std::nullopt;
      }
      if ((state == state_t::succeeded && result.is_null()) || (state == state_t::failed && error.is_null())) {
        return std::nullopt;
      }
      auto operations = impl_->operations;
      auto &operation = operations.at(operation_id);
      operation.state = state;
      operation.resource_id = std::move(resource_id);
      operation.result = state == state_t::succeeded ? std::move(result) : nlohmann::json(nullptr);
      operation.error = state == state_t::failed ? std::move(error) : nlohmann::json(nullptr);
      operation.updated_at = std::max(impl_->callbacks.now(), operation.updated_at);
      ++operation.revision;
      if (!impl_->save(operations, impl_->idempotency)) {
        return std::nullopt;
      }
      impl_->operations = std::move(operations);
      updated = impl_->operations.at(operation_id);
    }
    if (impl_->callbacks.updated) {
      try {
        impl_->callbacks.updated(*updated);
      } catch (...) {}
    }
    return updated;
  }

  std::optional<operation_t> store_t::get(const std::string &operation_id) const {
    std::scoped_lock lock {impl_->mutex};
    const auto found = impl_->operations.find(operation_id);
    return found == impl_->operations.end() ? std::nullopt : std::optional<operation_t> {found->second};
  }

  std::size_t store_t::prune() {
    std::scoped_lock lock {impl_->mutex};
    auto operations = impl_->operations;
    auto idempotency = impl_->idempotency;
    const auto cutoff = impl_->callbacks.now() - impl_->retention;
    std::size_t removed = 0;
    for (auto iterator = operations.begin(); iterator != operations.end();) {
      if (terminal(iterator->second.state) && iterator->second.updated_at <= cutoff) {
        const auto id = iterator->first;
        iterator = operations.erase(iterator);
        std::erase_if(idempotency, [&](const auto &item) {
          return item.second.operation_id == id;
        });
        ++removed;
      } else {
        ++iterator;
      }
    }
    if (removed == 0) {
      return 0;
    }
    if (!impl_->save(operations, idempotency)) {
      return 0;
    }
    impl_->operations = std::move(operations);
    impl_->idempotency = std::move(idempotency);
    return removed;
  }

  nlohmann::json to_json(const operation_t &operation) {
    return {
      {"id", operation.id},
      {"state", state_name(operation.state)},
      {"resourceId", operation.resource_id ? nlohmann::json(*operation.resource_id) : nlohmann::json(nullptr)},
      {"createdAt", operation.created_at},
      {"updatedAt", operation.updated_at},
      {"revision", operation.revision},
      {"result", operation.result},
      {"error", operation.error},
    };
  }
}  // namespace terra_operations
