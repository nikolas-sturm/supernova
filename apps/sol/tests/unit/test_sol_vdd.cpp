/**
 * @file tests/unit/test_sol_vdd.cpp
 * @brief Test SolVDD topology protocol helpers.
 */

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// standard includes
#include <cstring>

// local includes
#include <src/terra_virtual_display.h>
#include <src/platform/windows/terra_virtual_display_provider.h>
#include <src/platform/windows/sol_vdd.h>

#ifdef _WIN32
namespace {
  /**
   * @brief Build a protocol response message for decode tests.
   *
   * @param request_id Request identifier to echo.
   * @param status Response status.
   * @param connectors Committed connector records.
   * @return Serialized response bytes.
   */
  std::vector<std::byte> make_response(const std::uint32_t request_id, const sol_vdd::status_t status, const std::vector<sol_vdd::connector_state_t> &connectors) {
    const sol_vdd::response_header_t header {
      sol_vdd::PROTOCOL_MAGIC,
      sol_vdd::PROTOCOL_VERSION,
      sizeof(sol_vdd::response_header_t),
      request_id,
      static_cast<std::uint32_t>(status),
      static_cast<std::uint32_t>(connectors.size()),
      0,
    };
    std::vector<std::byte> message(sizeof(header) + connectors.size() * sizeof(sol_vdd::connector_state_t));
    std::memcpy(message.data(), &header, sizeof(header));
    if (!connectors.empty()) {
      std::memcpy(message.data() + sizeof(header), connectors.data(), connectors.size() * sizeof(sol_vdd::connector_state_t));
    }
    return message;
  }

  const sol_vdd::connector_state_t CONNECTOR_A {0, 1920, 1080, 60, 1, 10, 1, 0};  ///< First test connector.
  const sol_vdd::connector_state_t CONNECTOR_B {2, 2560, 1440, 120, 1, 8, 0, 0};  ///< Second test connector.
}  // namespace

TEST(SolVddTest, EncodesTopologyRequests) {
  const auto query = sol_vdd::encode_query_request(7);
  ASSERT_EQ(query.size(), sizeof(sol_vdd::request_header_t));
  sol_vdd::request_header_t query_header {};
  std::memcpy(&query_header, query.data(), sizeof(query_header));
  EXPECT_EQ(query_header.magic, sol_vdd::PROTOCOL_MAGIC);
  EXPECT_EQ(query_header.version, sol_vdd::PROTOCOL_VERSION);
  EXPECT_EQ(query_header.request_id, 7U);
  EXPECT_EQ(query_header.command, static_cast<std::uint32_t>(sol_vdd::command_t::query_state));
  EXPECT_EQ(query_header.connector_count, 0U);

  const auto apply = sol_vdd::encode_apply_request(8, {{3, 3840, 2160, 240, 1, 10, true}});
  ASSERT_EQ(apply.size(), sizeof(sol_vdd::request_header_t) + sizeof(sol_vdd::connector_state_t));
  sol_vdd::request_header_t apply_header {};
  std::memcpy(&apply_header, apply.data(), sizeof(apply_header));
  EXPECT_EQ(apply_header.request_id, 8U);
  EXPECT_EQ(apply_header.command, static_cast<std::uint32_t>(sol_vdd::command_t::apply_topology));
  EXPECT_EQ(apply_header.connector_count, 1U);
  sol_vdd::connector_state_t record {};
  std::memcpy(&record, apply.data() + sizeof(apply_header), sizeof(record));
  EXPECT_EQ(record.slot, 3U);
  EXPECT_EQ(record.width, 3840U);
  EXPECT_EQ(record.refresh_numerator, 240U);
  EXPECT_EQ(record.bit_depth, 10U);
  EXPECT_EQ(record.hdr, 1U);
}

TEST(SolVddTest, DecodesCommittedTopology) {
  const auto decoded = sol_vdd::decode_response(make_response(11, sol_vdd::status_t::success, {CONNECTOR_A, CONNECTOR_B}), 11);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->status, sol_vdd::status_t::success);
  ASSERT_EQ(decoded->connectors.size(), 2);
  EXPECT_EQ(decoded->connectors[0].slot, 0U);
  EXPECT_EQ(decoded->connectors[0].width, 1920U);
  EXPECT_TRUE(decoded->connectors[0].hdr);
  EXPECT_EQ(decoded->connectors[1].slot, 2U);
  EXPECT_EQ(decoded->connectors[1].bit_depth, 8U);
  EXPECT_FALSE(decoded->connectors[1].hdr);
}

