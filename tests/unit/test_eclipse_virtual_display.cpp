/**
 * @file tests/unit/test_eclipse_virtual_display.cpp
 * @brief Tests for Eclipse virtual display lifecycle management.
 */

// standard includes
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/eclipse_virtual_display.h>

namespace {
  using namespace eclipse_virtual_display;
  constexpr const char *OWNER = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";  ///< Test owner UUID.
  constexpr const char *RESOURCE = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";  ///< First resource UUID.
  constexpr const char *RESOURCE_2 = "cccccccc-cccc-cccc-cccc-cccccccccccc";  ///< Second resource UUID.
  constexpr const char *SESSION = "dddddddd-dddd-dddd-dddd-dddddddddddd";  ///< Test session UUID.

  /**
   * @brief Deterministic in-memory provider and persistence implementation.
   */
  struct fake_t {
    std::uint32_t count = 2;  ///< Current global connector count.
    std::vector<std::string> ids {"base-0", "base-1"};  ///< Current provider inventory.
    std::optional<std::string> document;  ///< Persisted manager document.
    std::vector<std::string> uuids {RESOURCE, RESOURCE_2};  ///< Generated resource IDs.
    std::size_t uuid_index = 0;  ///< Next generated UUID index.
    bool healthy = true;  ///< Provider health result.
    bool set_succeeds = true;  ///< Count setter result.
    bool apply_succeeds = true;  ///< Configuration apply result.
    bool save_succeeds = true;  ///< Persistence result.
    bool ambiguous_growth = false;  ///< Add two IDs while count grows once.
    int apply_calls = 0;  ///< Configuration apply call count.
    int set_calls = 0;  ///< Count setter call count.
    int fail_set_on_call = 0;  ///< One-based count setter call that fails, or zero.

    /**
     * @brief Set count using highest-connector removal semantics.
     *
     * @param value Desired global count.
     * @return Configured success result.
     */
    bool set_count(const std::uint32_t value) {
      ++set_calls;
      if (!set_succeeds || set_calls == fail_set_on_call) {
        return false;
      }
      while (ids.size() < value) {
        ids.push_back("managed-" + std::to_string(ids.size()));
      }
      while (ids.size() > value) {
        ids.pop_back();
      }
      if (ambiguous_growth && value > count) {
        ids.push_back("ambiguous");
      }
      count = value;
      return true;
    }

    /**
     * @brief Apply configurations and synthesize stable actual modes.
     *
     * @param configurations Mutable provider configurations.
     * @return Configured success result.
     */
    bool apply(std::vector<platform_configuration_t> &configurations) {
      ++apply_calls;
      if (!apply_succeeds) {
        return false;
      }
      for (auto &configuration : configurations) {
        const auto &mode = configuration.specification.mode;
        configuration.actual_mode = {mode.width, mode.height, mode.refresh_numerator, mode.refresh_denominator, mode.bit_depth, mode.hdr, configuration.platform_id + ":mode"};
      }
      return true;
    }

    /**
     * @brief Build callbacks bound to fixture.
     *
     * @return Manager callbacks.
     */
    callbacks_t callbacks() {
      return {
        [&]() {
          return document;
        },
        [&](const std::string &value) {
          if (save_succeeds) {
            document = value;
          }
          return save_succeeds;
        },
        []() {
          return 1000;
        },
        [&]() {
          return uuids.at(uuid_index++);
        },
        [&]() {
          return healthy;
        },
        [&]() -> std::optional<std::uint32_t> {
          return count;
        },
        [&](const std::uint32_t value) {
          return set_count(value);
        },
        [&]() -> std::optional<std::vector<std::string>> {
          return ids;
        },
        [&](std::vector<platform_configuration_t> &values) {
          return apply(values);
        },
      };
    }
  };

  /**
   * @brief Build valid standard display specification.
   *
   * @param persistent Whether resource survives restart.
   * @return Valid display specification.
   */
  specification_t specification(const bool persistent = true) {
    return {"Virtual", {1920, 1080, 60000, 1000, 10, true}, {10, 20}, 1.25, 0, false, true, persistent, std::nullopt};
  }
}  // namespace

