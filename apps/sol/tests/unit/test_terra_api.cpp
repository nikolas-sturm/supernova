/**
 * @file tests/unit/test_terra_api.cpp
 * @brief Test Terra API capability and discovery metadata.
 */

// standard includes
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/process.h>

TEST(TerraApiTest, DerivesRfc6455AcceptKey) {
  EXPECT_EQ(
    nvhttp::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ=="),
    "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
  );
}

TEST(TerraApiTest, ParsesCommaSeparatedHttpHeaderTokens) {
  EXPECT_TRUE(nvhttp::test_support::http_header_contains_token("keep-alive, Upgrade", "upgrade"));
  EXPECT_TRUE(nvhttp::test_support::http_header_contains_token(" websocket \t", "WebSocket"));
  EXPECT_FALSE(nvhttp::test_support::http_header_contains_token("notupgrade", "upgrade"));
  EXPECT_FALSE(nvhttp::test_support::http_header_contains_token(" , \t", "upgrade"));
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

  // A 16-bit extended length claims 256 payload bytes that never arrive.
  std::vector<std::uint8_t> long_frame {0x82, 0x80 | 126, 0x01, 0x00, 0, 0, 0, 0};
  EXPECT_FALSE(nvhttp::websocket_decode_frame(long_frame));

  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x81}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x81, 0x05, 'a', 'b'}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x01, 0x80, 0, 0, 0, 0}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0xC1, 0x80, 0, 0, 0, 0}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x82, 0x80, 0, 0, 0, 0}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x81, 0x80, 0, 0, 0, 0, 'x'}));

  std::vector<std::uint8_t> noncanonical {0x81, 0x80 | 126, 0, 5, 0, 0, 0, 0};
  noncanonical.resize(13);
  EXPECT_FALSE(nvhttp::websocket_decode_frame(noncanonical));

  std::vector<std::uint8_t> oversized_control {0x89, 0x80 | 126, 0, 126, 0, 0, 0, 0};
  oversized_control.resize(134);
  EXPECT_FALSE(nvhttp::websocket_decode_frame(oversized_control));

  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x88, 0x81, 0, 0, 0, 0, 0}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x88, 0x82, 0, 0, 0, 0, 0x03, 0xED}));
  EXPECT_FALSE(nvhttp::websocket_decode_frame({0x88, 0x83, 0, 0, 0, 0, 0x03, 0xE8, 0xFF}));
  const auto valid_close = nvhttp::websocket_decode_frame({0x88, 0x82, 0, 0, 0, 0, 0x03, 0xE8});
  ASSERT_TRUE(valid_close);
  EXPECT_EQ(valid_close->first, 0x8);
}

TEST(TerraApiTest, SerializesOperationalCapabilitiesForServerInfo) {
  EXPECT_EQ(
    terra_api::capabilities_csv(),
    "client-permissions,session-ids,structured-errors,catalog-v2"
  );
}

TEST(TerraApiTest, RequiresDnsSdAndHttpForDiscoveryHealth) {
  std::vector<bool> availability_changes;
  platf::publish::set_availability_callback([&](const bool available) {
    availability_changes.push_back(available);
  });
  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
  auto health = platf::publish::health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.registration, platf::publish::state_t::stopped);
  EXPECT_FALSE(health.http_available);
  EXPECT_EQ(health.reason_code, "server_unavailable");
  EXPECT_EQ(health.reason, "GameStream discovery HTTP server is not listening");

  platf::publish::set_http_available(true);
  health = platf::publish::health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.reason_code, "provider_stopped");
  EXPECT_EQ(health.reason, "DNS-SD service registration is stopped");

  platf::publish::set_registration_state(platf::publish::state_t::starting);
  health = platf::publish::health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.reason_code, "provider_starting");
  EXPECT_EQ(health.reason, "DNS-SD service registration is in progress");

  platf::publish::set_registration_state(platf::publish::state_t::unavailable);
  health = platf::publish::health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.reason_code, "provider_unavailable");
  EXPECT_EQ(health.reason, "DNS-SD service registration is unavailable");

  platf::publish::set_registration_state(platf::publish::state_t::unavailable, "registration_failed", "registration failed");
  health = platf::publish::health();
  EXPECT_FALSE(health.available);
  EXPECT_EQ(health.reason_code, "registration_failed");
  EXPECT_EQ(health.reason, "registration failed");

  platf::publish::set_registration_state(platf::publish::state_t::available);
  health = platf::publish::health();
  EXPECT_TRUE(health.available);
  EXPECT_EQ(health.registration, platf::publish::state_t::available);
  EXPECT_TRUE(health.http_available);
  EXPECT_TRUE(health.reason_code.empty());
  EXPECT_TRUE(health.reason.empty());

  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
  platf::publish::set_availability_callback({});
  EXPECT_EQ(availability_changes, std::vector<bool>({true, false}));
}

