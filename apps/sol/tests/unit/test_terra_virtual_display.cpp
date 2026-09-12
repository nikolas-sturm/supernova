/**
 * @file tests/unit/test_terra_virtual_display.cpp
 * @brief Tests for Terra virtual display lifecycle management.
 */

// standard includes
#include <algorithm>
#include <cstdint>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/terra_virtual_display.h>

namespace {
  using namespace terra_virtual_display;
  constexpr const char *OWNER = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";  ///< Test owner UUID.
  constexpr const char *RESOURCE = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";  ///< First resource UUID.
  constexpr const char *RESOURCE_2 = "cccccccc-cccc-cccc-cccc-cccccccccccc";  ///< Second resource UUID.
  constexpr const char *RESOURCE_3 = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";  ///< Third resource UUID.
  constexpr const char *SESSION = "dddddddd-dddd-dddd-dddd-dddddddddddd";  ///< Test session UUID.

  /**
   * @brief Deterministic in-memory provider and persistence implementation.
   */
  struct fake_t {
    std::vector<connector_t> connectors;  ///< Present provider connectors.
    std::optional<std::string> document;  ///< Persisted manager document.
    std::vector<std::string> uuids {RESOURCE, RESOURCE_2, RESOURCE_3};  ///< Generated resource IDs.
    std::size_t uuid_index = 0;  ///< Next generated UUID index.
    bool healthy = true;  ///< Provider health result.
    bool apply_succeeds = true;  ///< Configuration apply result.
    bool capture_succeeds = true;  ///< Host display rollback capture result.
    bool restore_succeeds = true;  ///< Host display rollback restore result.
    bool save_succeeds = true;  ///< Persistence result.
    bool normalize_existing_position = false;  ///< Adjust an existing display position on later applies.
    std::uint32_t max_count = 16;  ///< Provider connector capacity.
    int apply_calls = 0;  ///< Configuration apply call count.
    int capture_calls = 0;  ///< Host display rollback snapshot count.
    int restore_calls = 0;  ///< Host display rollback restore count.
    int fail_apply_on_call = 0;  ///< One-based apply call that fails, or zero.
    std::vector<std::pair<std::optional<resource_t>, std::optional<resource_t>>> changes;  ///< Committed resource transitions.

    /**
     * @brief Read present provider connectors.
     *
     * @return Present connectors by slot.
     */
    std::optional<std::vector<connector_t>> inventory() const {
      return connectors;
    }

