/**
 * @file tests/unit/test_terra_workspaces.cpp
 * @brief Tests for standalone Terra workspace core.
 */

// standard includes
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/terra_workspaces.h>

namespace {
  using namespace terra_workspaces;
  constexpr const char *OWNER = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";  ///< Owner UUID.
  constexpr const char *OTHER = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";  ///< Other owner UUID.
  constexpr const char *WORKSPACE = "cccccccc-cccc-cccc-cccc-cccccccccccc";  ///< Workspace UUID.
  constexpr const char *APP = "dddddddd-dddd-dddd-dddd-dddddddddddd";  ///< Desktop app UUID.
  constexpr const char *APP_2 = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";  ///< Permitted app UUID.
  constexpr const char *PROFILE = "11111111-1111-1111-1111-111111111111";  ///< Profile UUID.
  constexpr const char *SANDBOX = "33333333-3333-3333-3333-333333333333";  ///< Sandbox UUID.
  constexpr const char *DISPLAY = "44444444-4444-4444-4444-444444444444";  ///< Display UUID.
  constexpr const char *CLAIM = "55555555-5555-5555-5555-555555555555";  ///< Claim UUID.
  constexpr const char *WORKSPACE_2 = "66666666-6666-6666-6666-666666666666";  ///< Second workspace UUID.

  /**
   * @brief Deterministic persistence and runtime provider.
   */
  struct fake_t {
    std::optional<std::string> document;  ///< Persisted document.
    std::int64_t time = 1000;  ///< Current time.
    bool save_succeeds = true;  ///< Save result.
    bool validation_succeeds = true;  ///< Policy validation result.
    bool prepare_succeeds = true;  ///< Preparation result.
    bool stop_succeeds = true;  ///< Stop result.
    bool restore_succeeds = true;  ///< Runtime restoration result.
    bool reconcile_succeeds = true;  ///< Reconciliation result.
    bool report_preparation_progress = false;  ///< Report one acquired display before completion.
    bool cleanup_succeeds = true;  ///< Whether failed preparation cleanup resolves all IDs.
    bool progress_was_persisted = false;  ///< Whether manager persisted reported IDs before provider continued.
    int fail_stop_on_call = 0;  ///< One-based stop call to fail, or zero.
    std::vector<std::string> uuids {WORKSPACE, WORKSPACE_2};  ///< Generated workspace UUIDs.
    std::size_t uuid_index = 0;  ///< Next generated UUID.
    int prepare_calls = 0;  ///< Preparation count.
    int cleanup_calls = 0;  ///< Failed cleanup count.
    int stop_calls = 0;  ///< Stop count.
    int restore_calls = 0;  ///< Failed stop restoration count.
    bool last_terminate = false;  ///< Last stop mode.
    std::string prepared_owner;  ///< Owner supplied to runtime preparation.
    prepared_t cleaned {false};  ///< IDs passed to failed cleanup.
    std::vector<std::string> restored_ids;  ///< Restored workspace IDs.
    std::vector<std::string> stopped_ids;  ///< Stop-attempt workspace IDs.

