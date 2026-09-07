/**
 * @file tests/unit/test_terra_api.cpp
 * @brief Test Terra API capability and discovery metadata.
 */

// standard includes
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/nvhttp.h>
#include <src/process.h>

TEST(TerraApiTest, DerivesRfc6455AcceptKey) {
  EXPECT_EQ(
    nvhttp::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ=="),
    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
  );
}

TEST(TerraApiTest, RoundTripsWebSocketFrames) {
  const auto frame = nvhttp::websocket_frame(0x1, "{\"type\":\"claim.open\"}");
  ASSERT_EQ(frame.front(), 0x81);
  const auto decoded = nvhttp::websocket_decode_frame(frame);
  // Server frames are unmasked, so decoding them again is malformed input.
  EXPECT_FALSE(decoded);

  std::vector<std::uint8_t> masked {0x81, 0x80 | 5, 0x01, 0x02, 0x03, 0x04, 0x49, 0x67, 0x6F, 0x68, 0x6E};
  const auto client = nvhttp::websocket_decode_frame(masked);
  ASSERT_TRUE(client);
  EXPECT_EQ(client->first, 0x1);
  EXPECT_EQ(client->second, "Hello");

  // A 126-bit extended length claims 256 payload bytes that never arrive.
  std::vector<std::uint8_t> long_frame {0x82, 0x80 | 126, 0x01, 0x00, 0, 0, 0, 0};
  EXPECT_FALSE(nvhttp::websocket_decode_frame(long_frame));

  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x81}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x81, 0x05, 'a', 'b'}));
}

TEST(TerraApiTest, SerializesOperationalCapabilitiesForServerInfo) {
  EXPECT_EQ(
    terra_api::capabilities_csv(),
    "client-permissions,session-ids,structured-errors,catalog-v2,events-v1,profiles-v1,workspaces-v1,telemetry-v1"
  );
}

TEST(TerraApiTest, RequiresCompleteExplicitPairingPolicy) {
  EXPECT_FALSE(terra_api::parse_pairing_policy("catalog.read", std::nullopt).valid);
  EXPECT_FALSE(terra_api::parse_pairing_policy(std::nullopt, "keyboard").valid);
  EXPECT_FALSE(terra_api::parse_pairing_policy("unknown.scope", "keyboard").valid);
  EXPECT_FALSE(terra_api::parse_pairing_policy("catalog.read", "unknown-input").valid);
}

TEST(TerraApiTest, ParsesExplicitPairingPolicyWithoutImplicitGrants) {
  const auto policy = terra_api::parse_pairing_policy("catalog.read,session.control", "keyboard,controller");

  ASSERT_TRUE(policy.valid);
  ASSERT_TRUE(policy.explicit_policy);
  EXPECT_EQ(policy.permissions.scopes.size(), 2);
  EXPECT_TRUE(policy.permissions.scopes.contains("catalog.read"));
  EXPECT_TRUE(policy.permissions.scopes.contains("session.control"));
  EXPECT_TRUE(policy.permissions.input.keyboard);
  EXPECT_FALSE(policy.permissions.input.mouse);
  EXPECT_TRUE(policy.permissions.input.controller);
  EXPECT_FALSE(policy.permissions.input.touch);
  EXPECT_FALSE(policy.permissions.input.pen);

  const auto empty_policy = terra_api::parse_pairing_policy("", "");
  EXPECT_TRUE(empty_policy.valid);
  EXPECT_TRUE(empty_policy.explicit_policy);
  EXPECT_TRUE(empty_policy.permissions.scopes.empty());
  EXPECT_FALSE(empty_policy.permissions.input.keyboard);
}

TEST(TerraApiTest, PreservesLegacyPairingPolicyWhenFieldsAreAbsent) {
  const auto policy = terra_api::parse_pairing_policy(std::nullopt, std::nullopt);

  ASSERT_TRUE(policy.valid);
  ASSERT_FALSE(policy.explicit_policy);
  EXPECT_EQ(policy.permissions.scopes.size(), terra_api::SCOPES.size());
  EXPECT_TRUE(policy.permissions.input.keyboard);
  EXPECT_TRUE(policy.permissions.input.mouse);
  EXPECT_TRUE(policy.permissions.input.controller);
  EXPECT_TRUE(policy.permissions.input.touch);
  EXPECT_TRUE(policy.permissions.input.pen);
}

TEST(TerraApiTest, RecognizesOnlyExplicitApiV1OptIn) {
  EXPECT_TRUE(terra_api::api_v1_requested("1"));
  EXPECT_FALSE(terra_api::api_v1_requested(""));
  EXPECT_FALSE(terra_api::api_v1_requested("2"));
}

TEST(TerraApiTest, ValidatesWakeOnLanAddresses) {
  EXPECT_TRUE(terra_api::wake_on_lan_available("02:00:5e:10:00:00"));
  EXPECT_TRUE(terra_api::wake_on_lan_available("AA:BB:CC:DD:EE:FF"));
  EXPECT_FALSE(terra_api::wake_on_lan_available("00:00:00:00:00:00"));
  EXPECT_FALSE(terra_api::wake_on_lan_available("01:00:5e:00:00:01"));
  EXPECT_FALSE(terra_api::wake_on_lan_available("not-a-mac"));
  EXPECT_FALSE(terra_api::wake_on_lan_available("00-11-22-33-44-55"));
}