    /**
     * @brief Apply complete topology and synthesize stable actual modes.
     *
     * @param configurations Mutable provider configurations.
     * @return Configured success result.
     */
    bool apply(std::vector<platform_configuration_t> &configurations) {
      ++apply_calls;
      if (!apply_succeeds || apply_calls == fail_apply_on_call) {
        return false;
      }
      std::vector<connector_t> next;
      for (auto &configuration : configurations) {
        const auto &mode = configuration.specification.mode;
        const auto existing = std::ranges::find(connectors, configuration.slot, &connector_t::slot);
        const auto platform_id = existing != connectors.end() ? existing->platform_id : "slot-" + std::to_string(configuration.slot);
        configuration.platform_id = platform_id;
        const auto divisor = std::gcd(mode.refresh_numerator, mode.refresh_denominator);
        configuration.actual_mode = {mode.width, mode.height, mode.refresh_numerator / divisor, mode.refresh_denominator / divisor, mode.bit_depth, mode.hdr, platform_id + ":mode", configuration.specification.position};
        if (normalize_existing_position && apply_calls > 2 && configuration.slot == 0) {
          ++configuration.actual_mode.position.x;
        }
        next.push_back({configuration.slot, platform_id});
      }
      connectors = std::move(next);
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
        [&]() -> std::optional<std::vector<connector_t>> {
          return inventory();
        },
        [&](std::vector<platform_configuration_t> &values) {
          return apply(values);
        },
        max_count,
        [&](const std::optional<resource_t> &previous, const std::optional<resource_t> &current) {
          changes.emplace_back(previous, current);
        },
        [&]() {
          ++capture_calls;
          return capture_succeeds;
        },
        [&]() {
          ++restore_calls;
          return restore_succeeds;
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

  /**
   * @brief Build a version-two persistence document with the given resource slots.
   *
   * @param slots Resource slots in serialization order.
   * @param ids Resource UUIDs in serialization order.
   * @param persistent Whether resources survive restart.
   * @return Persistence document.
   */
  std::string document_v2(const std::vector<std::uint32_t> &slots, const std::vector<std::string> &ids, const bool persistent) {
    nlohmann::json resources = nlohmann::json::array();
    for (std::size_t index = 0; index < slots.size(); ++index) {
      const auto platform = "slot-" + std::to_string(slots[index]);
      resources.push_back({
        {"id", ids[index]},
        {"name", "Virtual"},
        {"ownerClientUuid", OWNER},
        {"state", "ready"},
        {"requestedMode", {{"width", 1920}, {"height", 1080}, {"refreshNumerator", 60000}, {"refreshDenominator", 1000}, {"bitDepth", 10}, {"hdr", true}}},
        {"actualMode", {{"width", 1920}, {"height", 1080}, {"refreshNumerator", 60}, {"refreshDenominator", 1}, {"bitDepth", 10}, {"hdr", true}, {"id", platform + ":mode"}}},
        {"position", {{"x", 10}, {"y", 20}}},
        {"scale", 1.25},
        {"rotation", 0},
        {"primary", false},
        {"hdr", true},
        {"persistent", persistent},
        {"workspaceId", nullptr},
        {"sessionId", nullptr},
        {"error", nullptr},
        {"revision", 1},
        {"slot", slots[index]},
      });
    }
    return nlohmann::json {{"version", 2}, {"collectionRevision", 4}, {"resources", resources}}.dump();
  }
}  // namespace

TEST(TerraVirtualDisplayTest, CreatesStableResourceInLowestFreeSlot) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  EXPECT_EQ(manager.max_active(), 16);
  const auto result = manager.create(OWNER, specification());
  ASSERT_EQ(result.status, status_t::success);
  ASSERT_EQ(fake.connectors.size(), 1);
  EXPECT_EQ(fake.connectors[0].slot, 0);
  EXPECT_EQ(result.resource->id, RESOURCE);
  EXPECT_EQ(result.resource->slot, 0);
  EXPECT_EQ(result.resource->platform_id, "slot-0");
  EXPECT_EQ(result.resource->actual_mode.id, "slot-0:mode");
  EXPECT_FALSE(to_json(*result.resource).contains("platformId"));
  EXPECT_FALSE(to_json(*result.resource).contains("slot"));
  const auto listed = manager.list(0);
  EXPECT_TRUE(listed.changed);
  EXPECT_EQ(listed.resources.size(), 1);
  EXPECT_FALSE(manager.list(listed.revision).changed);
}

TEST(TerraVirtualDisplayTest, PreservesConnectorIdentityWhenRemovingMiddleSlot) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  const auto first = manager.create(OWNER, specification());
  const auto middle = manager.create(OWNER, specification());
  const auto last = manager.create(OWNER, specification());
  ASSERT_EQ(first.status, status_t::success);
  ASSERT_EQ(middle.status, status_t::success);
  ASSERT_EQ(last.status, status_t::success);
  ASSERT_EQ(fake.connectors.size(), 3);
  EXPECT_EQ(fake.connectors[1].slot, 1);

  ASSERT_EQ(manager.remove(middle.resource->id, middle.resource->revision).status, status_t::success);
  ASSERT_EQ(fake.connectors.size(), 2);
  EXPECT_EQ(fake.connectors[0].slot, 0);
  EXPECT_EQ(fake.connectors[1].slot, 2);
  EXPECT_EQ(manager.get(first.resource->id)->platform_id, "slot-0");
  EXPECT_EQ(manager.get(last.resource->id)->platform_id, "slot-2");
}

TEST(TerraVirtualDisplayTest, AdvancesSiblingRevisionForProviderAdjustedPosition) {
  fake_t fake;
  fake.normalize_existing_position = true;
  manager_t manager {fake.callbacks()};
  const auto first = manager.create(OWNER, specification());
  ASSERT_EQ(first.status, status_t::success);
  ASSERT_TRUE(first.resource);
  EXPECT_EQ(first.resource->revision, 1);

  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  const auto adjusted = manager.get(first.resource->id);
  ASSERT_TRUE(adjusted);
  EXPECT_EQ(adjusted->position.x, first.resource->position.x + 1);
  EXPECT_EQ(adjusted->revision, first.resource->revision + 1);
  EXPECT_EQ(manager.remove(adjusted->id, first.resource->revision).status, status_t::conflict);
}

TEST(TerraVirtualDisplayTest, CreatesWorkspaceLayoutInOneProviderTransaction) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto primary = specification();
  primary.primary = true;
  primary.position = {0, 0};
  auto secondary = specification();
  secondary.position = {1920, 0};