    /** @brief Build callbacks bound to fake. @return Workspace callbacks. */
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
        [&]() {
          return time++;
        },
        [&]() {
          return uuids.at(uuid_index++);
        },
        [&](const std::string &) {
          return validation_succeeds;
        },
        [&](const std::string &, const std::string &) {
          return validation_succeeds;
        },
        [&](const std::string &, const nlohmann::json &) {
          return validation_succeeds;
        },
        [&](const nlohmann::json &) {
          return validation_succeeds;
        },
        [&](const peripheral_policy_t &) {
          return validation_succeeds;
        },
        [&](const preparation_t &request, const std::function<bool(const prepared_t &)> &persist) -> std::optional<prepared_t> {
          ++prepare_calls;
          prepared_owner = request.owner_client_uuid;
          if (report_preparation_progress) {
            prepared_t progress {true, std::nullopt, std::nullopt, {DISPLAY}, {}};
            if (!persist(progress)) {
              progress.success = false;
              return progress;
            }
            progress_was_persisted = document && document->find(DISPLAY) != std::string::npos;
            progress.success = prepare_succeeds;
            return progress;
          }
          return prepared_t {prepare_succeeds, std::nullopt, SANDBOX, {DISPLAY}, {CLAIM}};
        },
        [&](const prepared_t &value) {
          ++cleanup_calls;
          cleaned = value;
          return cleanup_succeeds ? prepared_t {false} : value;
        },
        [&](const resource_t &resource, const bool terminate) {
          ++stop_calls;
          stopped_ids.push_back(resource.id);
          last_terminate = terminate;
          return stop_succeeds && stop_calls != fail_stop_on_call;
        },
        [&](const resource_t &resource) {
          ++restore_calls;
          restored_ids.push_back(resource.id);
          return restore_succeeds;
        },
        [&](const resource_t &) {
          return reconcile_succeeds;
        },
      };
    }
  };

  /** @brief Build valid complete definition. @param persistent Persistence setting. @return Definition. */
  definition_t definition(const bool persistent = true) {
    return {"Development", "", false, APP, {APP_2}, PROFILE, PROFILE, PROFILE, PROFILE, {nlohmann::json {{"name", "Display"}}}, {{}, {"gamepad"}, "release"}, persistent, cleanup_policy_t::on_stop};
  }

  /** @brief Build exact wire creation object. @return Workspace definition JSON. */
  nlohmann::json definition_request() {
    return {{"name", "Development"}, {"description", ""}, {"shared", false}, {"desktopAppUuid", APP}, {"permittedAppUuids", {APP_2}}, {"displayProfileId", PROFILE}, {"streamProfileId", PROFILE}, {"launchProfileId", PROFILE}, {"sandboxProfileId", PROFILE}, {"virtualDisplays", nlohmann::json::array({{{"name", "Display"}}})}, {"peripheralPolicy", {{"requiredDeviceIds", nlohmann::json::array()}, {"requiredClasses", {"gamepad"}}, {"disconnectPolicy", "release"}}}, {"persistent", true}, {"cleanupPolicy", "on-stop"}};
  }
}  // namespace

TEST(TerraWorkspacesTest, ParsesExactWireRequests) {
  const auto definition = parse_definition(definition_request());
  ASSERT_TRUE(definition);
  EXPECT_EQ(definition->desktop_app_uuid, APP);
  auto extra = definition_request();
  extra["unknown"] = true;
  EXPECT_FALSE(parse_definition(extra));

  const auto patch = parse_patch({{"name", "Changed"}, {"displayProfileId", nullptr}});
  ASSERT_TRUE(patch);
  EXPECT_EQ(patch->name, "Changed");
  ASSERT_TRUE(patch->display_profile_id);
  EXPECT_FALSE(*patch->display_profile_id);
  EXPECT_FALSE(parse_patch(nlohmann::json::object()));

  const auto start = parse_start_request({{"appUuid", APP_2}, {"profileOverrides", {{"display", nullptr}, {"stream", {{"width", 1920}}}}}});
  ASSERT_TRUE(start);
  EXPECT_EQ(start->app_uuid, APP_2);
  ASSERT_TRUE(start->profile_overrides.display);
  EXPECT_TRUE(start->profile_overrides.display->is_null());
  EXPECT_FALSE(parse_start_request({{"profileOverrides", {{"unknown", nullptr}}}}));
}