TEST(TerraApiTest, DeliversConcurrentDiscoveryTransitionsInOrder) {
  platf::publish::set_availability_callback({});
  platf::publish::set_http_available(true);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);

  std::mutex gate_mutex;
  std::condition_variable gate_condition;
  bool available_callback_entered = false;
  bool release_available_callback = false;
  std::mutex changes_mutex;
  std::vector<bool> availability_changes;
  platf::publish::set_availability_callback([&](const bool available) {
    if (available) {
      std::unique_lock lock {gate_mutex};
      available_callback_entered = true;
      gate_condition.notify_one();
      gate_condition.wait(lock, [&] {
        return release_available_callback;
      });
    }
    std::lock_guard changes_lock {changes_mutex};
    availability_changes.push_back(available);
  });

  std::jthread available_thread([] {
    platf::publish::set_registration_state(platf::publish::state_t::available);
  });
  bool available_callback_started = false;
  {
    std::unique_lock lock {gate_mutex};
    available_callback_started = gate_condition.wait_for(lock, std::chrono::seconds {1}, [&] {
      return available_callback_entered;
    });
  }
  std::jthread unavailable_thread([] {
    platf::publish::set_registration_state(platf::publish::state_t::unavailable);
  });

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds {1};
  while (platf::publish::health().available && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const bool unavailable_state_committed = !platf::publish::health().available;
  {
    std::lock_guard lock {gate_mutex};
    release_available_callback = true;
  }
  gate_condition.notify_one();
  available_thread.join();
  unavailable_thread.join();

  platf::publish::set_availability_callback({});
  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
  EXPECT_TRUE(available_callback_started);
  EXPECT_TRUE(unavailable_state_committed);
  EXPECT_EQ(availability_changes, std::vector<bool>({true, false}));
}

TEST(TerraApiTest, SuppressesSupersededDiscoveryTransitions) {
  platf::publish::set_availability_callback({});
  platf::publish::set_http_available(true);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);

  std::vector<bool> availability_changes;
  platf::publish::set_availability_callback([&](const bool available) {
    availability_changes.push_back(available);
  });

  bool available_state_committed = false;
  bool unavailable_state_committed = false;
  {
    std::unique_lock delivery_lock {platf::publish::health_state.callback_mutex};
    std::jthread available_thread([] {
      platf::publish::set_registration_state(platf::publish::state_t::available);
    });
    const auto available_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {1};
    while (!platf::publish::health().available && std::chrono::steady_clock::now() < available_deadline) {
      std::this_thread::yield();
    }
    available_state_committed = platf::publish::health().available;

    std::jthread unavailable_thread([] {
      platf::publish::set_registration_state(platf::publish::state_t::unavailable);
    });
    const auto unavailable_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {1};
    while (platf::publish::health().available && std::chrono::steady_clock::now() < unavailable_deadline) {
      std::this_thread::yield();
    }
    unavailable_state_committed = !platf::publish::health().available;
    delivery_lock.unlock();
    available_thread.join();
    unavailable_thread.join();
  }

  platf::publish::set_availability_callback({});
  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
  EXPECT_TRUE(available_state_committed);
  EXPECT_TRUE(unavailable_state_committed);
  EXPECT_EQ(availability_changes, std::vector<bool>({false}));
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