TEST(TerraApiTest, BuildsCompleteCapabilityDocument) {
  terra_api::client_permissions_t permissions;
  permissions.scopes.emplace("catalog.read");
  permissions.allowed_apps.emplace("11111111-1111-1111-1111-111111111111");
  permissions.expires_at = 4102444800;

  const auto document = nvhttp::test_support::capabilities_document(
    "22222222-2222-2222-2222-222222222222",
    "Terra client",
    permissions,
    "33333333-3333-3333-3333-333333333333",
    "Sol host",
    "windows",
    "1.0.0",
    true
  );

  EXPECT_EQ(document["apiVersion"], 1);
  EXPECT_EQ(document["client"]["uuid"], "22222222-2222-2222-2222-222222222222");
  EXPECT_EQ(document["client"]["scopes"], nlohmann::json::array({"catalog.read"}));
  EXPECT_EQ(document["client"]["allowedApps"], nlohmann::json::array({"11111111-1111-1111-1111-111111111111"}));
  EXPECT_EQ(document["client"]["expiresAt"], 4102444800);
  EXPECT_EQ(document["host"]["uuid"], "33333333-3333-3333-3333-333333333333");
  EXPECT_EQ(document["host"]["name"], "Sol host");
  EXPECT_EQ(document["host"]["platform"], "windows");
  EXPECT_EQ(document["host"]["version"], "1.0.0");
  EXPECT_TRUE(document["host"]["wakeOnLanAvailable"]);
  EXPECT_EQ(document["features"].size(), terra_api::KNOWN_CAPABILITIES.size());
  EXPECT_TRUE(document["features"]["catalog-v2"]["available"]);
  EXPECT_FALSE(document["features"]["catalog-v2"].contains("reasonCode"));
  EXPECT_TRUE(document["features"]["events-v1"]["available"]);
  EXPECT_TRUE(document["features"]["profiles-v1"]["available"]);
  EXPECT_TRUE(document["features"]["workspaces-v1"]["available"]);
  EXPECT_TRUE(document["features"]["telemetry-v1"]["available"]);
  EXPECT_FALSE(document["features"]["discovery-v1"]["available"]);
  EXPECT_FALSE(document["features"]["virtual-displays-v1"]["available"]);
#ifdef _WIN32
  EXPECT_TRUE(document["features"]["displays-v1"]["available"]);
#else
  EXPECT_FALSE(document["features"]["displays-v1"]["available"]);
#endif
#ifdef _WIN32
  // Sandbox and peripheral availability depend on live providers created by other
  // suites; every advertised capability must be an available feature entry.
  for (const auto &item : document["capabilities"].items()) {
    const auto name = item.value().get<std::string>();
    ASSERT_TRUE(document["features"].contains(name)) << name;
    EXPECT_TRUE(document["features"][name]["available"]) << name;
  }
  EXPECT_GE(document["capabilities"].size(), terra_api::CAPABILITIES.size());
  EXPECT_EQ(document["limits"]["sandboxes"]["maxActive"], document["features"]["sandboxes-v1"]["available"].get<bool>() ? 4 : 0);
  EXPECT_EQ(
    document["limits"]["peripherals"]["maxDevices"],
    document["features"]["peripherals-v1"]["available"].get<bool>() ? 32 : 0
  );
#else
  EXPECT_EQ(document["capabilities"].size(), terra_api::CAPABILITIES.size());
  EXPECT_FALSE(document["features"]["sandboxes-v1"]["available"]);
  EXPECT_FALSE(document["features"]["sandboxes-v1"]["reasonCode"].get<std::string>().empty());
#endif
  EXPECT_EQ(document["limits"]["sessions"]["maxActive"], 1);
  EXPECT_EQ(document["limits"]["displays"]["maxManaged"], 0);
  EXPECT_EQ(document["limits"]["virtualDisplays"]["maxActive"], 0);
  EXPECT_EQ(document["limits"]["workspaces"]["maxActive"], 1);
  EXPECT_EQ(document["limits"]["sandboxes"]["maxActive"], document["features"]["sandboxes-v1"]["available"].get<bool>() ? 4 : 0);
  const bool peripherals_available = document["features"]["peripherals-v1"]["available"].get<bool>();
  EXPECT_EQ(document["limits"]["peripherals"]["maxDevices"], peripherals_available ? 32 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxClaims"], peripherals_available ? 32 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxMessageBytes"], peripherals_available ? 65536 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxPayloadBytes"], peripherals_available ? 49152 : 0);
}