TEST(TerraWorkspacesTest, PublishesEveryDurableLifecycleTransition) {
  fake_t fake;
  std::vector<std::pair<std::optional<state_t>, std::optional<state_t>>> changes;
  auto callbacks = fake.callbacks();
  callbacks.changed = [&](const std::optional<resource_t> &previous, const std::optional<resource_t> &current) {
    changes.emplace_back(previous ? std::optional {previous->state} : std::nullopt, current ? std::optional {current->state} : std::nullopt);
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  ASSERT_EQ(manager.stop(WORKSPACE, 3, true).status, status_t::success);
  ASSERT_EQ(manager.remove(WORKSPACE, 5).status, status_t::success);
  EXPECT_EQ(changes, (std::vector<std::pair<std::optional<state_t>, std::optional<state_t>>> {
                       {std::nullopt, state_t::stopped},
                       {state_t::stopped, state_t::preparing},
                       {state_t::preparing, state_t::ready},
                       {state_t::ready, state_t::stopping},
                       {state_t::stopping, state_t::stopped},
                       {state_t::stopped, std::nullopt},
                     }));
}

TEST(TerraWorkspacesTest, CreatesExactDefinitionAndCollectionSnapshot) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  const auto created = manager.create(OWNER, definition());
  ASSERT_EQ(created.status, status_t::success);
  ASSERT_TRUE(created.resource);
  EXPECT_EQ(created.resource->id, WORKSPACE);
  EXPECT_EQ(created.resource->state, state_t::stopped);
  EXPECT_EQ(created.resource->revision, 1);
  const auto json = to_json(*created.resource);
  EXPECT_EQ(json["name"], "Development");
  EXPECT_EQ(json["desktopAppUuid"], APP);
  EXPECT_EQ(json["cleanupPolicy"], "on-stop");
  EXPECT_EQ(json["ownerClientUuid"], OWNER);
  const auto listed = manager.list(0);
  EXPECT_TRUE(listed.changed);
  ASSERT_EQ(listed.workspaces.size(), 1);
  EXPECT_FALSE(manager.list(listed.revision).changed);
  EXPECT_TRUE(manager.list(listed.revision).workspaces.empty());
}

TEST(TerraWorkspacesTest, ValidatesCanonicalIdsPoliciesAndCallbacks) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto invalid = definition();
  invalid.desktop_app_uuid = "DDDDDDDD-DDDD-DDDD-DDDD-DDDDDDDDDDDD";
  EXPECT_EQ(manager.create(OWNER, invalid).status, status_t::invalid);
  invalid = definition();
  invalid.virtual_displays[0]["workspaceId"] = WORKSPACE;
  EXPECT_EQ(manager.create(OWNER, invalid).status, status_t::invalid);
  invalid = definition();
  invalid.peripheral_policy.disconnect_policy = "drop";
  EXPECT_EQ(manager.create(OWNER, invalid).status, status_t::invalid);
  fake.validation_succeeds = false;
  EXPECT_EQ(manager.create(OWNER, definition()).status, status_t::invalid);
}

TEST(TerraWorkspacesTest, PatchUsesCompleteReplacementsAndRollsBackFailedSave) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  patch_t patch;
  patch.name = "Changed";
  patch.permitted_app_uuids = std::vector<std::string> {};
  const auto changed = manager.patch(WORKSPACE, 1, patch);
  ASSERT_EQ(changed.status, status_t::success);
  EXPECT_EQ(changed.resource->definition.name, "Changed");
  EXPECT_TRUE(changed.resource->definition.permitted_app_uuids.empty());
  fake.save_succeeds = false;
  patch.name = "Not published";
  EXPECT_EQ(manager.patch(WORKSPACE, 2, patch).status, status_t::persistence_error);
  EXPECT_EQ(manager.get(WORKSPACE)->definition.name, "Changed");
  EXPECT_EQ(manager.get(WORKSPACE)->revision, 2);
}