  const auto result = manager.create_batch(OWNER, {primary, secondary});

  ASSERT_EQ(result.status, status_t::success);
  ASSERT_EQ(result.resources.size(), 2);
  EXPECT_EQ(result.resources[0].id, RESOURCE);
  EXPECT_EQ(result.resources[1].id, RESOURCE_2);
  EXPECT_EQ(result.resources[0].slot, 0);
  EXPECT_EQ(result.resources[1].slot, 1);
  EXPECT_EQ(fake.connectors.size(), 2);
  EXPECT_EQ(fake.apply_calls, 2);
  EXPECT_EQ(fake.changes.size(), 2);
}

TEST(TerraVirtualDisplayTest, RejectsInvalidOrPartialWorkspaceLayouts) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto primary = specification();
  primary.primary = true;
  primary.position = {0, 0};
  auto overlapping = specification();
  overlapping.position = {100, 0};
  const auto apply_calls_before = fake.apply_calls;
  EXPECT_EQ(manager.create_batch(OWNER, {primary, overlapping}).status, status_t::invalid);
  EXPECT_EQ(fake.apply_calls, apply_calls_before);

  auto secondary = specification();
  secondary.position = {1920, 0};
  fake.save_succeeds = false;
  EXPECT_EQ(manager.create_batch(OWNER, {primary, secondary}).status, status_t::persistence_error);
  EXPECT_TRUE(fake.connectors.empty());
  EXPECT_TRUE(manager.list().resources.empty());
}

TEST(TerraVirtualDisplayTest, AvailabilityTracksLiveProviderHealth) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  fake.healthy = false;
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.adopt(RESOURCE, 1, OWNER).status, status_t::unavailable);
  fake.healthy = true;
  EXPECT_TRUE(manager.available());
}

TEST(TerraVirtualDisplayTest, RejectsCreationAtProviderCapacity) {
  fake_t fake;
  fake.max_count = 0;
  manager_t manager {fake.callbacks()};

  EXPECT_EQ(manager.max_active(), 0);
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::limit_reached);
  EXPECT_TRUE(fake.changes.empty());
}

TEST(TerraVirtualDisplayTest, ReportsOnlyCommittedResourceTransitions) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  ASSERT_EQ(fake.changes.size(), 1);
  EXPECT_FALSE(fake.changes[0].first);
  ASSERT_TRUE(fake.changes[0].second);
  EXPECT_EQ(fake.changes[0].second->id, RESOURCE);

  patch_t patch;
  patch.name = "Changed";
  ASSERT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::success);
  ASSERT_EQ(fake.changes.size(), 2);
  ASSERT_TRUE(fake.changes[1].first);
  ASSERT_TRUE(fake.changes[1].second);
  EXPECT_EQ(fake.changes[1].first->name, "Virtual");
  EXPECT_EQ(fake.changes[1].second->name, "Changed");

  ASSERT_EQ(manager.remove(RESOURCE, 2).status, status_t::success);
  ASSERT_EQ(fake.changes.size(), 3);
  ASSERT_TRUE(fake.changes[2].first);
  EXPECT_FALSE(fake.changes[2].second);
  EXPECT_EQ(fake.changes[2].first->id, RESOURCE);
}

TEST(TerraVirtualDisplayTest, ProviderConnectorDriftFailsClosed) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  fake.connectors.push_back({7, "untracked"});
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::provider_error);
  patch_t patch;
  patch.name = "Changed";
  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::provider_error);
  EXPECT_EQ(manager.remove(RESOURCE, 1).status, status_t::provider_error);
  EXPECT_EQ(manager.get(RESOURCE)->name, "Virtual");
}

