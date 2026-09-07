/**
 * @file tests/unit/test_terra_peripherals.cpp
 * @brief Unit tests for Terra peripherals-v1 device registry and claim manager.
 */
// standard includes
#include <chrono>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

// local includes
#include <src/terra_peripherals.h>

namespace {
  /**
   * @brief Millisecond clock matching production semantics.
   * @return Current Unix time in milliseconds.
   */
  std::int64_t test_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  /**
   * @brief Monotonic UUID factory for deterministic tests.
   * @return Canonical UUID-shaped string.
   */
  std::string test_uuid() {
    static unsigned counter = 0;
    ++counter;
    return std::format("00000000-0000-4000-8000-{:012}", counter);
  }

  /**
   * @brief Monotonic credential factory for deterministic tests.
   * @return Opaque token string.
   */
  std::string test_token() {
    static unsigned counter = 0;
    ++counter;
    return std::format("token-{}", counter);
  }

  /**
   * @brief Construct a manager with deterministic factories.
   * @return Ready manager instance.
   */
  terra_peripherals::manager_t make_manager() {
    return terra_peripherals::manager_t(test_now, test_uuid, test_token);
  }

  /**
   * @brief Valid keyboard device registration.
   * @return Creation fields.
   */
  terra_peripherals::create_device_t keyboard_device() {
    return {
      .device_class = "keyboard",
      .platform_id = "hid-keyboard-1",
      .name = "Terra Keyboard",
      .vendor_id = 0x1234,
      .product_id = 0x5678,
      .serial = std::nullopt,
      .capabilities = {"keyboard.hid"},
      .report_descriptor_base64 = std::nullopt,
    };
  }

  /**
   * @brief Claim creation against the first session target.
   * @param device_id Registered device UUID.
   * @return Creation fields.
   */
  terra_peripherals::create_claim_t session_claim(const std::string &device_id) {
    return {
      .device_id = device_id,
      .target = {"session", "11111111-1111-4111-8111-111111111111"},
      .requested_capabilities = {"keyboard.hid"},
      .exclusive = false,
      .disconnect_policy = "release",
    };
  }

  TEST(TerraPeripheralsTest, RejectsUnsupportedClassesAndCapabilities) {
    auto manager = make_manager();
    auto unsupported_class = keyboard_device();
    unsupported_class.device_class = "usb";
    EXPECT_EQ(manager.create_device("owner-1", unsupported_class).status, terra_peripherals::status_t::unsupported_configuration);

    auto unsupported_capability = keyboard_device();
    unsupported_capability.capabilities = {"usb.bulk"};
    EXPECT_EQ(manager.create_device("owner-1", unsupported_capability).status, terra_peripherals::status_t::unsupported_configuration);

    auto mismatched_capability = keyboard_device();
    mismatched_capability.capabilities = {"mouse.hid"};
    EXPECT_EQ(manager.create_device("owner-1", mismatched_capability).status, terra_peripherals::status_t::unsupported_configuration);

    auto duplicate_capabilities = keyboard_device();
    duplicate_capabilities.capabilities = {"keyboard.hid", "keyboard.hid"};
    EXPECT_EQ(manager.create_device("owner-1", duplicate_capabilities).status, terra_peripherals::status_t::unsupported_configuration);
  }

  TEST(TerraPeripheralsTest, RegistersAndListsOwnedDevices) {
    auto manager = make_manager();
    const auto created = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(created.status, terra_peripherals::status_t::success);
    ASSERT_TRUE(created.resource);

    const auto devices = manager.list_devices("owner-1");
    ASSERT_EQ(devices.size(), 1);
    EXPECT_EQ(devices[0].first.id, created.resource->id);
    EXPECT_FALSE(devices[0].second);

    EXPECT_TRUE(manager.list_devices("owner-2").empty());

    const auto serialized = terra_peripherals::device_json(*created.resource);
    EXPECT_EQ(serialized.at("class"), "keyboard");
    EXPECT_TRUE(serialized.at("claimable"));
    EXPECT_TRUE(serialized.at("activeClaimId").is_null());
  }