TEST(TerraWorkspacesTest, StartPreparesReadyWorkspaceWithoutMediaSession) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  start_request_t request {.app_uuid = APP_2};
  request.profile_overrides.display = nlohmann::json {{"mode", "native"}};
  const auto started = manager.start(WORKSPACE, 1, request);
  ASSERT_EQ(started.status, status_t::success);
  EXPECT_EQ(started.resource->state, state_t::ready);
  EXPECT_FALSE(started.resource->session_id);
  EXPECT_EQ(started.resource->sandbox_id, SANDBOX);
  EXPECT_EQ(started.resource->display_ids, (std::vector<std::string> {DISPLAY}));
  EXPECT_EQ(started.resource->peripheral_claim_ids, (std::vector<std::string> {CLAIM}));
  EXPECT_EQ(started.resource->revision, 3);
  EXPECT_EQ(fake.prepare_calls, 1);
  EXPECT_EQ(fake.prepared_owner, OWNER);
  const auto repeated = manager.start(WORKSPACE, started.resource->revision);
  EXPECT_EQ(repeated.status, status_t::conflict);
  EXPECT_EQ(fake.prepare_calls, 1);
}

TEST(TerraWorkspacesTest, FailedPreparationCleansPartialActualResources) {
  fake_t fake;
  fake.prepare_succeeds = false;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  const auto result = manager.start(WORKSPACE, 1);
  ASSERT_EQ(result.status, status_t::preparation_error);
  ASSERT_TRUE(result.resource);
  EXPECT_EQ(result.resource->state, state_t::stopped);
  EXPECT_EQ(result.resource->revision, 3);
  EXPECT_TRUE(result.resource->error.is_null());
  EXPECT_EQ(fake.cleanup_calls, 1);
  EXPECT_EQ(fake.cleaned.sandbox_id, SANDBOX);
  EXPECT_EQ(fake.cleaned.display_ids, (std::vector<std::string> {DISPLAY}));
  EXPECT_EQ(fake.cleaned.peripheral_claim_ids, (std::vector<std::string> {CLAIM}));
}

TEST(TerraWorkspacesTest, PersistsPreparationProgressAndRetainsFailedCleanup) {
  fake_t fake;
  fake.prepare_succeeds = false;
  fake.report_preparation_progress = true;
  fake.cleanup_succeeds = false;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);

  const auto result = manager.start(WORKSPACE, 1);
  ASSERT_EQ(result.status, status_t::preparation_error);
  ASSERT_TRUE(result.resource);
  EXPECT_TRUE(fake.progress_was_persisted);
  EXPECT_EQ(result.resource->state, state_t::failed);
  EXPECT_EQ(result.resource->display_ids, (std::vector<std::string> {DISPLAY}));
  EXPECT_EQ(result.resource->error["code"], "cleanup_failed");
  EXPECT_EQ(result.resource->revision, 4);
  EXPECT_EQ(manager.start(WORKSPACE, result.resource->revision).status, status_t::conflict);

  const auto retried = manager.stop(WORKSPACE, result.resource->revision, true);
  ASSERT_EQ(retried.status, status_t::success);
  EXPECT_EQ(retried.resource->state, state_t::stopped);
  EXPECT_TRUE(retried.resource->display_ids.empty());
}

TEST(TerraWorkspacesTest, ReadyRevisionAdvancesPastPreparationProgress) {
  fake_t fake;
  fake.report_preparation_progress = true;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);

  const auto result = manager.start(WORKSPACE, 1);
  ASSERT_EQ(result.status, status_t::success);
  ASSERT_TRUE(result.resource);
  EXPECT_EQ(result.resource->state, state_t::ready);
  EXPECT_EQ(result.resource->revision, 4);
}

