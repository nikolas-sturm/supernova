/**
 * @file tests/unit/test_eclipse_events.cpp
 * @brief Tests for Eclipse V1 event replay hub.
 */

// standard includes
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/eclipse_events.h>

namespace {
  using namespace std::chrono_literals;

  /**
   * @brief Build a hub using a mutable test clock.
   *
   * @param now Mutable Unix millisecond time.
   * @param capacity Replay capacity.
   * @return Event hub.
   */
  std::unique_ptr<eclipse_events::hub_t> hub(std::int64_t &now, const std::size_t capacity = 8) {
    return std::make_unique<eclipse_events::hub_t>(capacity, [&]() {
      return now;
    });
  }

  /**
   * @brief Publish a collection revision event.
   *
   * @param events Event hub.
   * @param revision Collection revision.
   * @param recipients Visible clients.
   */
  void publish(eclipse_events::hub_t &events, const std::uint64_t revision, const std::vector<std::string> &recipients = {"client"}) {
    events.publish({"catalog.changed", std::nullopt, revision, {{"revision", revision}}}, recipients);
  }
}  // namespace

TEST(EclipseEventsTest, AssignsOrderedPerClientIdsWithoutHiddenGaps) {
  std::int64_t now = 1000;
  auto events = hub(now);
  const auto first = events->open("allowed", std::nullopt, {"catalog"});
  const auto hidden = events->open("hidden", std::nullopt, {"catalog"});
  publish(*events, 1, {"allowed", "allowed"});
  publish(*events, 2, {"hidden"});
  publish(*events, 3, {"allowed"});

  EXPECT_EQ(events->wait(first.stream, 0ms).record->id, 1);
  EXPECT_EQ(events->wait(first.stream, 0ms).record->id, 2);
  EXPECT_EQ(events->wait(hidden.stream, 0ms).record->id, 1);
}

TEST(EclipseEventsTest, ReplaysEventsAfterLastEventIdInOrder) {
  std::int64_t now = 1000;
  auto events = hub(now);
  events->open("client", std::nullopt, {"catalog"});
  publish(*events, 1);
  publish(*events, 2);
  publish(*events, 3);

  const auto opened = events->open("client", "1", {"catalog"});
  ASSERT_FALSE(opened.resync_required);
  ASSERT_EQ(opened.replay.size(), 2);
  EXPECT_EQ(opened.replay[0].id, 2);
  EXPECT_EQ(opened.replay[1].id, 3);
}

TEST(EclipseEventsTest, InvalidStaleAndFutureCursorsRequireResync) {
  std::int64_t now = 1000;
  auto events = hub(now, 2);
  events->open("client", std::nullopt, {"catalog", "sessions"});
  publish(*events, 1);
  publish(*events, 2);
  publish(*events, 3);
  publish(*events, 4);

  for (const auto &cursor : {"invalid", "1", "99"}) {
    const auto opened = events->open("client", cursor, {"catalog", "sessions"});
    ASSERT_TRUE(opened.resync_required);
    ASSERT_EQ(opened.replay.size(), 1);
    EXPECT_EQ(opened.replay[0].event.type, "resync.required");
    EXPECT_EQ(opened.replay[0].event.data["collections"], nlohmann::json({"catalog", "sessions"}));
  }
}

TEST(EclipseEventsTest, SlowConsumerGetsResyncThenDisconnects) {
  std::int64_t now = 1000;
  auto events = hub(now, 2);
  const auto opened = events->open("client", std::nullopt, {"catalog"});
  publish(*events, 1);
  publish(*events, 2);
  publish(*events, 3);

  const auto overflow = events->wait(opened.stream, 0ms);
  ASSERT_EQ(overflow.status, eclipse_events::wait_status_t::event);
  EXPECT_EQ(overflow.record->event.type, "resync.required");
  EXPECT_EQ(events->wait(opened.stream, 0ms).status, eclipse_events::wait_status_t::disconnected);
}

TEST(EclipseEventsTest, PublishReadsClockOnceAndClockFailureIsAtomic) {
  std::int64_t now = 1000;
  std::size_t calls = 0;
  bool fails = true;
  eclipse_events::hub_t events {8, [&]() {
                                  ++calls;
                                  if (fails) {
                                    throw std::runtime_error("clock failed");
                                  }
                                  return now;
                                }};
  const auto first = events.open("first", std::nullopt, {}).stream;
  const auto second = events.open("second", std::nullopt, {}).stream;

  EXPECT_THROW(publish(events, 1, {"first", "second"}), std::runtime_error);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(events.wait(first, 0ms).status, eclipse_events::wait_status_t::idle);
  EXPECT_EQ(events.wait(second, 0ms).status, eclipse_events::wait_status_t::idle);

  fails = false;
  publish(events, 2, {"first", "second"});
  EXPECT_EQ(calls, 2);
  const auto first_record = events.wait(first, 0ms).record.value();
  const auto second_record = events.wait(second, 0ms).record.value();
  EXPECT_EQ(first_record.id, 1);
  EXPECT_EQ(second_record.id, 1);
  EXPECT_EQ(first_record.timestamp, second_record.timestamp);
}

