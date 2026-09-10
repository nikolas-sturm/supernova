/**
 * @file tests/unit/test_terra_display.cpp
 * @brief Tests for Windows Terra display snapshot helpers.
 */

#ifdef _WIN32
  // platform includes
  #include <windows.h>

  // test includes
  #include <gtest/gtest.h>
  #include <nlohmann/json.hpp>

  // local includes
  #include <src/platform/windows/terra_display.h>

namespace {
  using namespace terra::windows::display;
}  // namespace

TEST(TerraDisplayTest, ReducesAndRejectsRationals) {
  EXPECT_EQ(make_rational(60000, 1000), (Rational {60, 1}));
  EXPECT_EQ(make_rational(0, 7), (Rational {0, 1}));
  EXPECT_FALSE(make_rational(1, 0));
  EXPECT_FALSE(make_rational(std::uint64_t {std::numeric_limits<std::uint32_t>::max()} + 1, 1));
}

TEST(TerraDisplayTest, ConvertsScaleExactly) {
  EXPECT_EQ(scale_rational(display_device::Rational {3, 2}), (Rational {3, 2}));
  EXPECT_EQ(scale_rational(1.25), (Rational {5, 4}));
  EXPECT_EQ(scale_rational(0.0), (Rational {0, 1}));
  EXPECT_FALSE(scale_rational(-1.0));
  EXPECT_FALSE(scale_rational(std::numeric_limits<double>::infinity()));
  EXPECT_FALSE(scale_rational(std::numeric_limits<double>::denorm_min()));
  EXPECT_FALSE(scale_rational(static_cast<double>(std::numeric_limits<std::uint32_t>::max()) + 1.0));
}

TEST(TerraDisplayTest, ComputesLogicalSize) {
  EXPECT_EQ(logical_size({3840, 2160}, {3, 2}), (Size {2560, 1440}));
  EXPECT_FALSE(logical_size({1920, 1080}, {0, 1}));
  EXPECT_FALSE(logical_size({1920, 1080}, {1, 0}));
}

TEST(TerraDisplayTest, BuildsStableModeIdWithoutRefreshRounding) {
  EXPECT_EQ(stable_mode_id({1920, 1080}, {60000, 1001}, 10, true), "1920x1080@60000/1001:10:hdr");
  EXPECT_NE(stable_mode_id({1920, 1080}, {60, 1}, 10, true), stable_mode_id({1920, 1080}, {60000, 1001}, 10, true));
  EXPECT_NE(stable_mode_id({1920, 1080}, {60, 1}, 8, false), stable_mode_id({1920, 1080}, {60, 1}, 10, true));
}

TEST(TerraDisplayTest, DerivesDeterministicHostScopedUuid) {
  const auto first = display_resource_uuid("host-a", "device-a");
  EXPECT_EQ(first, display_resource_uuid("host-a", "device-a"));
  EXPECT_NE(first, display_resource_uuid("host-b", "device-a"));
  EXPECT_NE(first, display_resource_uuid("host-a", "device-b"));
  EXPECT_EQ(first.size(), 36);
  EXPECT_EQ(first[14], '8');
  EXPECT_TRUE(first[19] == '8' || first[19] == '9' || first[19] == 'a' || first[19] == 'b');
}

TEST(TerraDisplayTest, ClassifiesOutputTechnologyWithoutNames) {
  EXPECT_EQ(kind_from_output_technology(DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL), Kind::Internal);
  EXPECT_EQ(kind_from_output_technology(DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI), Kind::External);
  EXPECT_EQ(kind_from_output_technology(DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED), Kind::Virtual);
  EXPECT_EQ(kind_from_output_technology(DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER), Kind::Unknown);
}

TEST(TerraDisplayTest, SerializesOnlyContractDisplayKinds) {
  EXPECT_EQ(kind_name(Kind::Unknown), "physical");
  EXPECT_EQ(kind_name(Kind::Internal), "physical");
  EXPECT_EQ(kind_name(Kind::External), "physical");
  EXPECT_EQ(kind_name(Kind::Virtual), "virtual");
}