TEST(TerraWorkspacesTest, FailedReadyPublicationCleansPreparedResources) {
  fake_t fake;
  int saves = 0;
  auto callbacks = fake.callbacks();
  callbacks.save = [&](const std::string &value) {
    ++saves;
    if (saves == 4) {
      return false;
    }
    fake.document = value;
    return true;
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  EXPECT_EQ(manager.start(WORKSPACE, 1).status, status_t::persistence_error);
  EXPECT_EQ(fake.cleanup_calls, 1);
  EXPECT_EQ(manager.get(WORKSPACE)->state, state_t::stopped);
  EXPECT_TRUE(manager.available());
}

TEST(TerraWorkspacesTest, FailedReadyPublicationRetainsUnresolvedResources) {
  fake_t fake;
  fake.cleanup_succeeds = false;
  int saves = 0;
  auto callbacks = fake.callbacks();
  callbacks.save = [&](const std::string &value) {
    ++saves;
    if (saves == 4) {
      return false;
    }
    fake.document = value;
    return true;
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);

  const auto result = manager.start(WORKSPACE, 1);
  ASSERT_EQ(result.status, status_t::persistence_error);
  ASSERT_TRUE(result.resource);
  EXPECT_EQ(result.resource->state, state_t::failed);
  EXPECT_EQ(result.resource->sandbox_id, SANDBOX);
  EXPECT_EQ(result.resource->display_ids, (std::vector<std::string> {DISPLAY}));
  EXPECT_EQ(result.resource->peripheral_claim_ids, (std::vector<std::string> {CLAIM}));
  EXPECT_EQ(result.resource->error["code"], "cleanup_failed");
}

TEST(TerraWorkspacesTest, FailedStartRollbackSaveDisablesManager) {
  fake_t fake;
  int saves = 0;
  auto callbacks = fake.callbacks();
  callbacks.save = [&](const std::string &value) {
    ++saves;
    if (saves >= 4) {
      return false;
    }
    fake.document = value;
    return true;
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  EXPECT_EQ(manager.start(WORKSPACE, 1).status, status_t::unavailable);
  EXPECT_EQ(fake.cleanup_calls, 1);
  EXPECT_FALSE(manager.available());
}

TEST(TerraWorkspacesTest, StopDisconnectsOrTerminatesWithExpectedResourceBehavior) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  const auto active = manager.activate(WORKSPACE, 3, "77777777-7777-4777-8777-777777777777");
  ASSERT_EQ(active.status, status_t::success);
  EXPECT_EQ(active.resource->state, state_t::active);
  EXPECT_TRUE(active.resource->session_id);
  EXPECT_EQ(manager.start(WORKSPACE, 3).status, status_t::conflict);
  EXPECT_EQ(manager.activate(WORKSPACE, active.resource->revision, *active.resource->session_id).status, status_t::success);
  const auto disconnected = manager.stop(WORKSPACE, 4, false);
  ASSERT_EQ(disconnected.status, status_t::success);
  EXPECT_EQ(disconnected.resource->state, state_t::ready);
  EXPECT_EQ(disconnected.resource->revision, 6);
  EXPECT_FALSE(disconnected.resource->session_id);
  EXPECT_EQ(disconnected.resource->sandbox_id, SANDBOX);
  EXPECT_FALSE(fake.last_terminate);
  const auto stopped = manager.stop(WORKSPACE, 6, true);
  ASSERT_EQ(stopped.status, status_t::success);
  EXPECT_EQ(stopped.resource->state, state_t::stopped);
  EXPECT_FALSE(stopped.resource->sandbox_id);
  EXPECT_TRUE(stopped.resource->display_ids.empty());
  EXPECT_EQ(stopped.resource->revision, 8);
  EXPECT_TRUE(fake.last_terminate);
}

TEST(TerraWorkspacesTest, FailedStopAndPersistencePreservePublishedState) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  fake.stop_succeeds = false;
  const auto failed_stop = manager.stop(WORKSPACE, 3, true);
  EXPECT_EQ(failed_stop.status, status_t::provider_error);
  ASSERT_TRUE(failed_stop.resource);
  EXPECT_EQ(failed_stop.resource->revision, 5);
  EXPECT_EQ(manager.get(WORKSPACE)->state, state_t::ready);
  EXPECT_EQ(fake.restore_calls, 1);
  fake.stop_succeeds = true;
  fake.save_succeeds = false;
  EXPECT_EQ(manager.stop(WORKSPACE, 5, true).status, status_t::persistence_error);
  EXPECT_EQ(manager.get(WORKSPACE)->state, state_t::ready);
  EXPECT_EQ(fake.restore_calls, 1);
  EXPECT_TRUE(manager.available());
}