TEST(EclipseVirtualDisplayTest, PreservesBaselineAndCreatesStableResource) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  const auto result = manager.create(OWNER, specification());
  ASSERT_EQ(result.status, status_t::success);
  EXPECT_EQ(fake.count, 3);
  EXPECT_EQ(fake.ids[0], "base-0");
  EXPECT_EQ(result.resource->id, RESOURCE);
  EXPECT_EQ(result.resource->platform_id, "managed-2");
  EXPECT_EQ(result.resource->actual_mode.id, "managed-2:mode");
  EXPECT_FALSE(to_json(*result.resource).contains("platformId"));
  const auto listed = manager.list(0);
  EXPECT_TRUE(listed.changed);
  EXPECT_EQ(listed.resources.size(), 1);
  EXPECT_FALSE(manager.list(listed.revision).changed);
}

TEST(EclipseVirtualDisplayTest, CountDriftFailsClosedWithoutChangingState) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ++fake.count;
  fake.ids.push_back("external");
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::provider_error);
  EXPECT_TRUE(manager.list().resources.empty());
}

TEST(EclipseVirtualDisplayTest, AmbiguousInventoryDiffRollsBackCount) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  fake.ambiguous_growth = true;
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::provider_error);
  EXPECT_EQ(fake.count, 2);
  EXPECT_EQ(fake.ids.size(), 2);
}

TEST(EclipseVirtualDisplayTest, ApplyAndPersistenceFailuresRollbackProviderAndPublication) {
  fake_t apply_fake;
  manager_t apply_manager {apply_fake.callbacks()};
  apply_fake.apply_succeeds = false;
  EXPECT_EQ(apply_manager.create(OWNER, specification()).status, status_t::provider_error);
  EXPECT_EQ(apply_fake.count, 2);
  EXPECT_FALSE(apply_manager.get(RESOURCE));

  fake_t save_fake;
  manager_t save_manager {save_fake.callbacks()};
  save_fake.save_succeeds = false;
  EXPECT_EQ(save_manager.create(OWNER, specification()).status, status_t::persistence_error);
  EXPECT_EQ(save_fake.count, 2);
  EXPECT_FALSE(save_manager.get(RESOURCE));
}

TEST(EclipseVirtualDisplayTest, ValidatesInputAndRevisionPreconditions) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto invalid = specification();
  invalid.scale = 0;
  EXPECT_EQ(manager.create(OWNER, invalid).status, status_t::invalid);
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  patch_t patch;
  patch.name = "Changed";
  EXPECT_EQ(manager.patch(RESOURCE, 99, patch).status, status_t::conflict);
  const auto changed = manager.patch(RESOURCE, 1, patch);
  ASSERT_EQ(changed.status, status_t::success);
  EXPECT_EQ(changed.resource->revision, 2);
  EXPECT_EQ(changed.resource->name, "Changed");
}

TEST(EclipseVirtualDisplayTest, AttachmentBlocksDeleteUntilDetached) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  const auto attached = manager.attach(RESOURCE, 1, {.session_id = SESSION});
  ASSERT_EQ(attached.status, status_t::success);
  EXPECT_EQ(manager.remove(RESOURCE, 2).status, status_t::conflict);
  const auto detached = manager.detach(RESOURCE, 2);
  ASSERT_EQ(detached.status, status_t::success);
  EXPECT_EQ(manager.remove(RESOURCE, 3).status, status_t::success);
  EXPECT_EQ(fake.count, 2);
  EXPECT_EQ(fake.ids, (std::vector<std::string> {"base-0", "base-1"}));
}

TEST(EclipseVirtualDisplayTest, DeleteRemapsSurvivorWhenHighestConnectorDisappears) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  const auto removed = manager.remove(RESOURCE, 1);
  ASSERT_EQ(removed.status, status_t::success);
  const auto survivor = manager.get(RESOURCE_2);
  ASSERT_TRUE(survivor);
  EXPECT_EQ(survivor->platform_id, "managed-2");
  EXPECT_EQ(survivor->revision, 2);
}

TEST(EclipseVirtualDisplayTest, RevocationOrphansAndAdoptionClaimsResource) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  ASSERT_EQ(manager.revoke_owner(OWNER), status_t::success);
  const auto orphan = manager.get(RESOURCE);
  ASSERT_TRUE(orphan);
  EXPECT_FALSE(orphan->owner_client_uuid);
  const auto adopted = manager.adopt(RESOURCE, 2, OWNER);
  ASSERT_EQ(adopted.status, status_t::success);
  EXPECT_EQ(adopted.resource->owner_client_uuid, OWNER);
  EXPECT_EQ(adopted.resource->revision, 3);
}

