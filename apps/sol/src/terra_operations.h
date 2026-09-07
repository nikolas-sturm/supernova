/**
 * @file src/terra_operations.h
 * @brief Thread-safe Terra V1 operations and idempotency storage.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// lib includes
#include <nlohmann/json.hpp>

/**
 * @brief Terra V1 asynchronous operation storage.
 */
namespace terra_operations {
  constexpr std::int64_t MIN_TERMINAL_RETENTION_MS = 10 * 60 * 1000;  ///< Minimum terminal record retention.

  /**
   * @brief Lifecycle state of an asynchronous operation.
   */
  enum class state_t {
    pending,  ///< Operation accepted but not started.
    running,  ///< Operation currently executing.
    succeeded,  ///< Operation completed successfully.
    failed,  ///< Operation completed with an error.
  };

  /**
   * @brief Stored Terra operation.
   */
  struct operation_t {
    std::string id;  ///< Canonical operation UUID.
    std::string client_uuid;  ///< Canonical UUID of client that submitted operation.
    std::string action;  ///< Stable mutation action name.
    state_t state;  ///< Current lifecycle state.
    std::optional<std::string> resource_id;  ///< Created or affected resource identifier.
    std::int64_t created_at;  ///< Creation time in Unix milliseconds.
    std::int64_t updated_at;  ///< Last update time in Unix milliseconds.
    std::uint64_t revision;  ///< Monotonically increasing operation revision.
    nlohmann::json result;  ///< Success result, or JSON null.
    nlohmann::json error;  ///< Failure details, or JSON null.
  };

  /**
   * @brief Result category from an idempotent submission.
   */
  enum class submission_status_t {
    created,  ///< New operation and idempotency entry created.
    replayed,  ///< Equivalent request replayed stored response.
    conflict,  ///< Key was previously used with a different request body.
    persistence_failed,  ///< Persistence callback rejected new state.
  };

  /**
   * @brief Result of submitting an idempotent operation.
   */
  struct submission_t {
    submission_status_t status;  ///< Submission outcome.
    std::optional<operation_t> operation;  ///< Created or replayed operation.
    nlohmann::json response;  ///< Original response for created or replayed submissions, otherwise JSON null.
  };

  /**
   * @brief Caller-provided storage, time, and identity services.
   */
  struct callbacks_t {
    std::function<std::optional<std::string>()> load;  ///< Load persisted JSON document, or no document.
    std::function<bool(const std::string &)> save;  ///< Atomically persist complete JSON document.
    std::function<std::int64_t()> now;  ///< Return current Unix time in milliseconds.
    std::function<std::string()> uuid;  ///< Return a new canonical UUID.
    std::function<void(const operation_t &)> updated;  ///< Publish a successfully persisted operation transition.
  };

  /**
   * @brief Thread-safe operation and idempotency store.
   */
  class store_t {
  public:
    /**
     * @brief Construct and reload a store.
     *
     * @param callbacks Caller-provided persistence and platform services.
     * @param terminal_retention_ms Terminal and idempotency retention duration.
     */
    explicit store_t(callbacks_t callbacks, std::int64_t terminal_retention_ms = MIN_TERMINAL_RETENTION_MS);

    /**
     * @brief Destroy store implementation.
     */
    ~store_t();

    store_t(const store_t &) = delete;  ///< Copying a synchronized store is unsupported.
    store_t &operator=(const store_t &) = delete;  ///< Copy assignment is unsupported.
    store_t(store_t &&) = delete;  ///< Moving a synchronized store is unsupported.
    store_t &operator=(store_t &&) = delete;  ///< Move assignment is unsupported.

    /**
     * @brief Submit an operation under an idempotency scope.
     *
     * @param client_uuid Canonical client UUID.
     * @param action Stable action name.
     * @param idempotency_key Client-provided idempotency key.
     * @param body Parsed request body whose canonical JSON form determines equivalence.
     * @param response_builder Builds response stored for exact replay.
     * @return Submission outcome and stored values.
     */
    submission_t submit(const std::string &client_uuid, const std::string &action, const std::string &idempotency_key, const nlohmann::json &body, const std::function<nlohmann::json(const operation_t &)> &response_builder);

    /**
     * @brief Transition an operation to a valid next state.
     *
     * @param operation_id Operation UUID.
     * @param state Requested next state.
     * @param resource_id Optional affected resource identifier.
     * @param result Success result, otherwise JSON null.
     * @param error Failure details, otherwise JSON null.
     * @return Updated operation, or no value for missing, invalid, or unpersisted transitions.
     */
    std::optional<operation_t> transition(const std::string &operation_id, state_t state, std::optional<std::string> resource_id = std::nullopt, nlohmann::json result = nullptr, nlohmann::json error = nullptr);

    /**
     * @brief Find an operation by UUID.
     *
     * @param operation_id Operation UUID.
     * @return Snapshot of operation, or no value when absent.
     */
    std::optional<operation_t> get(const std::string &operation_id) const;

    /**
     * @brief Remove expired terminal operations and associated idempotency entries.
     *
     * @return Number of operations removed, or zero if persistence fails.
     */
    std::size_t prune();

  private:
    /**
     * @brief Hidden implementation.
     */
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Owned implementation.
  };

  /**
   * @brief Serialize an operation for Terra API responses.
   *
   * @param operation Operation to serialize.
   * @return JSON operation object.
   */
  nlohmann::json to_json(const operation_t &operation);
}  // namespace terra_operations