TEST(TerraWorkspacesTest, FailedStopRestorationDisablesManager) {
  fake_t fake;
  int saves = 0;
  auto callbacks = fake.callbacks();
  callbacks.save = [&](const std::string &value) {
    ++saves;
    if (saves == 6) {
      return false;
    }
    fake.document = value;
    return true;
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  fake.restore_succeeds = false;
  EXPECT_EQ(manager.stop(WORKSPACE, 3, true).status, status_t::unavailable);
  EXPECT_FALSE(manager.available());
}

TEST(TerraWorkspacesTest, RevocationStopsAndOrphansPersistentButDeletesEphemeral) {
  fake_t persistent_fake;
  manager_t persistent {persistent_fake.callbacks()};
  ASSERT_EQ(persistent.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(persistent.start(WORKSPACE, 1).status, status_t::success);
  ASSERT_EQ(persistent.revoke_owner(OWNER), status_t::success);
  const auto orphan = persistent.get(WORKSPACE);
  ASSERT_TRUE(orphan);
  EXPECT_FALSE(orphan->owner_client_uuid);
  EXPECT_EQ(orphan->state, state_t::stopped);
  EXPECT_EQ(persistent_fake.stop_calls, 1);
  EXPECT_EQ(persistent.adopt(WORKSPACE, orphan->revision, OTHER).status, status_t::success);
  EXPECT_EQ(persistent.adopt(WORKSPACE, orphan->revision, OWNER).status, status_t::resource_busy);

  fake_t ephemeral_fake;
  manager_t ephemeral {ephemeral_fake.callbacks()};
  ASSERT_EQ(ephemeral.create(OWNER, definition(false)).status, status_t::success);
  ASSERT_EQ(ephemeral.revoke_owner(OWNER), status_t::success);
  EXPECT_FALSE(ephemeral.get(WORKSPACE));
}

TEST(TerraWorkspacesTest, RejectsAdoptionAfterOrphanBecomesEphemeral) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.revoke_owner(OWNER), status_t::success);
  const auto orphan = manager.get(WORKSPACE);
  ASSERT_TRUE(orphan);

  patch_t patch;
  patch.persistent = false;
  const auto ephemeral = manager.patch(WORKSPACE, orphan->revision, patch);
  ASSERT_EQ(ephemeral.status, status_t::success);
  ASSERT_TRUE(ephemeral.resource);
  EXPECT_EQ(manager.adopt(WORKSPACE, ephemeral.resource->revision, OTHER).status, status_t::invalid);
}

TEST(TerraWorkspacesTest, MultiResourceRevokeNeverRestartsRevokedRuntimeAfterFailure) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE_2, 1).status, status_t::success);
  fake.fail_stop_on_call = 2;
  EXPECT_EQ(manager.revoke_owner(OWNER), status_t::provider_error);
  EXPECT_EQ(fake.stop_calls, 2);
  EXPECT_TRUE(fake.restored_ids.empty());
  EXPECT_EQ(manager.get(WORKSPACE)->owner_client_uuid, OWNER);
  EXPECT_FALSE(manager.get(WORKSPACE_2)->owner_client_uuid);
  const auto persisted = nlohmann::json::parse(*fake.document);
  const auto persisted_first = std::ranges::find_if(persisted["workspaces"], [](const auto &value) {
    return value["id"] == WORKSPACE_2;
  });
  ASSERT_NE(persisted_first, persisted["workspaces"].end());
  EXPECT_TRUE((*persisted_first)["ownerClientUuid"].is_null());
}

TEST(TerraWorkspacesTest, SelectiveRevocationRetainsAuthorizedDefinitions) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  const auto stale_revision = manager.list().revision;
  auto retained = definition();
  retained.name = "Retained";
  ASSERT_EQ(manager.create(OWNER, retained).status, status_t::success);
  EXPECT_EQ(manager.revoke_unauthorized(OWNER, {WORKSPACE_2}, stale_revision), status_t::conflict);
  EXPECT_EQ(manager.get(WORKSPACE)->owner_client_uuid, OWNER);
  ASSERT_EQ(manager.revoke_unauthorized(OWNER, {WORKSPACE_2}, manager.list().revision), status_t::success);
  EXPECT_FALSE(manager.get(WORKSPACE)->owner_client_uuid);
  EXPECT_EQ(manager.get(WORKSPACE_2)->owner_client_uuid, OWNER);
}