TEST(TerraApiTest, MatchesOnlyTerraApiNamespace) {
  EXPECT_TRUE(nvhttp::test_support::is_terra_api_path("/eclipse/v1"));
  EXPECT_TRUE(nvhttp::test_support::is_terra_api_path("/eclipse/v1/capabilities"));
  EXPECT_FALSE(nvhttp::test_support::is_terra_api_path("/eclipse/v1evil"));
  EXPECT_FALSE(nvhttp::test_support::is_terra_api_path("/serverinfo"));
}

TEST(TerraApiTest, TranslatesHidKeyboardInput) {
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x04), 'A');
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x3A), 0x70);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x46), 0x2C);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x49), 0x2D);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x4B), 0x21);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x4F), 0x27);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x54), 0x6F);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x58), 0x0D);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x59), 0x61);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x62), 0x60);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x63), 0x6E);
  EXPECT_EQ(nvhttp::test_support::hid_usage_to_virtual_key(0x00), 0);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0x00), 0x00);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0x01), 0x02);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0x22), 0x01);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0x44), 0x04);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0x88), 0x08);
  EXPECT_EQ(nvhttp::test_support::hid_keyboard_modifiers(0xFF), 0x0F);
}

TEST(TerraApiTest, BuildsCompleteCapabilityDocument) {
  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
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
  std::string advertised_capabilities;
  for (const auto &capability : document["capabilities"]) {
    if (!advertised_capabilities.empty()) {
      advertised_capabilities += ',';
    }
    advertised_capabilities += capability.get<std::string>();
  }
  EXPECT_EQ(advertised_capabilities, nvhttp::test_support::operational_capabilities_csv());
  EXPECT_TRUE(document["features"]["catalog-v2"]["available"]);
  EXPECT_FALSE(document["features"]["catalog-v2"].contains("reasonCode"));
  EXPECT_TRUE(document["features"]["events-v1"]["available"]);
  EXPECT_TRUE(document["features"]["profiles-v1"]["available"]);
  EXPECT_TRUE(document["features"]["workspaces-v1"]["available"]);
  EXPECT_EQ(document["features"]["multi-display-streaming-v1"]["available"], document["features"]["workspaces-v1"]["available"]);
  EXPECT_TRUE(document["features"]["telemetry-v1"]["available"]);
  EXPECT_FALSE(document["features"]["discovery-v1"]["available"]);
  EXPECT_EQ(document["features"]["discovery-v1"]["reasonCode"], "server_unavailable");
  EXPECT_EQ(document["features"]["discovery-v1"]["protocol"], "dns-sd");
  EXPECT_EQ(document["features"]["discovery-v1"]["protocolVersion"], 1);
  EXPECT_EQ(document["features"]["discovery-v1"]["serviceType"], "_nvstream._tcp.local");
  EXPECT_EQ(document["features"]["discovery-v1"]["verificationPath"], "/serverinfo");
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
  EXPECT_EQ(document["limits"]["sandboxes"]["maxActive"], document["features"]["sandboxes-v1"]["available"].get<bool>() ? nlohmann::json(nullptr) : nlohmann::json(0));
  EXPECT_EQ(
    document["limits"]["peripherals"]["maxDevices"],
    document["features"]["peripherals-v1"]["available"].get<bool>() ? 32 : 0
  );
#else
  EXPECT_GE(document["capabilities"].size(), terra_api::CAPABILITIES.size() + 1);
  EXPECT_FALSE(document["features"]["sandboxes-v1"]["available"]);
  EXPECT_FALSE(document["features"]["sandboxes-v1"]["reasonCode"].get<std::string>().empty());
#endif
  EXPECT_EQ(document["limits"]["sessions"]["maxActive"], 1);
  EXPECT_EQ(document["limits"]["streaming"]["maxDisplays"], 4);
  EXPECT_EQ(document["limits"]["displays"]["maxManaged"], 0);
  EXPECT_EQ(document["limits"]["virtualDisplays"]["maxActive"], 0);
  EXPECT_EQ(document["limits"]["workspaces"]["maxActive"], document["features"]["workspaces-v1"]["available"].get<bool>() ? 1 : 0);
  EXPECT_EQ(document["limits"]["sandboxes"]["maxActive"], document["features"]["sandboxes-v1"]["available"].get<bool>() ? nlohmann::json(nullptr) : nlohmann::json(0));
  const bool peripherals_available = document["features"]["peripherals-v1"]["available"].get<bool>();
  EXPECT_EQ(document["limits"]["peripherals"]["maxDevices"], peripherals_available ? 32 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxClaims"], peripherals_available ? 32 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxMessageBytes"], peripherals_available ? 65536 : 0);
  EXPECT_EQ(document["limits"]["peripherals"]["maxPayloadBytes"], peripherals_available ? 8 : 0);
  if (peripherals_available) {
    EXPECT_EQ(document["features"]["peripherals-v1"]["classes"], nlohmann::json::array({"keyboard", "mouse"}));
    EXPECT_EQ(document["features"]["peripherals-v1"]["capabilities"], nlohmann::json::array({"keyboard.hid", "mouse.hid"}));
    EXPECT_EQ(document["features"]["peripherals-v1"]["protocol"], "eclipse-peripheral-json");
  }
}

