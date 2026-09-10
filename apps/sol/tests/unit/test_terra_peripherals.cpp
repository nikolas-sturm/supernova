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
    EXPECT_EQ(manager.get_device("owner-1", device.resource->id)->first.revision, 2);
    EXPECT_EQ(terra_peripherals::active_claim_count(manager.list_claims("owner-1")), 1);

    // Same owner may not double-claim exclusively.
    EXPECT_EQ(manager.create_claim("owner-1", claim_request).status, terra_peripherals::status_t::conflict);

    // Foreign owner cannot see or claim the device.
    EXPECT_TRUE(manager.list_devices("owner-2").empty());
    EXPECT_EQ(manager.create_claim("owner-2", claim_request).status, terra_peripherals::status_t::not_found);
    EXPECT_FALSE(manager.get_claim("owner-2", claim.resource->id));
    EXPECT_EQ(terra_peripherals::active_claim_count(manager.list_claims("owner-2")), 0);

    // Wrong credential fails authentication.
    EXPECT_FALSE(manager.authenticate_claim(claim.resource->id, "bogus", "owner-1"));
    EXPECT_FALSE(manager.authenticate_claim(claim.resource->id, claim.resource->credential, "owner-2"));
    EXPECT_TRUE(manager.authenticate_claim(claim.resource->id, claim.resource->credential, "owner-1"));

    const auto opened = manager.open_channel(claim.resource->id);
    ASSERT_EQ(opened.status, terra_peripherals::status_t::success);
    EXPECT_EQ(opened.resource->state, terra_peripherals::claim_state_t::active);
    EXPECT_EQ(manager.get_device("owner-1", device.resource->id)->first.revision, 3);

    // Device reports the active claim.
    const auto listed = manager.get_device("owner-1", device.resource->id);
    ASSERT_TRUE(listed);
    ASSERT_TRUE(listed->second);
    EXPECT_EQ(*listed->second, claim.resource->id);

    // Closing with release policy terminates the claim.
    const auto closed = manager.close_channel(claim.resource->id);
    ASSERT_TRUE(closed);
    EXPECT_EQ(closed->state, terra_peripherals::claim_state_t::released);
    EXPECT_EQ(manager.get_device("owner-1", device.resource->id)->first.revision, 4);
    EXPECT_EQ(terra_peripherals::active_claim_count(manager.list_claims("owner-1")), 0);

    // Release is idempotent.
    EXPECT_EQ(manager.release_claim("owner-1", claim.resource->id, closed->revision - 1).status, terra_peripherals::status_t::conflict);
    const auto released = manager.release_claim("owner-1", claim.resource->id, closed->revision);
    ASSERT_EQ(released.status, terra_peripherals::status_t::success);
    const auto released_again = manager.release_claim("owner-1", claim.resource->id, released.resource->revision);
    ASSERT_EQ(released_again.status, terra_peripherals::status_t::success);
    EXPECT_EQ(released.resource->state, released_again.resource->state);
  }

  TEST(TerraPeripheralsTest, ExclusiveClaimsBlockConflictingSharedClaims) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);

    auto exclusive_request = session_claim(device.resource->id);
    exclusive_request.exclusive = true;
    exclusive_request.disconnect_policy = "suspend";
    const auto exclusive_claim = manager.create_claim("owner-1", exclusive_request);
    ASSERT_EQ(exclusive_claim.status, terra_peripherals::status_t::success);

    const auto shared_request = session_claim(device.resource->id);
    EXPECT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::conflict);

    ASSERT_EQ(manager.open_channel(exclusive_claim.resource->id).status, terra_peripherals::status_t::success);
    EXPECT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::conflict);

    ASSERT_TRUE(manager.close_channel(exclusive_claim.resource->id));
    EXPECT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::conflict);

    ASSERT_EQ(manager.release_claim("owner-1", exclusive_claim.resource->id, 3).status, terra_peripherals::status_t::success);
    EXPECT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::success);
  }

  TEST(TerraPeripheralsTest, NewExclusiveClaimBlocksExistingSharedClaims) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);

    const auto shared_request = session_claim(device.resource->id);
    ASSERT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::success);
    ASSERT_EQ(manager.create_claim("owner-1", shared_request).status, terra_peripherals::status_t::success);

    auto exclusive_request = shared_request;
    exclusive_request.exclusive = true;
    EXPECT_EQ(manager.create_claim("owner-1", exclusive_request).status, terra_peripherals::status_t::conflict);
  }

  TEST(TerraPeripheralsTest, EnforcesPublishedDeviceAndClaimLimits) {
    auto manager = make_manager();
    for (std::size_t index = 0; index < terra_peripherals::MAX_DEVICES; ++index) {
      ASSERT_EQ(manager.create_device("owner-1", keyboard_device()).status, terra_peripherals::status_t::success);
    }
    EXPECT_EQ(manager.create_device("owner-1", keyboard_device()).status, terra_peripherals::status_t::limit_reached);
    auto unsupported = keyboard_device();
    unsupported.capabilities = {"keyboard.text"};
    EXPECT_EQ(manager.create_device("owner-1", unsupported).status, terra_peripherals::status_t::unsupported_configuration);

    const auto device = manager.list_devices("owner-1").front().first;
    for (std::size_t index = 0; index < terra_peripherals::MAX_CLAIMS; ++index) {
      ASSERT_EQ(manager.create_claim("owner-1", session_claim(device.id)).status, terra_peripherals::status_t::success);
    }
    EXPECT_EQ(manager.create_claim("owner-1", session_claim(device.id)).status, terra_peripherals::status_t::limit_reached);
    EXPECT_EQ(manager.create_claim("owner-2", session_claim(device.id)).status, terra_peripherals::status_t::not_found);

    const auto released = manager.list_claims("owner-1").front();
    ASSERT_EQ(manager.release_claim("owner-1", released.id, released.revision).status, terra_peripherals::status_t::success);
    EXPECT_EQ(manager.create_claim("owner-1", session_claim(device.id)).status, terra_peripherals::status_t::success);
  }

  TEST(TerraPeripheralsTest, ReturnsExpiringCredentialOnlyOnCreation) {
    std::int64_t now = 1000;
    terra_peripherals::manager_t manager {[&now]() {
                                            return now;
                                          },
                                          test_uuid,
                                          test_token};
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);
    const auto claim = manager.create_claim("owner-1", session_claim(device.resource->id));
    ASSERT_EQ(claim.status, terra_peripherals::status_t::success);
    ASSERT_TRUE(claim.resource);
    EXPECT_EQ(claim.resource->credential_expires_at, now + terra_peripherals::CLAIM_CREDENTIAL_TTL_MS);

    const auto regular = terra_peripherals::claim_json(*claim.resource);
    EXPECT_TRUE(regular.at("channel").is_null());
    EXPECT_FALSE(regular.dump().contains(claim.resource->credential));

    const auto created = terra_peripherals::claim_creation_json(*claim.resource, "/claim/channel");
    EXPECT_EQ(created.at("channel").at("endpoint"), "/claim/channel");
    EXPECT_EQ(created.at("channel").at("protocol"), "eclipse-peripheral-json");
    EXPECT_EQ(created.at("channel").at("protocolVersion"), 1);
    EXPECT_EQ(created.at("channel").at("credential"), claim.resource->credential);
    EXPECT_EQ(created.at("channel").at("expiresAt"), claim.resource->credential_expires_at);

    EXPECT_TRUE(manager.authenticate_claim(claim.resource->id, claim.resource->credential, "owner-1"));
    now = claim.resource->credential_expires_at;
    EXPECT_FALSE(manager.authenticate_claim(claim.resource->id, claim.resource->credential, "owner-1"));
  }

  TEST(TerraPeripheralsTest, RejectsUnsupportedDescriptorAndUnimplementedCapabilities) {
    auto manager = make_manager();
    auto descriptor = keyboard_device();
    descriptor.report_descriptor_base64 = "AA==";
    EXPECT_EQ(manager.create_device("owner-1", descriptor).status, terra_peripherals::status_t::unsupported_configuration);

    auto text = keyboard_device();
    text.capabilities = {"keyboard.text"};
    EXPECT_EQ(manager.create_device("owner-1", text).status, terra_peripherals::status_t::unsupported_configuration);
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

    const auto suspended = manager.target_ended("session", suspend_request.target.id, false);
    ASSERT_EQ(suspended.size(), 1);
    EXPECT_EQ(manager.get_claim("owner-1", suspend_claim.resource->id)->state, terra_peripherals::claim_state_t::suspended);
    EXPECT_TRUE(manager.target_ended("session", suspend_request.target.id, false).empty());

    auto release_request = session_claim(device.resource->id);
    const auto release_claim = manager.create_claim("owner-1", release_request);
    ASSERT_EQ(release_claim.status, terra_peripherals::status_t::success);
    static_cast<void>(manager.open_channel(release_claim.resource->id));

    const auto target_released = manager.target_ended("session", release_request.target.id, true);
    ASSERT_EQ(target_released.size(), 2);
    EXPECT_EQ(manager.get_claim("owner-1", release_claim.resource->id)->state, terra_peripherals::claim_state_t::released);
  }

  TEST(TerraPeripheralsTest, DeleteDeviceReleasesClaimsAndRevocationClearsOwner) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);
    const auto claim = manager.create_claim("owner-1", session_claim(device.resource->id));
    ASSERT_EQ(claim.status, terra_peripherals::status_t::success);

    std::vector<terra_peripherals::claim_t> released_claims;
    const auto current_device = manager.get_device("owner-1", device.resource->id);
    ASSERT_TRUE(current_device);
    EXPECT_EQ(manager.delete_device("owner-1", device.resource->id, current_device->first.revision + 1, released_claims).status, terra_peripherals::status_t::conflict);
    const auto removed = manager.delete_device("owner-1", device.resource->id, current_device->first.revision, released_claims);
    ASSERT_EQ(removed.status, terra_peripherals::status_t::success);
    ASSERT_EQ(released_claims.size(), 1);
    EXPECT_EQ(released_claims.front().id, claim.resource->id);
    EXPECT_EQ(released_claims.front().state, terra_peripherals::claim_state_t::released);
    EXPECT_TRUE(manager.list_devices("owner-1").empty());
    EXPECT_EQ(manager.delete_device("owner-1", device.resource->id, device.resource->revision, released_claims).status, terra_peripherals::status_t::not_found);

    const auto second_device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(second_device.status, terra_peripherals::status_t::success);
    const auto second_claim = manager.create_claim("owner-1", session_claim(second_device.resource->id));
    ASSERT_EQ(second_claim.status, terra_peripherals::status_t::success);
    const auto revoked = manager.revoke_owner("owner-1");
    ASSERT_EQ(revoked.claims.size(), 1);
    ASSERT_EQ(revoked.devices.size(), 1);
    EXPECT_EQ(revoked.devices.front().id, second_device.resource->id);
    EXPECT_TRUE(manager.list_devices("owner-1").empty());
    EXPECT_EQ(manager.get_claim("owner-1", second_claim.resource->id)->state, terra_peripherals::claim_state_t::released);

    const auto serialized = terra_peripherals::claim_json(*claim.resource);
    EXPECT_TRUE(serialized.at("channel").is_null());
    EXPECT_EQ(serialized.at("state"), "pending");
  }

  TEST(TerraPeripheralsTest, RevocationCanSelectDeviceClass) {
    auto manager = make_manager();
    const auto keyboard = manager.create_device("owner-1", keyboard_device());
    auto mouse_request = keyboard_device();
    mouse_request.device_class = "mouse";
    mouse_request.platform_id = "hid-mouse-1";
    mouse_request.name = "Terra Mouse";
    mouse_request.capabilities = {"mouse.hid"};
    const auto mouse = manager.create_device("owner-1", mouse_request);
    ASSERT_EQ(keyboard.status, terra_peripherals::status_t::success);
    ASSERT_EQ(mouse.status, terra_peripherals::status_t::success);

    const auto revoked = manager.revoke_owner("owner-1", {"keyboard"});
    ASSERT_EQ(revoked.devices.size(), 1);
    EXPECT_EQ(revoked.devices.front().id, keyboard.resource->id);
    const auto remaining = manager.list_devices("owner-1");
    ASSERT_EQ(remaining.size(), 1);
    EXPECT_EQ(remaining.front().first.id, mouse.resource->id);
  }

  TEST(TerraPeripheralsTest, ReleasesInactiveClaimsWhenCredentialsExpire) {
    std::int64_t now = 1'000;
    terra_peripherals::manager_t manager {[&now]() {
                                            return now;
                                          },
                                          test_uuid,
                                          test_token};
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);
    const auto claim = manager.create_claim("owner-1", session_claim(device.resource->id));
    ASSERT_EQ(claim.status, terra_peripherals::status_t::success);

    now += terra_peripherals::CLAIM_CREDENTIAL_TTL_MS;
    auto replacement = session_claim(device.resource->id);
    replacement.exclusive = true;
    const auto replaced = manager.create_claim("owner-1", replacement);
    ASSERT_EQ(replaced.status, terra_peripherals::status_t::success);
    ASSERT_EQ(replaced.expired_claims.size(), 1);
    EXPECT_EQ(replaced.expired_claims.front().id, claim.resource->id);
    EXPECT_EQ(replaced.expired_claims.front().state, terra_peripherals::claim_state_t::released);

    now += terra_peripherals::CLAIM_CREDENTIAL_TTL_MS;
    const auto swept = manager.expire_credentials();
    ASSERT_EQ(swept.size(), 1);
    EXPECT_EQ(swept.front().id, replaced.resource->id);
    EXPECT_TRUE(manager.expire_credentials().empty());
  }

  TEST(TerraPeripheralsTest, BoundsReleasedClaimHistory) {
    auto manager = make_manager();
    const auto device = manager.create_device("owner-1", keyboard_device());
    ASSERT_EQ(device.status, terra_peripherals::status_t::success);

    for (std::size_t index = 0; index < terra_peripherals::MAX_RETAINED_CLAIMS + 32; ++index) {
      const auto claim = manager.create_claim("owner-1", session_claim(device.resource->id));
      ASSERT_EQ(claim.status, terra_peripherals::status_t::success);
      ASSERT_EQ(manager.release_claim("owner-1", claim.resource->id, claim.resource->revision).status, terra_peripherals::status_t::success);
    }
    EXPECT_EQ(manager.list_claims("owner-1").size(), terra_peripherals::MAX_RETAINED_CLAIMS);
  }
}  // namespace