TEST(TerraWorkspacesTest, RevokeSaveFailureFailsClosedWithoutRestartingRuntime) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  ASSERT_EQ(manager.start(WORKSPACE_2, 1).status, status_t::success);
  fake.save_succeeds = false;
  EXPECT_EQ(manager.revoke_owner(OWNER), status_t::unavailable);
  EXPECT_EQ(fake.stop_calls, 1);
  EXPECT_EQ(fake.restore_calls, 0);
  EXPECT_EQ(manager.get(WORKSPACE)->owner_client_uuid, OWNER);
  EXPECT_EQ(manager.get(WORKSPACE_2)->owner_client_uuid, OWNER);
  EXPECT_FALSE(manager.available());
}

TEST(TerraWorkspacesTest, RestartRejectsMalformedRecordsFailClosed) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
    ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  }
  auto document = nlohmann::json::parse(*fake.document);
  document["workspaces"].push_back({{"id", "bad"}});
  auto extra_key = document["workspaces"][0];
  extra_key["id"] = WORKSPACE_2;
  extra_key["unexpected"] = true;
  document["workspaces"].push_back(extra_key);
  auto bad_peripheral = document["workspaces"][0];
  bad_peripheral["id"] = OTHER;
  bad_peripheral["peripheralPolicy"]["unexpected"] = true;
  document["workspaces"].push_back(bad_peripheral);
  fake.document = document.dump();
  fake.reconcile_succeeds = false;
  manager_t restarted {fake.callbacks()};
  EXPECT_FALSE(restarted.available());
  EXPECT_TRUE(restarted.list().workspaces.empty());
}

TEST(TerraWorkspacesTest, PersistentDefinitionsSurviveRestartAndEphemeralDoNot) {
  fake_t persistent_fake;
  {
    manager_t manager {persistent_fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  }
  manager_t restarted {persistent_fake.callbacks()};
  EXPECT_TRUE(restarted.get(WORKSPACE));

  fake_t ephemeral_fake;
  {
    manager_t manager {ephemeral_fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, definition(false)).status, status_t::success);
  }
  manager_t ephemeral_restart {ephemeral_fake.callbacks()};
  EXPECT_FALSE(ephemeral_restart.get(WORKSPACE));
}

TEST(TerraWorkspacesTest, DeleteAndMutationsEnforceRevisionAndLifecycle) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  EXPECT_EQ(manager.remove(WORKSPACE, 1).status, status_t::not_found);
  ASSERT_EQ(manager.create(OWNER, definition()).status, status_t::success);
  EXPECT_EQ(manager.remove(WORKSPACE, 9).status, status_t::conflict);
  ASSERT_EQ(manager.start(WORKSPACE, 1).status, status_t::success);
  patch_t patch;
  patch.name = "Blocked";
  EXPECT_EQ(manager.patch(WORKSPACE, 3, patch).status, status_t::conflict);
  EXPECT_EQ(manager.remove(WORKSPACE, 3).status, status_t::conflict);
}

TEST(TerraWorkspacesTest, MalformedDocumentFailsClosedAndInitialSaveFailureUnavailable) {
  fake_t malformed;
  malformed.document = "not json";
  manager_t malformed_manager {malformed.callbacks()};
  EXPECT_FALSE(malformed_manager.available());
  EXPECT_EQ(malformed_manager.create(OWNER, definition()).status, status_t::unavailable);

  fake_t failed_save;
  failed_save.save_succeeds = false;
  manager_t failed_manager {failed_save.callbacks()};
  EXPECT_FALSE(failed_manager.available());
}