TEST(TerraVirtualDisplayTest, ApplyAndPersistenceFailuresRollbackProviderAndPublication) {
  fake_t apply_fake;
  manager_t apply_manager {apply_fake.callbacks()};
  const auto captures_before = apply_fake.capture_calls;
  apply_fake.apply_succeeds = false;
  EXPECT_EQ(apply_manager.create(OWNER, specification()).status, status_t::provider_error);
  EXPECT_TRUE(apply_fake.connectors.empty());
  EXPECT_EQ(apply_fake.capture_calls, captures_before + 1);
  EXPECT_EQ(apply_fake.restore_calls, 1);
  EXPECT_FALSE(apply_manager.get(RESOURCE));

  fake_t save_fake;
  manager_t save_manager {save_fake.callbacks()};
  save_fake.save_succeeds = false;
  EXPECT_EQ(save_manager.create(OWNER, specification()).status, status_t::persistence_error);
  EXPECT_TRUE(save_fake.connectors.empty());
  EXPECT_EQ(save_fake.restore_calls, 1);
  EXPECT_FALSE(save_manager.get(RESOURCE));
}

TEST(TerraVirtualDisplayTest, ValidatesInputAndRevisionPreconditions) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto invalid = specification();
  invalid.scale = 0;
  EXPECT_EQ(manager.create(OWNER, invalid).status, status_t::invalid);
  auto unsupported_depth = specification();
  unsupported_depth.mode.bit_depth = 12;
  EXPECT_EQ(manager.create(OWNER, unsupported_depth).status, status_t::invalid);
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  patch_t patch;
  patch.name = "Changed";
  EXPECT_EQ(manager.patch(RESOURCE, 99, patch).status, status_t::conflict);
  const auto changed = manager.patch(RESOURCE, 1, patch);
  ASSERT_EQ(changed.status, status_t::success);
  EXPECT_EQ(changed.resource->revision, 2);
  EXPECT_EQ(changed.resource->name, "Changed");
}

TEST(TerraVirtualDisplayTest, NormalizesRationalRefreshForExactModeMatch) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto spec = specification();
  spec.mode.refresh_numerator = 60000;
  spec.mode.refresh_denominator = 1000;
  const auto result = manager.create(OWNER, spec);
  ASSERT_EQ(result.status, status_t::success);
  EXPECT_EQ(result.resource->actual_mode.refresh_numerator, 60);
  EXPECT_EQ(result.resource->actual_mode.refresh_denominator, 1);
}

TEST(TerraVirtualDisplayTest, AttachmentBlocksDeleteUntilDetached) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  const auto attached = manager.attach(RESOURCE, 1, {.session_id = SESSION});
  ASSERT_EQ(attached.status, status_t::success);
  EXPECT_EQ(manager.remove(RESOURCE, 2).status, status_t::conflict);
  const auto detached = manager.detach(RESOURCE, 2);
  ASSERT_EQ(detached.status, status_t::success);
  EXPECT_EQ(manager.remove(RESOURCE, 3).status, status_t::success);
  EXPECT_TRUE(fake.connectors.empty());
}

TEST(TerraVirtualDisplayTest, RevocationOrphansAndAdoptionClaimsResource) {
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

TEST(TerraVirtualDisplayTest, RevocationDeletesEphemeralAndPreservesPersistentSlot) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification(false)).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, specification(true)).status, status_t::success);
  ASSERT_EQ(manager.revoke_owner(OWNER), status_t::success);
  EXPECT_FALSE(manager.get(RESOURCE));
  const auto persistent = manager.get(RESOURCE_2);
  ASSERT_TRUE(persistent);
  EXPECT_FALSE(persistent->owner_client_uuid);
  EXPECT_EQ(persistent->slot, 1);
  EXPECT_EQ(persistent->platform_id, "slot-1");
  EXPECT_EQ(persistent->revision, 2);
  ASSERT_EQ(fake.connectors.size(), 1);
  EXPECT_EQ(fake.connectors[0].slot, 1);
}

