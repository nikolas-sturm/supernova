/**
 * @file tests/unit/test_stream.cpp
 * @brief Test src/stream.*
 */

// test includes
#include "../tests_common.h"

// standard includes
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// local includes
#include <src/network.h>
#include <src/thread_safe.h>

namespace stream {
  std::vector<uint8_t> concat_and_insert(uint64_t insert_size, uint64_t slice_size, const std::string_view &data1, const std::string_view &data2);
  std::optional<std::pair<std::uint16_t, std::string_view>> parse_control_packet(const ENetPacket &packet);
  int wait_for_ping(safe::queue_t<std::pair<boost::asio::ip::udp::endpoint, std::string>> &messages, safe::event_t<bool> &shutdown_event, bool session_id_supported, std::string_view expected_payload, boost::asio::ip::udp::endpoint &peer, std::chrono::milliseconds timeout);
}  // namespace stream

/** @brief Socket-free startup ping tests using the production queue and shutdown event. */
class StartupPingTests: public testing::Test {
protected:
  safe::queue_t<std::pair<boost::asio::ip::udp::endpoint, std::string>> messages;  ///< Received packets.
  safe::event_t<bool> shutdown;  ///< Shared session shutdown signal.
  boost::asio::ip::udp::endpoint peer {boost::asio::ip::make_address("127.0.0.1"), 0};  ///< Unconfirmed endpoint.
  const boost::asio::ip::udp::endpoint sender {boost::asio::ip::make_address("127.0.0.1"), 12345};  ///< Ping source.
};

TEST_F(StartupPingTests, StopAlreadyRaisedPreservesSignalAndPeer) {
  using namespace std::chrono_literals;
  shutdown.raise(true);
  messages.raise(sender, "session-payload");
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 5s), -1);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(peer.port(), 0);
  EXPECT_TRUE(shutdown.view(0ms));
  EXPECT_EQ(messages.size(), 1);
}

TEST_F(StartupPingTests, StopDuringWaitWithoutUdpPing) {
  using namespace std::chrono_literals;
  std::jthread stopper {[&]() {
    std::this_thread::sleep_for(200ms);
    shutdown.raise(true);
  }};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 5s), -1);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_EQ(peer.port(), 0);
  EXPECT_TRUE(shutdown.view(0ms));
}

TEST_F(StartupPingTests, MatchesPayloadAfterDiscardingOtherPackets) {
  using namespace std::chrono_literals;
  messages.raise(sender, "unrelated");
  messages.raise(sender, "PING");
  messages.raise(sender, "prefix-session-payload-suffix");
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 1s), 0);
  EXPECT_EQ(peer, sender);
  EXPECT_EQ(messages.size(), 0);
  EXPECT_FALSE(shutdown.view(0ms));
}

TEST_F(StartupPingTests, SharedShutdownCancelsBothStartupWorkers) {
  using namespace std::chrono_literals;
  decltype(messages) audio_messages;
  auto audio_peer = peer;
  int video_result = 0;
  int audio_result = 0;
  const auto start = std::chrono::steady_clock::now();
  std::jthread video_worker {[&]() {
    video_result = stream::wait_for_ping(messages, shutdown, true, "video-payload", peer, 5s);
  }};
  std::jthread audio_worker {[&]() {
    audio_result = stream::wait_for_ping(audio_messages, shutdown, true, "audio-payload", audio_peer, 5s);
  }};
  std::this_thread::sleep_for(200ms);
  shutdown.raise(true);
  video_worker.join();
  audio_worker.join();
  EXPECT_EQ(video_result, -1);
  EXPECT_EQ(audio_result, -1);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2s);
  EXPECT_TRUE(shutdown.view(0ms));
}

TEST_F(StartupPingTests, AcceptsLegacyPing) {
  using namespace std::chrono_literals;
  messages.raise(sender, "PING");
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, false, "session-payload", peer, 1s), 0);
  EXPECT_EQ(peer, sender);
}

TEST_F(StartupPingTests, AcceptsPingAfterMultipleWaitSlices) {
  using namespace std::chrono_literals;
  std::jthread producer {[&]() {
    std::this_thread::sleep_for(250ms);
    messages.raise(sender, "session-payload");
  }};
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 2s), 0);
  EXPECT_EQ(peer, sender);
}

TEST_F(StartupPingTests, EmptyQueueWaitsUntilOriginalDeadline) {
  using namespace std::chrono_literals;
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 250ms), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 250ms);
  EXPECT_LT(elapsed, 2s);
  EXPECT_EQ(peer.port(), 0);
}

TEST_F(StartupPingTests, NonMatchingPacketsDoNotExtendDeadline) {
  using namespace std::chrono_literals;
  std::jthread producer {[&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      messages.raise(sender, "PING");
      std::this_thread::sleep_for(20ms);
    }
  }};
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 250ms), -1);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_GE(elapsed, 250ms);
  EXPECT_LT(elapsed, 2s);
  EXPECT_EQ(peer.port(), 0);
}

TEST_F(StartupPingTests, ExpiredDeadlineDoesNotAcceptQueuedPing) {
  using namespace std::chrono_literals;
  messages.raise(sender, "session-payload");
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 0ms), -1);
  EXPECT_EQ(peer.port(), 0);
  EXPECT_EQ(messages.size(), 1);
}

TEST_F(StartupPingTests, ClosedQueueReturnsWithoutSpinning) {
  using namespace std::chrono_literals;
  messages.stop();
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(stream::wait_for_ping(messages, shutdown, true, "session-payload", peer, 5s), -1);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(peer.port(), 0);
}

TEST(ConcatAndInsertTests, ConcatNoInsertionTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(0, 2, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatLargeStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, sizeof(b1) + sizeof(b2) + 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 'b', 'c', 'd', 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ConcatAndInsertTests, ConcatSmallStrideTest) {
  char b1[] = {'a', 'b'};
  char b2[] = {'c', 'd', 'e'};
  auto res = stream::concat_and_insert(1, 1, std::string_view {b1, sizeof(b1)}, std::string_view {b2, sizeof(b2)});
  auto expected = std::vector<uint8_t> {0, 'a', 0, 'b', 0, 'c', 0, 'd', 0, 'e'};
  ASSERT_EQ(res, expected);
}

TEST(ControlPacketTests, RejectsZeroLengthPacket) {
  net::packet_t packet {enet_packet_create(nullptr, 0, 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(packet->data, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, RejectsOneBytePacket) {
  const std::uint8_t data {0x06};
  net::packet_t packet {enet_packet_create(&data, sizeof(data), 0)};

  ASSERT_NE(packet, nullptr);
  EXPECT_EQ(stream::parse_control_packet(*packet), std::nullopt);
}

TEST(ControlPacketTests, AcceptsTypeWithoutPayload) {
  const std::array<std::uint8_t, 2> data {0x06, 0x02};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_TRUE(message->second.empty());
}

TEST(ControlPacketTests, AcceptsTypeAndPayload) {
  const std::array<std::uint8_t, 5> data {0x06, 0x02, 'a', 'b', 'c'};
  net::packet_t packet {enet_packet_create(data.data(), data.size(), 0)};

  ASSERT_NE(packet, nullptr);
  auto message = stream::parse_control_packet(*packet);
  ASSERT_TRUE(message);
  EXPECT_EQ(message->first, 0x0206);
  EXPECT_EQ(message->second, "abc");
}