TEST(TerraApiTest, BuildsCatalogResourceWithoutUnsupportedProfileLinks) {
  const auto image_path = std::filesystem::path {SOL_TEST_BIN_DIR} / "terra-catalog-resource.png";
  const std::vector<std::uint8_t> png {
    0x89,
    0x50,
    0x4E,
    0x47,
    0x0D,
    0x0A,
    0x1A,
    0x0A,
    0x00,
    0x00,
    0x00,
    0x0D,
    'I',
    'H',
    'D',
    'R',
    0x00,
    0x00,
    0x02,
    0x80,
    0x00,
    0x00,
    0x01,
    0xE0,
  };
  {
    std::ofstream image {image_path, std::ios::binary};
    image.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
  }
  proc::ctx_t app;
  app.uuid = "11111111-1111-1111-1111-111111111111";
  app.id = "42";
  app.name = "Example";
  app.image_path = image_path.string();
  app.terra_metadata = {
    {"kind", "game"},
    {"tags", {"controller", "controller", 7}},
    {"classification", {{"source", "user"}, {"confidence", 0.75}}},
    {"hdr", true},
    {"inputRequirements", {"controller", "unknown", "controller"}},
    {"launchProfiles", {
                         {{"id", "55555555-5555-5555-5555-555555555555"}, {"name", "Default"}, {"default", true}},
                         {{"id", "66666666-6666-6666-6666-666666666666"}, {"name", "Alternative"}, {"default", false}},
                         {{"id", "77777777-7777-7777-7777-777777777777"}, {"name", "Duplicate default"}, {"default", true}},
                       }},
    {"displayProfileId", "22222222-2222-2222-2222-222222222222"},
    {"streamProfileId", "33333333-3333-3333-3333-333333333333"},
    {"sandboxProfileId", "44444444-4444-4444-4444-444444444444"},
  };

  const auto resource = nvhttp::test_support::catalog_app_document(app);
  std::error_code remove_error;
  std::filesystem::remove(image_path, remove_error);

  EXPECT_EQ(resource["uuid"], app.uuid);
  EXPECT_EQ(resource["legacyId"], 42);
  EXPECT_EQ(resource["kind"], "game");
  EXPECT_EQ(resource["classification"]["source"], "user");
  EXPECT_EQ(resource["classification"]["confidence"], 0.75);
  EXPECT_EQ(resource["tags"], nlohmann::json::array({"controller"}));
  EXPECT_EQ(resource["inputRequirements"], nlohmann::json::array({"controller"}));
  EXPECT_TRUE(resource["hdr"]);
  ASSERT_EQ(resource["launchProfiles"].size(), 2);
  EXPECT_EQ(resource["launchProfiles"][0], nlohmann::json({{"id", "55555555-5555-5555-5555-555555555555"}, {"name", "Default"}, {"default", true}}));
  EXPECT_EQ(resource["launchProfiles"][1], nlohmann::json({{"id", "66666666-6666-6666-6666-666666666666"}, {"name", "Alternative"}, {"default", false}}));
  EXPECT_TRUE(resource["displayProfileId"].is_null());
  EXPECT_TRUE(resource["streamProfileId"].is_null());
  EXPECT_TRUE(resource["sandboxProfileId"].is_null());
  EXPECT_TRUE(resource["assets"].contains("poster"));
  EXPECT_TRUE(resource["assets"].contains("icon"));
}

TEST(TerraApiTest, CatalogDefaultsDoNotDependOnActiveEncoderState) {
  proc::ctx_t app;
  app.uuid = "11111111-1111-1111-1111-111111111111";
  app.id = "42";
  app.name = "Example";
  app.image_path = "missing-terra-catalog-image.png";
  app.terra_metadata = nlohmann::json::object();

  const auto resource = nvhttp::test_support::catalog_app_document(app);

  EXPECT_FALSE(resource["hdr"]);
  EXPECT_EQ(resource["kind"], "unknown");
  EXPECT_EQ(resource["classification"]["source"], "unknown");
  EXPECT_EQ(resource["classification"]["confidence"], 0.0);
}

TEST(TerraApiTest, BoundsRequestBodyBeforeMaterializingOverflow) {
  std::istringstream exact {"12345678"};
  auto accepted = terra_api::read_bounded_body(exact, 8);
  EXPECT_EQ(accepted.status, terra_api::body_read_status_t::success);
  EXPECT_EQ(accepted.text, "12345678");

  std::istringstream oversized {"123456789"};
  auto rejected = terra_api::read_bounded_body(oversized, 8);
  EXPECT_EQ(rejected.status, terra_api::body_read_status_t::too_large);
  EXPECT_TRUE(rejected.text.empty());
}

TEST(TerraApiTest, ProjectsEventCollectionsFromReadScopes) {
  terra_api::client_permissions_t permissions;
  permissions.scopes = {"catalog.read", "display.read", "telemetry.read"};
  const auto collections = nvhttp::test_support::event_collections(permissions);
  EXPECT_EQ(collections, std::vector<std::string>({"host", "capabilities", "operations", "catalog", "workspaces", "displays", "virtualDisplays", "telemetry", "profiles"}));
  EXPECT_EQ(std::ranges::count(collections, "profiles"), 1);
  EXPECT_EQ(std::ranges::count(collections, "sessions"), 0);
  EXPECT_EQ(std::ranges::count(collections, "sandboxes"), 0);
}
