/**
 * @file src/terra_events.h
 * @brief Thread-safe Terra V1 event replay hub.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

/**
 * @brief Terra V1 event publication and replay support.
 */
namespace terra_events {
  constexpr auto MAX_IDLE_INTERVAL = std::chrono::seconds(15);  ///< Longest wait before an idle SSE comment is required.

  /**
   * @brief Event fields supplied by a producer.
   */
  struct event_t {
    std::string type;  ///< Required Terra event type.
    std::optional<std::string> resource_id;  ///< Associated resource identifier, when applicable.
    std::optional<std::uint64_t> revision;  ///< Associated resource or collection revision, when applicable.
    nlohmann::json data;  ///< Type-specific event payload.
  };

  /**
   * @brief Event assigned to one client's visible stream.
   */
  struct record_t {
    std::uint64_t id;  ///< Per-client ordered event cursor.
    std::int64_t timestamp;  ///< UTC Unix timestamp in milliseconds.
    event_t event;  ///< Published event fields.
  };

  /**
   * @brief Opaque identity for one opened client stream generation.
   */
  struct stream_t {
    std::string client_id;  ///< Authenticated client identity.
    std::uint64_t generation;  ///< Generation invalidating superseded or disconnected streams.
  };

  /**
   * @brief Result of opening a stream and applying Last-Event-ID.
   */
  struct open_result_t {
    stream_t stream;  ///< Handle used for waiting and disconnection.
    std::vector<record_t> replay;  ///< Ordered replay or one resynchronization event.
    bool resync_required;  ///< Whether replay was unavailable or Last-Event-ID was invalid.
  };

  /**
   * @brief Outcome from waiting for one stream item.
   */
  enum class wait_status_t {
    event,  ///< A visible event is available.
    idle,  ///< Idle interval elapsed and caller should send an SSE comment.
    disconnected,  ///< Stream generation was disconnected or superseded.
  };

  /**
   * @brief Wait result containing an event when status is event.
   */
  struct wait_result_t {
    wait_status_t status;  ///< Wait outcome.
    std::optional<record_t> record;  ///< Available event, otherwise no value.
  };

  /**
   * @brief Determine whether a type belongs to the required Terra event vocabulary.
   *
   * @param type Event type to inspect.
   * @return True when type is supported.
   */
  bool valid_type(const std::string &type);

  /**
   * @brief Serialize a complete event JSON value.
   *
   * @param record Client-visible event record.
   * @return JSON object containing all required envelope fields.
   */
  nlohmann::json to_json(const record_t &record);

  /**
   * @brief Serialize one record as a complete SSE message.
   *
   * @param record Client-visible event record.
   * @return UTF-8 SSE message containing id, event, and one JSON data line.
   */
  std::string to_sse(const record_t &record);

  /**
   * @brief Frame one payload for HTTP chunked transfer encoding.
   *
   * An empty payload produces the terminal chunk.
   *
   * @param payload Payload bytes.
   * @return Complete HTTP chunk including delimiters.
   */
  std::string to_http_chunk(std::string_view payload);

  /**
   * @brief Thread-safe bounded per-client event replay hub.
   */
  class hub_t {
  public:
    /**
     * @brief Function returning current UTC Unix time in milliseconds.
     */
    using clock_t = std::function<std::int64_t()>;

    /**
     * @brief Construct an event hub.
     *
     * @param replay_capacity Maximum retained visible events per client.
     * @param clock Injectable timestamp source.
     */
    explicit hub_t(std::size_t replay_capacity, clock_t clock);

    /**
     * @brief Destroy hub implementation and wake waiting streams.
     */
    ~hub_t();

    hub_t(const hub_t &) = delete;  ///< Copying a synchronized hub is unsupported.
    hub_t &operator=(const hub_t &) = delete;  ///< Copy assignment is unsupported.
    hub_t(hub_t &&) = delete;  ///< Moving a synchronized hub is unsupported.
    hub_t &operator=(hub_t &&) = delete;  ///< Move assignment is unsupported.

    /**
     * @brief Publish an event only to explicitly visible recipients.
     *
     * Per-client IDs are assigned only for listed recipients. Duplicate recipient
     * identities are ignored. One clock value timestamps every recipient copy.
     * Clock failure leaves all recipient streams unchanged. Invalid event types
     * throw std::invalid_argument.
     *
     * @param event Event fields to publish.
     * @param recipients Authenticated clients allowed to observe this event.
     */
    void publish(event_t event, const std::vector<std::string> &recipients);

    /**
     * @brief Open or replace a client stream and prepare optional replay.
     *
     * @param client_id Authenticated client identity.
     * @param last_event_id Standard Last-Event-ID value, or no value for a fresh stream.
     * @param collections Collection names included in any resynchronization payload.
     * @return Stream handle and immediately available replay records.
     */
    open_result_t open(const std::string &client_id, const std::optional<std::string> &last_event_id, std::vector<std::string> collections);

    /**
     * @brief Wait for an event, disconnection, or required idle signal.
     *
     * @param stream Open stream handle.
     * @param idle_interval Requested idle interval, clamped to 15 seconds.
     * @return Wait outcome and optional event.
     */
    wait_result_t wait(const stream_t &stream, std::chrono::milliseconds idle_interval = MAX_IDLE_INTERVAL);

    /**
     * @brief Disconnect one stream generation and wake its waiter.
     *
     * @param stream Stream generation to disconnect.
     */
    void disconnect(const stream_t &stream);

    /**
     * @brief Disconnect current stream for one authenticated client.
     *
     * @param client_id Authenticated client identity.
     */
    void disconnect_client(const std::string &client_id);

    /**
     * @brief Disconnect one client and discard replay history after authorization changes.
     *
     * @param client_id Authenticated client identity.
     */
    void reset_client(const std::string &client_id);

    /**
     * @brief Disconnect all current streams and wake all waiters.
     */
    void disconnect_all();

  private:
    /**
     * @brief Hidden implementation.
     */
    struct impl_t;
    std::unique_ptr<impl_t> impl_;  ///< Owned implementation.
  };
}  // namespace terra_events