  TEST(TerraPeripheralsTest, ClaimLifecycleEnforcesVisibilityAndExclusivity) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);

    auto claim_request = session_claim(device.resource->id);
    claim_request.exclusive = true;
    const auto claim = manager.create_claim("owner-1", claim_request);
    ASSERT_EQ(claim.status, terra_peripherals::status_t::success);
    ASSERT_TRUE(claim.resource);
    EXPECT_EQ(claim.resource->state, terra_peripherals::claim_state_t::pending);
    EXPECT_EQ(claim.resource->device_class, "keyboard");

    // Same owner may not double-claim exclusively.
    EXPECT_EQ(manager.create_claim("owner-1", claim_request).status, terra_peripherals::status_t::conflict);

    // Foreign owner cannot see or claim the device.
    EXPECT_TRUE(manager.list_devices("owner-2").empty());
    EXPECT_EQ(manager.create_claim("owner-2", claim_request).status, terra_peripherals::status_t::not_found);
    EXPECT_FALSE(manager.get_claim("owner-2", claim.resource->id));

    // Wrong credential fails authentication.
    EXPECT_FALSE(manager.authenticate_claim(claim.resource->id, "bogus"));
    EXPECT_TRUE(manager.authenticate_claim(claim.resource->id, claim.resource->credential));

    const auto opened = manager.open_channel(claim.resource->id);
    ASSERT_EQ(opened.status, terra_peripherals::status_t::success);
    EXPECT_EQ(opened.resource->state, terra_peripherals::claim_state_t::active);

    // Device reports the active claim.
    const auto listed = manager.get_device("owner-1", device.resource->id);
    ASSERT_TRUE(listed);
    ASSERT_TRUE(listed->second);
    EXPECT_EQ(*listed->second, claim.resource->id);

    // Closing with release policy terminates the claim.
    const auto closed = manager.close_channel(claim.resource->id);
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->state, terra_peripherals::claim_state_t::released);

    // Release is idempotent.
    const auto released = manager.release_claim("owner-1", claim.resource->id);
    ASSERT_EQ(released.status, terra_peripherals::status_t::success);
    const auto released_again = manager.release_claim("owner-1", claim.resource->id);
    ASSERT_EQ(released_again.status, terra_peripherals::status_t::success);
    EXPECT_EQ(released.resource->state, released_again.resource->state);
  }

  TEST(TerraPeripheralsTest, TargetEndHonorsDisconnectPolicy) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);

    auto suspend_request = session_claim(device.resource->id);
    suspend_request.disconnect_policy = "suspend";
    const auto suspend_claim = manager.create_claim("owner-1", suspend_request);
    ASSERT_EQ(suspend_claim.status, terra_peripherals::status_t::success);
    static_cast<void>(manager.open_channel(suspend_claim.resource->id));

    manager.target_ended("session", suspend_request.target.id, false);
    EXPECT_EQ(manager.get_claim("owner-1", suspend_claim.resource->id)->state, terra_peripherals::claim_state_t::suspended);

    auto release_request = session_claim(device.resource->id);
    const auto release_claim = manager.create_claim("owner-1", release_request);
    ASSERT_EQ(release_claim.status, terra_peripherals::status_t::success);
    static_cast<void>(manager.open_channel(release_claim.resource->id));

    manager.target_ended("session", release_request.target.id, true);
    EXPECT_EQ(manager.get_claim("owner-1", release_claim.resource->id)->state, terra_peripherals::claim_state_t::released);
  }

  TEST(TerraPeripheralsTest, DeleteDeviceReleasesClaimsAndRevocationClearsOwner) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);
    const auto claim = manager.create_claim("owner-1", session_claim(device.resource->id));
    ASSERT_EQ(claim.status, terra_peripherals::status_t::success);

    manager.revoke_owner("owner-1");
    EXPECT_EQ(manager.get_claim("owner-1", claim.resource->id)->state, terra_peripherals::claim_state_t::released);

    const auto removed = manager.delete_device("owner-1", device.resource->id);
    ASSERT_EQ(removed.status, terra_peripherals::status_t::success);
    EXPECT_TRUE(manager.list_devices("owner-1").empty());
    EXPECT_EQ(manager.delete_device("owner-1", device.resource->id).status, terra_peripherals::status_t::not_found);

    const auto serialized = terra_peripherals::claim_json(*claim.resource);
    EXPECT_TRUE(serialized.at("channel").is_null());
    EXPECT_EQ(serialized.at("state"), "pending");
  }
}  // namespace
