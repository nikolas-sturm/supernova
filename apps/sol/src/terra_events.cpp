/**
 * @file src/terra_events.cpp
 * @brief Thread-safe Terra V1 event replay hub implementation.
 */

// standard includes
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// local includes
#include "terra_events.h"

namespace terra_events {
  namespace {
    constexpr std::uint64_t FIRST_EVENT_ID = 1;  ///< First cursor assigned to a visible client event.

    /**
     * @brief Parse a canonical positive Last-Event-ID value.
     *
     * @param value Header value.
     * @return Parsed cursor, or no value when malformed.
     */
    std::optional<std::uint64_t> parse_id(const std::string &value) {
      if (value.empty() || value.front() == '+' || value.front() == '-' || (value.size() > 1 && value.front() == '0')) {
        return std::nullopt;
      }
      std::uint64_t result = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
      if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size() || result == 0) {
        return std::nullopt;
      }
      return result;
    }

    /**
     * @brief Build a resynchronization producer event.
     *
     * @param collections Affected collection names.
     * @return Resynchronization event.
     */
    event_t resync_event(const std::vector<std::string> &collections) {
      return {"resync.required", std::nullopt, std::nullopt, {{"collections", collections}}};
    }
  }  // namespace

  struct hub_t::impl_t {
    /**
     * @brief Mutable state retained for one authenticated client.
     */
    struct client_t {
      std::uint64_t next_id = FIRST_EVENT_ID;  ///< Next visible event cursor.
      std::uint64_t generation = 0;  ///< Current stream generation.
      std::uint64_t cursor = FIRST_EVENT_ID;  ///< Next event awaited by current stream.
      bool connected = false;  ///< Whether current generation remains connected.
      std::vector<std::string> collections;  ///< Collections used by resynchronization events.
      std::deque<record_t> history;  ///< Bounded visible event history.
    };

    std::size_t capacity;  ///< Per-client history capacity.
    clock_t clock;  ///< Injectable timestamp source.
    std::mutex mutex;  ///< Protects all client state.
    std::condition_variable changed;  ///< Signals publication and generation changes.
    std::condition_variable waiters_done;  ///< Signals completion of active wait calls during destruction.
    std::unordered_map<std::string, client_t> clients;  ///< State keyed by authenticated identity.
    bool stopping = false;  ///< Whether destructor is disconnecting all streams.
    std::size_t active_waiters = 0;  ///< Wait calls currently using this implementation.

    /**
     * @brief Append one visible event while mutex is held.
     *
     * @param client Recipient state.
     * @param event Producer event.
     * @param timestamp Shared publication timestamp.
     * @return Assigned record.
     */
    record_t append(client_t &client, event_t event, const std::int64_t timestamp) {
      record_t record {client.next_id++, timestamp, std::move(event)};
      client.history.push_back(record);
      while (client.history.size() > capacity) {
        client.history.pop_front();
      }
      return record;
    }

    /**
     * @brief Create a control event without changing replay history.
     *
     * @param client Recipient state.
     * @param event Synthetic control event.
     * @return Assigned record.
     */
    record_t synthetic(client_t &client, event_t event) {
      return {client.next_id++, clock(), std::move(event)};
    }
  };

  bool valid_type(const std::string &type) {
    static const std::unordered_set<std::string> types {
      "host.changed",
      "capabilities.changed",
      "catalog.changed",
      "session.created",
      "session.updated",
      "session.removed",
      "displays.changed",
      "virtualDisplay.created",
      "virtualDisplay.updated",
      "virtualDisplay.removed",
      "telemetry.sample",
      "workspace.created",
      "workspace.updated",
      "workspace.removed",
      "profile.created",
      "profile.updated",
      "profile.removed",
      "peripheral.added",
      "peripheral.updated",
      "peripheral.removed",
      "sandbox.created",
      "sandbox.updated",
      "sandbox.removed",
      "operation.updated",
      "host.stopping",
      "resync.required",
    };
    return types.contains(type);
  }

  nlohmann::json to_json(const record_t &record) {
    nlohmann::json result {
      {"schemaVersion", 1},
      {"id", record.id},
      {"type", record.event.type},
      {"timestamp", record.timestamp},
      {"data", record.event.data},
    };
    if (record.event.resource_id) {
      result["resourceId"] = *record.event.resource_id;
    }
    if (record.event.revision) {
      result["revision"] = *record.event.revision;
    }
    return result;
  }

  std::string to_sse(const record_t &record) {
    return "id: " + std::to_string(record.id) + "\nevent: " + record.event.type + "\ndata: " + to_json(record).dump() + "\n\n";
  }

  hub_t::hub_t(const std::size_t replay_capacity, clock_t clock):
      impl_(std::make_unique<impl_t>()) {
    if (replay_capacity == 0 || !clock) {
      throw std::invalid_argument("Terra event replay capacity and clock must be valid");
    }
    impl_->capacity = replay_capacity;
    impl_->clock = std::move(clock);
  }

  hub_t::~hub_t() {
    std::unique_lock lock {impl_->mutex};
    impl_->stopping = true;
    for (auto &[client_id, client] : impl_->clients) {
      static_cast<void>(client_id);
      client.connected = false;
      ++client.generation;
    }
    impl_->changed.notify_all();
    impl_->waiters_done.wait(lock, [&]() {
      return impl_->active_waiters == 0;
    });
  }

  void hub_t::publish(event_t event, const std::vector<std::string> &recipients) {
    if (!valid_type(event.type)) {
      throw std::invalid_argument("Unsupported Terra event type");
    }
    std::unordered_set<std::string> unique;
    for (const auto &recipient : recipients) {
      if (!recipient.empty()) {
        unique.emplace(recipient);
      }
    }
    if (unique.empty()) {
      return;
    }
    std::scoped_lock lock {impl_->mutex};
    const auto timestamp = impl_->clock();
    for (const auto &recipient : unique) {
      auto &client = impl_->clients[recipient];
      impl_->append(client, event, timestamp);
    }
    impl_->changed.notify_all();
  }

  open_result_t hub_t::open(const std::string &client_id, const std::optional<std::string> &last_event_id, std::vector<std::string> collections) {
    if (client_id.empty()) {
      throw std::invalid_argument("Terra event client identity must not be empty");
    }
    std::scoped_lock lock {impl_->mutex};
    auto &client = impl_->clients[client_id];
    ++client.generation;
    client.connected = true;
    client.collections = std::move(collections);
    open_result_t result {{client_id, client.generation}, {}, false};

    if (!last_event_id) {
      client.cursor = client.next_id;
      impl_->changed.notify_all();
      return result;
    }

    const auto parsed = parse_id(*last_event_id);
    const auto newest = client.next_id - 1;
    const auto oldest = client.history.empty() ? client.next_id : client.history.front().id;
    if (!parsed || *parsed > newest || (*parsed < newest && (*parsed == std::numeric_limits<std::uint64_t>::max() || *parsed + 1 < oldest))) {
      result.resync_required = true;
      result.replay.push_back(impl_->synthetic(client, resync_event(client.collections)));
    } else {
      std::ranges::copy_if(client.history, std::back_inserter(result.replay), [&](const record_t &record) {
        return record.id > *parsed;
      });
    }
    client.cursor = client.next_id;
    impl_->changed.notify_all();
    return result;
  }

  wait_result_t hub_t::wait(const stream_t &stream, std::chrono::milliseconds idle_interval) {
    idle_interval = std::clamp(idle_interval, std::chrono::milliseconds::zero(), std::chrono::duration_cast<std::chrono::milliseconds>(MAX_IDLE_INTERVAL));
    std::unique_lock lock {impl_->mutex};
    ++impl_->active_waiters;

    struct waiter_guard_t {
      impl_t &impl;  ///< Implementation whose waiter count is protected by the held lock.

      /**
       * @brief Decrement active waiter count and notify a waiting destructor.
       */
      ~waiter_guard_t() {
        --impl.active_waiters;
        impl.waiters_done.notify_all();
      }
    } waiter_guard {*impl_};

    const auto ready = [&]() {
      const auto found = impl_->clients.find(stream.client_id);
      return impl_->stopping || found == impl_->clients.end() || found->second.generation != stream.generation || !found->second.connected || found->second.cursor < found->second.next_id;
    };
    if (!impl_->changed.wait_for(lock, idle_interval, ready)) {
      return {wait_status_t::idle, std::nullopt};
    }
    const auto found = impl_->clients.find(stream.client_id);
    if (impl_->stopping || found == impl_->clients.end() || found->second.generation != stream.generation || !found->second.connected) {
      return {wait_status_t::disconnected, std::nullopt};
    }
    auto &client = found->second;
    if (client.history.empty() || client.cursor < client.history.front().id) {
      const auto record = impl_->synthetic(client, resync_event(client.collections));
      client.cursor = client.next_id;
      client.connected = false;
      return {wait_status_t::event, record};
    }
    const auto offset = static_cast<std::size_t>(client.cursor - client.history.front().id);
    const auto record = client.history.at(offset);
    ++client.cursor;
    return {wait_status_t::event, record};
  }

  void hub_t::disconnect(const stream_t &stream) {
    {
      std::scoped_lock lock {impl_->mutex};
      const auto found = impl_->clients.find(stream.client_id);
      if (found == impl_->clients.end() || found->second.generation != stream.generation) {
        return;
      }
      found->second.connected = false;
      ++found->second.generation;
    }
    impl_->changed.notify_all();
  }

  void hub_t::disconnect_client(const std::string &client_id) {
    {
      std::scoped_lock lock {impl_->mutex};
      const auto found = impl_->clients.find(client_id);
      if (found == impl_->clients.end()) {
        return;
      }
      found->second.connected = false;
      ++found->second.generation;
    }
    impl_->changed.notify_all();
  }

  void hub_t::reset_client(const std::string &client_id) {
    {
      std::scoped_lock lock {impl_->mutex};
      impl_->clients.erase(client_id);
    }
    impl_->changed.notify_all();
  }

  void hub_t::disconnect_all() {
    {
      std::scoped_lock lock {impl_->mutex};
      for (auto &[client_id, client] : impl_->clients) {
        static_cast<void>(client_id);
        client.connected = false;
        ++client.generation;
      }
    }
    impl_->changed.notify_all();
  }
}  // namespace terra_events
