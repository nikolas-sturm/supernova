/**
 * @file tests/unit/test_mttvdd.cpp
 * @brief Test MttVDD named-pipe protocol helpers.
 */

#include <gtest/gtest.h>

// local includes
#include <src/eclipse_virtual_display.h>
#include <src/platform/windows/eclipse_virtual_display_provider.h>
#include <src/platform/windows/mttvdd.h>

#ifdef _WIN32
TEST(MttVddTest, BuildsBoundedDisplayCountCommands) {
  EXPECT_EQ(mttvdd::display_count_command(0), L"SETDISPLAYCOUNT 0");
  EXPECT_EQ(mttvdd::display_count_command(99), L"SETDISPLAYCOUNT 99");
  EXPECT_FALSE(mttvdd::display_count_command(100));
}

TEST(MttVddTest, ValidatesSettingsProtocolResponse) {
  EXPECT_TRUE(mttvdd::is_settings_response(L"SETTINGS DEBUG=false LOG=false"));
  EXPECT_TRUE(mttvdd::is_settings_response(std::wstring_view {L"SETTINGS DEBUG=true LOG=true\0", 29}));
  EXPECT_FALSE(mttvdd::is_settings_response(L""));
  EXPECT_FALSE(mttvdd::is_settings_response(L"PONG"));
  EXPECT_FALSE(mttvdd::is_settings_response(L"SETTINGS DEBUG=false"));
  EXPECT_FALSE(mttvdd::is_settings_response(L"SETTINGS LOG=false"));
}

TEST(MttVddTest, ParsesBoundedConfiguredDisplayCount) {
  EXPECT_EQ(mttvdd::parse_display_count("<vdd_settings><monitors><count>0</count></monitors></vdd_settings>"), 0);
  EXPECT_EQ(mttvdd::parse_display_count("<vdd_settings><monitors><count>99</count></monitors></vdd_settings>"), 99);
  EXPECT_FALSE(mttvdd::parse_display_count("<vdd_settings><monitors><count>-1</count></monitors></vdd_settings>"));
  EXPECT_FALSE(mttvdd::parse_display_count("<vdd_settings><monitors><count>100</count></monitors></vdd_settings>"));
  EXPECT_FALSE(mttvdd::parse_display_count("<not-settings/>"));
  EXPECT_FALSE(mttvdd::parse_display_count("not xml"));
}

TEST(MttVddTest, RecognizesOnlyMttVddMonitorIds) {
  EXPECT_TRUE(mttvdd::is_monitor_id(LR"(DISPLAY\MTT1337\1&15ECD195&1&UID256)"));
  EXPECT_TRUE(mttvdd::is_monitor_id(LR"(monitor\mtt1337\instance)"));
  EXPECT_FALSE(mttvdd::is_monitor_id(LR"(DISPLAY\SPD3301\5&1A80FEA3&6&UID4352)"));
  EXPECT_FALSE(mttvdd::is_monitor_id(L"MTT1337"));
}

TEST(MttVddTest, MatchesInstanceIdAcrossWindowsPathSyntax) {
  EXPECT_TRUE(mttvdd::monitor_id_matches_path(R"(DISPLAY\MTT1337\1&15ECD195&1&UID256)", R"(\\?\DISPLAY#MTT1337#1&15ecd195&1&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"));
  EXPECT_FALSE(mttvdd::monitor_id_matches_path(R"(DISPLAY\MTT1337\1&15ECD195&1&UID257)", R"(\\?\DISPLAY#MTT1337#1&15ecd195&1&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7})"));
}

TEST(MttVddTest, InstalledProviderCountMatchesFilteredInventory) {
  const auto status = mttvdd::probe();
  if (!status.available) {
    GTEST_SKIP() << status.reason_code;
  }
  const auto count = mttvdd::configured_display_count();
  const auto inventory = mttvdd::display_inventory();
  ASSERT_TRUE(count);
  ASSERT_TRUE(inventory);
  EXPECT_EQ(inventory->size(), *count);
  EXPECT_TRUE(eclipse::windows::virtual_display::available());
}

TEST(MttVddTest, DISABLED_ProductionProviderRoundTripRestoresBaseline) {
  if (!eclipse::windows::virtual_display::available()) {
    GTEST_SKIP() << "MttVDD provider unavailable";
  }
  const auto before = mttvdd::configured_display_count();
  ASSERT_TRUE(before);
  const std::filesystem::path persistence_path = std::filesystem::path {SUNSHINE_TEST_BIN_DIR} / "mttvdd-live-test.json";
  std::error_code error;
  std::filesystem::remove(persistence_path, error);
  eclipse_virtual_display::manager_t manager {eclipse::windows::virtual_display::make_callbacks(persistence_path)};
  ASSERT_TRUE(manager.available());
  const eclipse_virtual_display::specification_t specification {
    "Eclipse live test",
    {1920, 1080, 60, 1, 8, false},
    {4480, 0},
    1.0,
    0,
    false,
    false,
    false,
    std::nullopt,
  };
  const auto created = manager.create("aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa", specification);
  EXPECT_EQ(created.status, eclipse_virtual_display::status_t::success);
  if (created.resource) {
    EXPECT_EQ(manager.remove(created.resource->id, created.resource->revision).status, eclipse_virtual_display::status_t::success);
  }
  EXPECT_EQ(mttvdd::configured_display_count(), before);
  std::filesystem::remove(persistence_path, error);
}
#endif