TEST(TerraDisplayTest, TransformsDevicesWithoutGuessingKind) {
  const display_device::EnumeratedDeviceList devices {
    {.m_device_id = "stable-id",
     .m_display_name = R"(\\.\DISPLAY1)",
     .m_friendly_name = "Friendly panel",
     .m_info = display_device::EnumeratedDevice::Info {
       .m_resolution = {3840, 2160},
       .m_resolution_scale = display_device::Rational {3, 2},
       .m_refresh_rate = display_device::Rational {60000, 1001},
       .m_primary = true,
       .m_origin_point = {-100, 20},
       .m_hdr_state = display_device::HdrState::Enabled
     }},
    {.m_device_id = "disabled", .m_display_name = "DISPLAY2"}
  };
  const Mode supported {stable_mode_id({3840, 2160}, {60000, 1001}, 10, true), {3840, 2160}, {60000, 1001}, 10, true};
  const std::vector<PlatformDetails> details {
    {.device_id = "stable-id", .enabled = true, .capture_eligible = true, .kind = Kind::Unknown, .hdr_supported = true, .current_bit_depth = 10, .supported_modes = {supported}}
  };

  const auto snapshots = make_snapshot("host", devices, details);
  ASSERT_EQ(snapshots.size(), 2);
  EXPECT_EQ(snapshots[0].name, "Friendly panel");
  EXPECT_EQ(snapshots[0].platform_id, R"(\\.\DISPLAY1)");
  EXPECT_TRUE(snapshots[0].enabled);
  EXPECT_TRUE(snapshots[0].primary);
  EXPECT_EQ(snapshots[0].position, (Position {-100, 20}));
  EXPECT_EQ(snapshots[0].logical_size, (Size {2560, 1440}));
  EXPECT_EQ(snapshots[0].scale, (Rational {3, 2}));
  EXPECT_EQ(snapshots[0].current_mode, supported);
  EXPECT_EQ(snapshots[0].supported_modes, std::vector<Mode> {supported});
  EXPECT_EQ(snapshots[0].hdr_supported, true);
  EXPECT_EQ(snapshots[0].hdr_enabled, true);
  EXPECT_TRUE(snapshots[0].capture_eligible);
  EXPECT_EQ(snapshots[0].kind, Kind::Unknown);
  EXPECT_FALSE(snapshots[1].enabled);
  EXPECT_FALSE(snapshots[1].capture_eligible);
  EXPECT_EQ(snapshots[1].kind, Kind::Unknown);
  EXPECT_FALSE(snapshots[1].current_mode);
}

TEST(TerraDisplayTest, UsesContractBitDepthWithoutDxgiMetadata) {
  const display_device::EnumeratedDeviceList devices {{
    .m_device_id = "indirect-display",
    .m_display_name = R"(\\.\DISPLAY2)",
    .m_info = display_device::EnumeratedDevice::Info {
      .m_resolution = {1920, 1080},
      .m_resolution_scale = display_device::Rational {1, 1},
      .m_refresh_rate = display_device::Rational {60, 1},
      .m_hdr_state = std::nullopt,
    },
  }};

  const auto snapshots = make_snapshot("host", devices, {});
  ASSERT_EQ(snapshots.size(), 1);
  ASSERT_TRUE(snapshots.front().current_mode);
  EXPECT_EQ(snapshots.front().current_mode->bit_depth, 8);
  EXPECT_GT(to_json(snapshots.front()).at("currentMode").at("bitDepth"), 0);
}

TEST(TerraDisplayTest, SerializesExactContractWithoutInternalDeviceId) {
  const terra::windows::display::Snapshot snapshot {
    .resource_uuid = "aaaaaaaa-aaaa-8aaa-8aaa-aaaaaaaaaaaa",
    .device_id = "{private-device-id}",
    .platform_id = R"(\\.\DISPLAY1)",
    .name = "Display",
    .enabled = true,
    .primary = true,
    .position = {-100, 20},
    .logical_size = {1280, 720},
    .scale = {3, 2},
    .current_mode = terra::windows::display::Mode {"1920x1080@60/1:10:hdr", {1920, 1080}, {60, 1}, 10, true},
    .supported_modes = {{"1920x1080@60/1:10:hdr", {1920, 1080}, {60, 1}, 10, true}},
    .hdr_supported = true,
    .hdr_enabled = true,
    .capture_eligible = true,
    .kind = terra::windows::display::Kind::Virtual,
  };
  const auto json = terra::windows::display::to_json(snapshot);
  EXPECT_EQ(json.at("kind"), "virtual");
  EXPECT_EQ(json.at("position").at("x"), -100);
  EXPECT_EQ(json.at("currentMode").at("refreshNumerator"), 60);
  EXPECT_EQ(json.at("scale"), (nlohmann::json {{"numerator", 3}, {"denominator", 2}}));
  EXPECT_FALSE(json.contains("deviceId"));
}

TEST(TerraDisplayTest, SerializesPhysicalDisplayKind) {
  Snapshot snapshot;
  snapshot.kind = Kind::Internal;

  EXPECT_EQ(to_json(snapshot).at("kind"), "physical");
}
#endif