TEST(EclipseEventsTest, SyntheticResyncDoesNotEvictReplayHistory) {
  std::int64_t now = 1000;
  auto events = hub(now, 2);
  const auto slow = events->open("client", std::nullopt, {"catalog"});
  publish(*events, 1);
  publish(*events, 2);
  publish(*events, 3);
  ASSERT_EQ(events->wait(slow.stream, 0ms).record->event.type, "resync.required");

  const auto replay = events->open("client", "2", {"catalog"});
  ASSERT_FALSE(replay.resync_required);
  ASSERT_EQ(replay.replay.size(), 1);
  EXPECT_EQ(replay.replay.front().id, 3);
  EXPECT_EQ(replay.replay.front().event.type, "catalog.changed");
}

TEST(EclipseEventsTest, WaitWakesForPublicationAndSignalsIdle) {
  std::int64_t now = 1000;
  auto events = hub(now);
  const auto opened = events->open("client", std::nullopt, {"catalog"});
  auto waiter = std::async(std::launch::async, [&]() {
    return events->wait(opened.stream, 5s);
  });
  std::this_thread::sleep_for(20ms);
  publish(*events, 1);
  ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(waiter.get().status, eclipse_events::wait_status_t::event);
  EXPECT_EQ(events->wait(opened.stream, 1ms).status, eclipse_events::wait_status_t::idle);
}

TEST(EclipseEventsTest, DisconnectsOneGenerationOrAllStreams) {
  std::int64_t now = 1000;
  auto events = hub(now);
  const auto old_stream = events->open("one", std::nullopt, {}).stream;
  const auto replacement = events->open("one", std::nullopt, {}).stream;
  const auto other = events->open("two", std::nullopt, {}).stream;
  EXPECT_EQ(events->wait(old_stream, 0ms).status, eclipse_events::wait_status_t::disconnected);
  events->disconnect(replacement);
  EXPECT_EQ(events->wait(replacement, 0ms).status, eclipse_events::wait_status_t::disconnected);
  events->disconnect_all();
  EXPECT_EQ(events->wait(other, 0ms).status, eclipse_events::wait_status_t::disconnected);
}

TEST(EclipseEventsTest, DisconnectsCurrentStreamByClientIdentity) {
  std::int64_t now = 1000;
  auto events = hub(now);
  const auto stream = events->open("client", std::nullopt, {}).stream;

  events->disconnect_client("missing");
  events->disconnect_client("client");

  EXPECT_EQ(events->wait(stream, 0ms).status, eclipse_events::wait_status_t::disconnected);
}

TEST(EclipseEventsTest, DestructorWakesAndJoinsActiveWaiters) {
  std::int64_t now = 1000;
  auto events = hub(now);
  const auto stream = events->open("client", std::nullopt, {}).stream;
  auto waiter = std::async(std::launch::async, [&]() {
    return events->wait(stream, 15s);
  });
  std::this_thread::sleep_for(20ms);

  events.reset();

  ASSERT_EQ(waiter.wait_for(1s), std::future_status::ready);
  EXPECT_EQ(waiter.get().status, eclipse_events::wait_status_t::disconnected);
}

TEST(EclipseEventsTest, SerializesCompleteJsonAndSseEnvelope) {
  const eclipse_events::record_t record {7, 1234, {"session.updated", "session-id", 9, {{"id", "session-id"}, {"revision", 9}}}};
  const auto json = eclipse_events::to_json(record);
  EXPECT_EQ(json["schemaVersion"], 1);
  EXPECT_EQ(json["id"], 7);
  EXPECT_EQ(json["timestamp"], 1234);
  EXPECT_EQ(json["type"], "session.updated");
  EXPECT_EQ(json["resourceId"], "session-id");
  EXPECT_EQ(json["revision"], 9);
  EXPECT_EQ(json["data"]["id"], "session-id");
  EXPECT_EQ(eclipse_events::to_sse(record), "id: 7\nevent: session.updated\ndata: " + json.dump() + "\n\n");
}

TEST(EclipseEventsTest, UsesInjectedClockAndOmitsInapplicableOptionalFields) {
  std::int64_t now = 4321;
  auto events = hub(now);
  const auto opened = events->open("client", std::nullopt, {});
  events->publish({"host.changed", std::nullopt, std::nullopt, {{"online", true}}}, {"client"});
  const auto record = events->wait(opened.stream, 0ms).record.value();
  const auto json = eclipse_events::to_json(record);
  EXPECT_EQ(record.timestamp, 4321);
  EXPECT_FALSE(json.contains("resourceId"));
  EXPECT_FALSE(json.contains("revision"));
}

TEST(EclipseEventsTest, ValidatesRequiredTypeVocabularyAndConstruction) {
  const std::vector<std::string> required_types {
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
  for (const auto &type : required_types) {
    EXPECT_TRUE(eclipse_events::valid_type(type)) << type;
  }
  EXPECT_FALSE(eclipse_events::valid_type("unknown.changed"));
  std::int64_t now = 1000;
  auto events = hub(now);
  EXPECT_THROW(events->publish({"unknown.changed", {}, {}, nullptr}, {"client"}), std::invalid_argument);
  EXPECT_THROW(eclipse_events::hub_t(0, [&]() {
                 return now;
               }),
               std::invalid_argument);
  EXPECT_THROW(eclipse_events::hub_t(1, {}), std::invalid_argument);
}