TEST(TerraVirtualDisplayTest, AttachmentRequiresExactlyOneValidTarget) {
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

TEST(TerraVirtualDisplayTest, RestartRetainsPersistentAndRemovesEphemeralResources) {
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
  EXPECT_EQ(persistent->slot, 1);
  EXPECT_EQ(persistent->platform_id, "slot-1");
  ASSERT_EQ(fake.connectors.size(), 1);
  EXPECT_EQ(fake.connectors[0].slot, 1);
}

TEST(TerraVirtualDisplayTest, RestartWithNoManagedDisplaysClearsStaleProvider) {
  fake_t fake;
  fake.connectors.push_back({3, "stale"});
  manager_t manager {fake.callbacks()};
  EXPECT_TRUE(manager.available());
  EXPECT_TRUE(fake.connectors.empty());
  EXPECT_EQ(fake.apply_calls, 1);
}

TEST(TerraVirtualDisplayTest, RestartRepairsUntrackedProviderConnector) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, specification(true)).status, status_t::success);
  }
  fake.connectors.push_back({9, "untracked"});
  fake.uuid_index = 0;

  manager_t restarted {fake.callbacks()};

  ASSERT_TRUE(restarted.available());
  ASSERT_TRUE(restarted.get(RESOURCE));
  EXPECT_EQ(restarted.get(RESOURCE)->platform_id, "slot-0");
  ASSERT_EQ(fake.connectors.size(), 1);
  EXPECT_EQ(fake.connectors[0].slot, 0);
}

TEST(TerraVirtualDisplayTest, RestartDetachesPersistentRuntimeAttachment) {
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

TEST(TerraVirtualDisplayTest, FailedConfigurationRollbackDisablesManager) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, specification()).status, status_t::success);
  fake.apply_succeeds = false;
  fake.restore_succeeds = false;
  patch_t patch;
  patch.name = "Changed";

  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::provider_error);
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.patch(RESOURCE, 1, patch).status, status_t::unavailable);
}

TEST(TerraVirtualDisplayTest, MigratesVersionOneDocumentAndDropsBaseline) {
  fake_t fake;
  fake.connectors.push_back({5, "stale"});
  fake.document = R"({"version":1,"baselineCount":2,"baselineInventory":["base-0","base-1"],"collectionRevision":3,"resources":[{"id":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","name":"Virtual","ownerClientUuid":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","state":"ready","requestedMode":{"width":1920,"height":1080,"refreshNumerator":60000,"refreshDenominator":1000,"bitDepth":10,"hdr":true},"actualMode":{"width":1920,"height":1080,"refreshNumerator":60,"refreshDenominator":1,"bitDepth":10,"hdr":true,"id":"base-0:mode"},"position":{"x":10,"y":20},"scale":1.25,"rotation":0,"primary":false,"hdr":true,"persistent":true,"workspaceId":null,"sessionId":null,"error":null,"revision":1,"platformId":"DISPLAY\\SLV1337\\UID256"}]})";
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  const auto resource = manager.get(RESOURCE);
  ASSERT_TRUE(resource);
  EXPECT_EQ(resource->slot, 0);
  EXPECT_EQ(resource->platform_id, "slot-0");
  ASSERT_TRUE(fake.document);
  const auto parsed = nlohmann::json::parse(*fake.document);
  EXPECT_EQ(parsed.at("version"), 2);
  EXPECT_FALSE(parsed.contains("baselineCount"));
  EXPECT_FALSE(parsed.contains("baselineInventory"));
  ASSERT_EQ(parsed.at("resources").size(), 1);
  EXPECT_EQ(parsed.at("resources").at(0).at("slot"), 0);
  EXPECT_FALSE(parsed.at("resources").at(0).contains("platformId"));
}

TEST(TerraVirtualDisplayTest, MalformedPersistenceFailsClosed) {
  fake_t fake;
  fake.document = R"({"version":2,"collectionRevision":0,"resources":[{"id":"bad"}]})";
  manager_t manager {fake.callbacks()};
  EXPECT_FALSE(manager.available());
  EXPECT_EQ(manager.create(OWNER, specification()).status, status_t::unavailable);
  EXPECT_TRUE(manager.list().resources.empty());
}

TEST(TerraVirtualDisplayTest, RejectsLegacyTwelveBitModeDocument) {
  fake_t fake;
  fake.document = document_v2({0}, {RESOURCE}, true);
  fake.document->replace(fake.document->find("\"bitDepth\":10"), 12, "\"bitDepth\":12");
  manager_t manager {fake.callbacks()};
  EXPECT_FALSE(manager.available());
}