TEST(SolVddTest, RejectsMalformedProtocolResponses) {
  EXPECT_FALSE(sol_vdd::decode_response({}, 1));
  EXPECT_FALSE(sol_vdd::decode_response(make_response(2, sol_vdd::status_t::success, {}), 1));

  auto wrong_magic = make_response(1, sol_vdd::status_t::success, {});
  sol_vdd::response_header_t header {};
  std::memcpy(&header, wrong_magic.data(), sizeof(header));
  header.magic = 0;
  std::memcpy(wrong_magic.data(), &header, sizeof(header));
  EXPECT_FALSE(sol_vdd::decode_response(wrong_magic, 1));

  auto wrong_version = make_response(1, sol_vdd::status_t::success, {});
  std::memcpy(&header, wrong_version.data(), sizeof(header));
  header.version = 99;
  std::memcpy(wrong_version.data(), &header, sizeof(header));
  EXPECT_FALSE(sol_vdd::decode_response(wrong_version, 1));

  auto unknown_status = make_response(1, static_cast<sol_vdd::status_t>(99), {});
  EXPECT_FALSE(sol_vdd::decode_response(unknown_status, 1));

  auto invalid_record = make_response(1, sol_vdd::status_t::success, {{0, 1920, 1080, 60, 1, 12, 0, 0}});
  EXPECT_FALSE(sol_vdd::decode_response(invalid_record, 1));

  auto reserved_record = make_response(1, sol_vdd::status_t::success, {{0, 1920, 1080, 60, 1, 10, 1, 5}});
  EXPECT_FALSE(sol_vdd::decode_response(reserved_record, 1));

  auto duplicate_slots = make_response(1, sol_vdd::status_t::success, {CONNECTOR_A, CONNECTOR_A});
  EXPECT_FALSE(sol_vdd::decode_response(duplicate_slots, 1));

  // Declare two records but provide one.
  auto truncated = make_response(1, sol_vdd::status_t::success, {CONNECTOR_A});
  std::memcpy(&header, truncated.data(), sizeof(header));
  header.connector_count = 2;
  std::memcpy(truncated.data(), &header, sizeof(header));
  EXPECT_FALSE(sol_vdd::decode_response(truncated, 1));
}

TEST(SolVddTest, ParsesSlotFromEdidSerialNumber) {
  std::vector<std::byte> edid(128);
  ASSERT_FALSE(sol_vdd::monitor_slot_from_edid(edid));
  const auto serial = sol_vdd::EDID_SERIAL_BASE + 3;
  edid[12] = static_cast<std::byte>(serial & 0xFF);
  edid[13] = static_cast<std::byte>((serial >> 8) & 0xFF);
  edid[14] = static_cast<std::byte>((serial >> 16) & 0xFF);
  edid[15] = static_cast<std::byte>((serial >> 24) & 0xFF);
  EXPECT_EQ(sol_vdd::monitor_slot_from_edid(edid), 3U);
  edid.resize(64);
  EXPECT_FALSE(sol_vdd::monitor_slot_from_edid(edid));
}

TEST(SolVddTest, ValidatesConnectorRecords) {
  EXPECT_TRUE(sol_vdd::valid_connector({0, 1920, 1080, 60, 1, 8, false}));
  EXPECT_TRUE(sol_vdd::valid_connector({15, 7680, 4320, 120, 1, 10, true}));
  EXPECT_FALSE(sol_vdd::valid_connector({16, 1920, 1080, 60, 1, 8, false}));
  EXPECT_FALSE(sol_vdd::valid_connector({0, 0, 1080, 60, 1, 8, false}));
  EXPECT_FALSE(sol_vdd::valid_connector({0, 1920, 1080, 0, 1, 8, false}));
  EXPECT_FALSE(sol_vdd::valid_connector({0, 1920, 1080, 60, 1, 12, false}));
}

TEST(SolVddTest, RecognizesOnlyVddMonitorIds) {
  EXPECT_TRUE(sol_vdd::is_monitor_id(LR"(DISPLAY\SLV1337\1&15ECD195&1&UID256)"));
  EXPECT_TRUE(sol_vdd::is_monitor_id(LR"(monitor\slv1337\instance)"));
  EXPECT_FALSE(sol_vdd::is_monitor_id(LR"(DISPLAY\SPD3301\5&1A80FEA3&6&UID4352)"));
  EXPECT_FALSE(sol_vdd::is_monitor_id(L"SLV1337"));
}