TEST(TerraApiTest, AdvertisesDiscoveryOnlyWhileOperational) {
  platf::publish::set_http_available(true);
  platf::publish::set_registration_state(platf::publish::state_t::available);

  const auto capabilities = nvhttp::test_support::operational_capabilities_csv();
  EXPECT_NE(capabilities.find("discovery-v1"), std::string::npos);

  terra_api::client_permissions_t permissions;
  const auto document = nvhttp::test_support::capabilities_document("22222222-2222-2222-2222-222222222222", "Terra client", permissions, "33333333-3333-3333-3333-333333333333", "Sol host", "linux", "1.0.0", false);
  EXPECT_TRUE(document["features"]["discovery-v1"]["available"]);
  EXPECT_FALSE(document["features"]["discovery-v1"].contains("reasonCode"));

  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
}

TEST(TerraApiTest, ReportsDiscoveryProviderFailureMetadata) {
  platf::publish::set_http_available(true);
  platf::publish::set_registration_state(platf::publish::state_t::unavailable, "registration_lost", "DNS-SD registration was lost");

  terra_api::client_permissions_t permissions;
  const auto document = nvhttp::test_support::capabilities_document("22222222-2222-2222-2222-222222222222", "Terra client", permissions, "33333333-3333-3333-3333-333333333333", "Sol host", "linux", "1.0.0", false);
  EXPECT_EQ(std::ranges::find(document["capabilities"], "discovery-v1"), document["capabilities"].end());
  EXPECT_FALSE(document["features"]["discovery-v1"]["available"]);
  EXPECT_EQ(document["features"]["discovery-v1"]["reasonCode"], "registration_lost");
  EXPECT_EQ(document["features"]["discovery-v1"]["reason"], "DNS-SD registration was lost");

  platf::publish::set_http_available(false);
  platf::publish::set_registration_state(platf::publish::state_t::stopped);
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
    0x00,
    0x01,
    0x00,
    0x00,
    0x00,
    0x01,
    0x08,
    0x06,
    0x00,
    0x00,
    0x00,
    0x1F,
    0x15,
    0xC4,
    0x89,
    0x00,
    0x00,
    0x00,
    0x0D,
    'I',
    'D',
    'A',
    'T',
    0x08,
    0xD7,
    0x63,
    0xF8,
    0xCF,
    0xC0,
    0xF0,
    0x1F,
    0x00,
    0x05,
    0x00,
    0x01,
    0xFF,
    0x72,
    0x9C,
    0x52,
    0x67,
    0x00,
    0x00,
    0x00,
    0x00,
    'I',
    'E',
    'N',
    'D',
    0xAE,
    0x42,
    0x60,
    0x82,
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