TEST(EclipseVirtualDisplayTest, RevocationDeletesEphemeralAndRemapsPersistentResource) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification(false)).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, specification(true)).status, status_t::success);
  ASSERT_EQ(manager.revoke_owner(OWNER), status_t::success);
  EXPECT_FALSE(manager.get(RESOURCE));
  const auto persistent = manager.get(RESOURCE_2);
  ASSERT_TRUE(persistent);
  EXPECT_FALSE(persistent->owner_client_uuid);
  EXPECT_EQ(persistent->platform_id, "managed-2");
  EXPECT_EQ(persistent->revision, 3);
  EXPECT_EQ(fake.count, 3);
}

TEST(EclipseVirtualDisplayTest, AttachmentRequiresExactlyOneValidTarget) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  EXPECT_EQ(manager.attach(RESOURCE, 1, {}).status, status_t::invalid);
  EXPECT_EQ(manager.attach(RESOURCE, 1, {.session_id = SESSION, .workspace_id = OWNER}).status, status_t::invalid);
  const auto attached = manager.attach(RESOURCE, 1, {.workspace_id = OWNER});
  ASSERT_EQ(attached.status, status_t::success);
  EXPECT_EQ(attached.resource->workspace_id, OWNER);
  EXPECT_FALSE(attached.resource->session_id);
}

TEST(EclipseVirtualDisplayTest, PatchRejectsProviderCountDrift) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  ++fake.count;
  fake.ids.push_back("external");
  patch_t patch;
  patch.name = "Changed";
  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::provider_error);
  EXPECT_EQ(manager.get(RESOURCE)->name, "Virtual");
}

TEST(EclipseVirtualDisplayTest, RestartRetainsPersistentAndRemovesEphemeralResources) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, specification(false)).status, status_t::success);
    ASSERT_EQ(manager.create(OWNER, specification(true)).status, status_t::success);
  }
  fake.uuid_index = 0;
  manager_t restarted {fake.callbacks()};
  ASSERT_TRUE(restarted.available());
  EXPECT_FALSE(restarted.get(RESOURCE));
  const auto persistent = restarted.get(RESOURCE_2);
  ASSERT_TRUE(persistent);
  EXPECT_EQ(persistent->platform_id, "managed-2");
  EXPECT_EQ(fake.count, 3);
}

TEST(EclipseVirtualDisplayTest, RestartDetachesPersistentRuntimeAttachment) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, specification(true)).status, status_t::success);
    ASSERT_EQ(manager.attach(RESOURCE, 1, {.session_id = SESSION}).status, status_t::success);
  }

  fake.uuid_index = 0;
  manager_t restarted {fake.callbacks()};
  ASSERT_TRUE(restarted.available());
  const auto resource = restarted.get(RESOURCE);
  ASSERT_TRUE(resource);
  EXPECT_EQ(resource->state, state_t::ready);
  EXPECT_FALSE(resource->session_id);
  EXPECT_FALSE(resource->workspace_id);
  EXPECT_EQ(resource->revision, 3);
}

TEST(EclipseVirtualDisplayTest, FailedConfigurationRollbackDisablesManager) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  fake.apply_succeeds = false;
  patch_t patch;
  patch.name = "Changed";

  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::provider_error);
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::unavailable);
}

TEST(EclipseVirtualDisplayTest, FailedCountRollbackDisablesManager) {
  fake_t fake;
  fake.ambiguous_growth = true;
  fake.fail_set_on_call = 2;
  manager_t manager {fake.callbacks()};

  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::provider_error);
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::unavailable);
}

TEST(EclipseVirtualDisplayTest, MalformedPersistenceFailsClosed) {
  fake_t fake;
  fake.document = R"({"version":1,"baselineCount":2,"baselineInventory":["base-0","base-1"],"collectionRevision":0,"resources":[{"id":"bad"}]})";
  manager_t manager {fake.callbacks()};
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::unavailable);
  EXPECT_TRUE(manager.list().resources.empty());
}