TEST(SolVddTest, MatchesInstanceIdAcrossWindowsPathSyntax) {
  EXPECT_TRUE(sol_vdd::monitor_id_matches_path(R"(DISPLAY\SLV1337\1&15ECD195&1&UID256)", R"(\\?\DISPLAY#SLV1337#1&15ecd195&1&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"));
  EXPECT_TRUE(sol_vdd::monitor_id_matches_path(R"(DISPLAY\SLV1337\UID256)", R"(\\?\DISPLAY#SLV1337#1&15ecd195&2&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"));
  EXPECT_FALSE(sol_vdd::monitor_id_matches_path(R"(DISPLAY\SLV1337\1&15ECD195&1&UID257)", R"(\\?\DISPLAY#SLV1337#1&15ecd195&1&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"));
}

TEST(SolVddTest, CanonicalizesConnectorIdentityAcrossDriverReinstall) {
  EXPECT_EQ(sol_vdd::canonical_monitor_id(R"(DISPLAY\SLV1337\1&15ECD195&1&UID256)"), "DISPLAY\\SLV1337\\UID256");
  EXPECT_EQ(sol_vdd::canonical_monitor_id(R"(DISPLAY\SLV1337\1&15ECD195&2&UID256)"), "DISPLAY\\SLV1337\\UID256");
  EXPECT_EQ(sol_vdd::canonical_monitor_id(R"(\\?\DISPLAY#SLV1337#1&15ecd195&2&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"), "DISPLAY\\SLV1337\\UID256");
  EXPECT_FALSE(sol_vdd::canonical_monitor_id(R"(DISPLAY\SPD3301\1&15ECD195&2&UID256)"));
  EXPECT_FALSE(sol_vdd::canonical_monitor_id(R"(DISPLAY\SLV1337\instance)"));
  EXPECT_FALSE(sol_vdd::canonical_monitor_id(R"(DISPLAY\SLV1337\1&15ECD195&2&UID256X)"));
  EXPECT_FALSE(sol_vdd::canonical_monitor_id(R"(DISPLAY\SLV1337\1&15ECD195&2&NOTUID256)"));
}

TEST(SolVddTest, InstalledProviderTopologyMatchesInventory) {
  const auto status = sol_vdd::probe();
  if (!status.available) {
    GTEST_SKIP() << status.reason_code;
  }
  const auto topology = sol_vdd::query_topology();
  const auto inventory = sol_vdd::display_inventory();
  ASSERT_TRUE(topology);
  ASSERT_TRUE(inventory);
  ASSERT_EQ(topology->size(), inventory->size());
  for (const auto &connector : *topology) {
    const auto present = std::ranges::find(*inventory, connector.slot, &sol_vdd::display_connector_t::slot);
    EXPECT_NE(present, inventory->end());
  }
  EXPECT_TRUE(terra::windows::virtual_display::available());
}

TEST(SolVddTest, DISABLED_ProductionProviderMultiDisplayRoundTripRestoresEmptyTopology) {
  if (!terra::windows::virtual_display::available()) {
    GTEST_SKIP() << "SolVDD provider unavailable";
  }
  const std::filesystem::path persistence_path = std::filesystem::path {SOL_TEST_BIN_DIR} / "solvdd-live-test.json";
  std::error_code error;
  std::filesystem::remove(persistence_path, error);
  terra_virtual_display::manager_t manager {terra::windows::virtual_display::make_callbacks(persistence_path)};
  ASSERT_TRUE(manager.available());
  std::vector<terra_virtual_display::resource_t> resources;
  for (int index = 0; index < 3; ++index) {
    const terra_virtual_display::specification_t specification {
      "Terra live test " + std::to_string(index + 1),
      {1920, 1080, 60, 1, 8, false},
      {4480 + 1920 * index, 0},
      1.0,
      0,
      false,
      false,
      false,
      std::nullopt,
    };
    const auto created = manager.create("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", specification);
    EXPECT_EQ(created.status, terra_virtual_display::status_t::success);
    if (!created.resource) {
      break;
    }
    resources.push_back(*created.resource);
  }
  EXPECT_EQ(resources.size(), 3);
  bool cleanup_succeeded = true;
  for (auto resource = resources.rbegin(); resource != resources.rend(); ++resource) {
    const auto current = manager.get(resource->id);
    if (!current) {
      ADD_FAILURE() << "Created live resource disappeared before cleanup";
      cleanup_succeeded = false;
      continue;
    }
    const auto removed = manager.remove(current->id, current->revision);
    EXPECT_EQ(removed.status, terra_virtual_display::status_t::success);
    cleanup_succeeded = cleanup_succeeded && removed.status == terra_virtual_display::status_t::success;
  }
  const auto inventory = sol_vdd::display_inventory();
  cleanup_succeeded = cleanup_succeeded && inventory && inventory->empty();
  if (cleanup_succeeded) {
    std::filesystem::remove(persistence_path, error);
  }
}
#endif
