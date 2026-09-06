/**
 * @file tests/unit/test_eclipse_sandboxes.cpp
 * @brief Tests for standalone Eclipse sandbox lifecycle core.
 */

// standard includes
#include <atomic>
#include <future>
#include <optional>
#include <string>
#include <vector>

// lib includes
#include <gtest/gtest.h>

// local includes
#include <src/eclipse_sandboxes.h>

namespace {
  using namespace eclipse_sandboxes;
  constexpr const char *OWNER = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";  ///< Test owner UUID.
  constexpr const char *SANDBOX = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";  ///< Test sandbox UUID.
  constexpr const char *SANDBOX_2 = "cccccccc-cccc-cccc-cccc-cccccccccccc";  ///< Second sandbox UUID.
  constexpr const char *PROFILE = "dddddddd-dddd-dddd-dddd-dddddddddddd";  ///< Test profile UUID.
  constexpr const char *APP = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";  ///< Test application UUID.
  constexpr const char *SESSION = "ffffffff-ffff-ffff-ffff-ffffffffffff";  ///< Test session UUID.
  constexpr const char *DISPLAY = "11111111-1111-1111-1111-111111111111";  ///< Test display UUID.
  constexpr const char *CLAIM = "22222222-2222-2222-2222-222222222222";  ///< Test claim UUID.

  /**
   * @brief In-memory persistence and sandbox provider.
   */
  struct fake_t {
    std::optional<std::string> document;  ///< Persisted manager document.
    std::vector<std::string> ids {SANDBOX, SANDBOX_2};  ///< Generated UUIDs.
    std::atomic_size_t id_index = 0;  ///< Next generated UUID.
    std::int64_t now = 1000;  ///< Test clock.
    bool save_succeeds = true;  ///< Persistence result.
    bool capable = true;  ///< Capability result.
    bool resolve_succeeds = true;  ///< Application resolution result.
    bool launch_succeeds = true;  ///< Launch result.
    bool terminate_succeeds = true;  ///< Termination result.
    int terminate_fail_on_call = 0;  ///< One-based termination call that fails, or zero.
    bool cleanup_succeeds = true;  ///< Cleanup result.
    bool duplicate_launch_ids = false;  ///< Whether launch returns duplicate resource UUIDs.
    reconciliation_t reconciliation {reconciliation_t::status_t::missing, std::nullopt, std::nullopt};  ///< Restart observation.
    int resolve_calls = 0;  ///< Application resolution calls.
    int capability_calls = 0;  ///< Capability calls.
    int launch_calls = 0;  ///< Provider launch calls.
    int terminate_calls = 0;  ///< Provider termination calls.
    int cleanup_calls = 0;  ///< Provider cleanup calls.
    std::vector<std::string> order;  ///< Callback order.
    std::optional<launch_request_t> launched_request;  ///< Most recent provider launch input.

    /**
     * @brief Build callbacks bound to fixture state.
     * @return Sandbox manager callbacks.
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
        [&]() {
          return now;
        },
        [&]() {
          return ids.at(id_index++);
        },
        [&](const std::string &app, const std::optional<std::string> &profile) -> std::optional<application_t> {
          ++resolve_calls;
          order.push_back("resolve");
          return resolve_succeeds ? std::optional<application_t> {{app, profile, "C:\\safe\\app.exe", nlohmann::json::object()}} : std::nullopt;
        },
        [&](const launch_request_t &, std::string &reason) {
          ++capability_calls;
          order.push_back("capability");
          reason = capable ? "" : "unenforceable";
          return capable;
        },
        [&](const launch_request_t &request, error_t &error) -> std::optional<launch_result_t> {
          ++launch_calls;
          order.push_back("launch");
          launched_request = request;
          if (!launch_succeeds) {
            error = {"isolation_failed", "Isolation could not be established"};
            return std::nullopt;
          }
          return launch_result_t {"runtime-1", SESSION, duplicate_launch_ids ? std::vector<std::string> {DISPLAY, DISPLAY} : std::vector<std::string> {DISPLAY}, {CLAIM}};
        },
        [&](const std::string &, const bool) {
          ++terminate_calls;
          const bool succeeds = terminate_succeeds && terminate_calls != terminate_fail_on_call;
          return termination_t {succeeds, succeeds ? std::optional<int> {0} : std::nullopt, succeeds ? std::nullopt : std::optional<error_t> {{"terminate_failed", "failed"}}};
        },
        [&](const std::string &) {
          return reconciliation;
        },
        [&](const std::string &, const nlohmann::json &) {
          ++cleanup_calls;
          return cleanup_succeeds;
        },
      };
    }
  };

  /**
   * @brief Build minimal valid policy allowing standard application.
   * @return Partial policy configuration.
   */
  nlohmann::json policy() {
    return {{"allowedAppUuids", {APP}}};
  }

  /**
   * @brief Build standard creation request.
   * @param persistent Persistence flag.
   * @return Valid creation request.
   */
  create_t creation(const bool persistent = true) {
    return {PROFILE, std::nullopt, APP, persistent, "Sandbox", policy()};
  }
}  // namespace

TEST(EclipseSandboxesPolicyTest, NormalizesEverySecureDefaultExactly) {
  nlohmann::json effective;
  ASSERT_TRUE(normalize_policy(nlohmann::json::object(), effective));
  EXPECT_EQ(effective["allowedAppUuids"], nlohmann::json::array());
  EXPECT_EQ(effective["executablePolicy"], "configured-only");
  EXPECT_EQ(effective["filesystem"], (nlohmann::json {{"readOnlyRoots", nlohmann::json::array()}, {"writableRoots", nlohmann::json::array()}, {"denyOther", true}}));
  EXPECT_EQ(effective["environment"], (nlohmann::json {{"allowedNames", nlohmann::json::array()}, {"values", nlohmann::json::object()}}));
  EXPECT_EQ(effective["network"], (nlohmann::json {{"mode", "none"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", nlohmann::json::array()}}));
  EXPECT_EQ(effective["resources"], (nlohmann::json {{"cpuPercent", nullptr}, {"memoryBytes", nullptr}, {"processCount", nullptr}, {"storageBytes", nullptr}}));
  EXPECT_EQ(effective["gpu"], (nlohmann::json {{"mode", "none"}, {"encoder", false}}));
  EXPECT_EQ(effective["displays"], (nlohmann::json {{"allowedIds", nlohmann::json::array()}, {"virtualOnly", true}}));
  EXPECT_EQ(effective["input"], (nlohmann::json {{"classes", nlohmann::json::array()}}));
  EXPECT_EQ(effective["peripherals"], (nlohmann::json {{"deviceIds", nlohmann::json::array()}, {"classes", nlohmann::json::array()}}));
  EXPECT_EQ(effective["clipboard"], "none");
  EXPECT_EQ(effective["hostIntegration"], "none");
  EXPECT_EQ(effective["elevation"], "deny");
  EXPECT_EQ(effective["persistentData"], (nlohmann::json {{"enabled", false}, {"name", nullptr}}));
  EXPECT_EQ(effective["timeoutMs"], 0);
  EXPECT_EQ(effective["cleanupPolicy"], "delete");
}

TEST(EclipseSandboxesPolicyTest, AcceptsCompletePolicyAndSecretReferences) {
  auto configuration = policy();
  configuration.update({
    {"executablePolicy", "allowlist"},
    {"filesystem", {{"readOnlyRoots", {"C:\\Windows"}}, {"writableRoots", {"C:\\Data"}}, {"denyOther", true}}},
    {"environment", {{"allowedNames", {"TOKEN", "PLAIN"}}, {"values", {{"TOKEN", {{"secretRef", "vault:item"}}}, {"PLAIN", "value"}}}}},
    {"network", {{"mode", "outbound"}, {"allowedHosts", {"example.test"}}, {"allowedPorts", {443}}}},
    {"resources", {{"cpuPercent", 50}, {"memoryBytes", 1024}, {"processCount", 2}, {"storageBytes", 4096}}},
    {"gpu", {{"mode", "compute"}, {"encoder", false}}},
    {"displays", {{"allowedIds", {DISPLAY}}, {"virtualOnly", true}}},
    {"input", {{"classes", {"gamepad"}}}},
    {"peripherals", {{"deviceIds", {"usb:1"}}, {"classes", {"hid"}}}},
    {"clipboard", "read"},
    {"hostIntegration", "restricted"},
    {"elevation", "allow"},
    {"persistentData", {{"enabled", true}, {"name", "data"}}},
    {"timeoutMs", 60000},
    {"cleanupPolicy", "retain"},
  });
  nlohmann::json effective;
  EXPECT_TRUE(normalize_policy(configuration, effective));
  EXPECT_EQ(effective, configuration);
}

TEST(EclipseSandboxesPolicyTest, RejectsUnknownNestedMalformedAndOutOfRangeValues) {
  nlohmann::json ignored;
  const std::vector<nlohmann::json> invalid {
    nullptr,
    {{"unknown", true}},
    {{"allowedAppUuids", {"BAD"}}},
    {{"allowedAppUuids", {APP, APP}}},
    {{"executablePolicy", "any"}},
    {{"filesystem", {{"readOnlyRoots", nlohmann::json::array()}, {"writableRoots", nlohmann::json::array()}, {"denyOther", true}, {"unknown", true}}}},
    {{"environment", {{"allowedNames", {"TOKEN"}}, {"values", {{"OTHER", "x"}}}}}},
    {{"environment", {{"allowedNames", {"TOKEN"}}, {"values", {{"TOKEN", {{"secretRef", "x"}, {"extra", true}}}}}}}},
    {{"network", {{"mode", "outbound"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", {0}}}}},
    {{"network", {{"mode", "outbound"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", {65536}}}}},
    {{"resources", {{"cpuPercent", 101}, {"memoryBytes", nullptr}, {"processCount", nullptr}, {"storageBytes", nullptr}}}},
    {{"gpu", {{"mode", "device"}, {"encoder", false}}}},
    {{"displays", {{"allowedIds", {"bad"}}, {"virtualOnly", true}}}},
    {{"input", {{"classes", {"keyboard", "keyboard"}}}}},
    {{"clipboard", "all"}},
    {{"hostIntegration", "full"}},
    {{"elevation", "prompt"}},
    {{"persistentData", {{"enabled", true}, {"name", nullptr}}}},
    {{"timeoutMs", -1}},
    {{"cleanupPolicy", "unknown"}},
  };
  for (const auto &value : invalid) {
    EXPECT_FALSE(normalize_policy(value, ignored)) << value.dump();
  }
}

TEST(EclipseSandboxesTest, CreatesListsSerializesAndRollsBackFailedWrite) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_TRUE(manager.available());
  const auto created = manager.create(OWNER, creation());
  ASSERT_EQ(created.status, status_t::success);
  EXPECT_EQ(created.resource->id, SANDBOX);
  EXPECT_EQ(created.resource->state, state_t::created);
  EXPECT_EQ(created.resource->revision, std::uint64_t {1});
  EXPECT_FALSE(to_json(*created.resource).contains("runtimeId"));
  const auto listed = manager.list(0);
  EXPECT_TRUE(listed.changed);
  EXPECT_EQ(listed.resources.size(), std::size_t {1});
  EXPECT_FALSE(manager.list(listed.revision).changed);

  fake.save_succeeds = false;
  EXPECT_EQ(manager.create(OWNER, creation()).status, status_t::persistence_error);
  EXPECT_FALSE(manager.get(SANDBOX_2));
}

TEST(EclipseSandboxesTest, ValidatesCreationAndRejectsUnenforceablePolicy) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  auto request = creation();
  request.profile_id = "BAD";
  EXPECT_EQ(manager.create(OWNER, request).status, status_t::invalid);
  request = creation();
  request.profile_configuration["network"] = {{"mode", "full"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", nlohmann::json::array()}};
  fake.capable = false;
  EXPECT_EQ(manager.create(OWNER, request).status, status_t::unsupported_configuration);
  EXPECT_FALSE(manager.get(SANDBOX));
}

TEST(EclipseSandboxesTest, ResolvesBeforeCapabilityAndLaunchThenStops) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  fake.order.clear();
  fake.now = 2000;
  const auto started = manager.start(SANDBOX, 1);
  ASSERT_EQ(started.status, status_t::success);
  EXPECT_EQ(fake.order, (std::vector<std::string> {"resolve", "capability", "launch"}));
  EXPECT_EQ(started.resource->state, state_t::running);
  EXPECT_EQ(started.resource->session_id, SESSION);
  EXPECT_EQ(started.resource->display_ids, (std::vector<std::string> {DISPLAY}));
  EXPECT_EQ(started.resource->revision, std::uint64_t {3});
  fake.now = 3000;
  const auto stopped = manager.stop(SANDBOX, 3, false);
  ASSERT_EQ(stopped.status, status_t::success);
  EXPECT_EQ(stopped.resource->state, state_t::stopped);
  EXPECT_EQ(stopped.resource->exit_code, 0);
  EXPECT_EQ(stopped.resource->stopped_at, 3000);
  EXPECT_FALSE(stopped.resource->runtime_id);
  EXPECT_EQ(fake.terminate_calls, 1);
  EXPECT_EQ(fake.cleanup_calls, 1);
}

TEST(EclipseSandboxesTest, AppliesValidatedLaunchConfigurationOverride) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  start_t request;
  request.launch_data = {{"arguments", {"--workspace"}}};
  ASSERT_EQ(manager.start(SANDBOX, 1, request).status, status_t::success);
  ASSERT_TRUE(fake.launched_request);
  EXPECT_EQ(fake.launched_request->application.launch_data, *request.launch_data);
}

TEST(EclipseSandboxesTest, FailsBeforeExecutionForResolutionOrCapabilityFailure) {
  fake_t unresolved;
  manager_t unresolved_manager {unresolved.callbacks()};
  ASSERT_EQ(unresolved_manager.create(OWNER, creation()).status, status_t::success);
  unresolved.resolve_succeeds = false;
  EXPECT_EQ(unresolved_manager.start(SANDBOX, 1).status, status_t::application_not_found);
  EXPECT_EQ(unresolved.launch_calls, 0);

  fake_t unsupported;
  manager_t unsupported_manager {unsupported.callbacks()};
  ASSERT_EQ(unsupported_manager.create(OWNER, creation()).status, status_t::success);
  unsupported.capable = false;
  EXPECT_EQ(unsupported_manager.start(SANDBOX, 1).status, status_t::unsupported_configuration);
  EXPECT_EQ(unsupported.launch_calls, 0);
}

TEST(EclipseSandboxesTest, CleansFailedLaunchAndRollsBackSuccessfulLaunchOnWriteFailure) {
  fake_t failed;
  manager_t failed_manager {failed.callbacks()};
  ASSERT_EQ(failed_manager.create(OWNER, creation()).status, status_t::success);
  failed.launch_succeeds = false;
  const auto result = failed_manager.start(SANDBOX, 1);
  ASSERT_EQ(result.status, status_t::provider_error);
  EXPECT_EQ(result.resource->state, state_t::failed);
  EXPECT_EQ(result.resource->error->code, "isolation_failed");

  fake_t write_failed;
  int saves = 0;
  auto callbacks = write_failed.callbacks();
  callbacks.save = [&](const std::string &value) {
    ++saves;
    if (saves == 4) {
      return false;
    }
    write_failed.document = value;
    return true;
  };
  manager_t write_manager {std::move(callbacks)};
  ASSERT_EQ(write_manager.create(OWNER, creation()).status, status_t::success);
  EXPECT_EQ(write_manager.start(SANDBOX, 1).status, status_t::persistence_error);
  EXPECT_EQ(write_failed.terminate_calls, 1);
  EXPECT_EQ(write_failed.cleanup_calls, 1);
  EXPECT_EQ(write_manager.get(SANDBOX)->state, state_t::created);
}

TEST(EclipseSandboxesTest, RejectsAndCleansDuplicateLaunchResourceIds) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  fake.duplicate_launch_ids = true;
  const auto result = manager.start(SANDBOX, 1);
  ASSERT_EQ(result.status, status_t::provider_error);
  EXPECT_EQ(result.resource->state, state_t::failed);
  EXPECT_EQ(fake.terminate_calls, 1);
  EXPECT_EQ(fake.cleanup_calls, 1);
}

TEST(EclipseSandboxesTest, RestartsThroughConfirmedTermination) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  const auto restarted = manager.restart(SANDBOX, 3, true);
  ASSERT_EQ(restarted.status, status_t::success);
  EXPECT_EQ(fake.terminate_calls, 1);
  EXPECT_EQ(fake.launch_calls, 2);
  EXPECT_EQ(restarted.resource->state, state_t::running);
  EXPECT_EQ(restarted.resource->revision, std::uint64_t {7});
}

TEST(EclipseSandboxesTest, RevocationOrphansPersistentDeletesEphemeralAndAllowsAdoption) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation(true)).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, creation(false)).status, status_t::success);
  ASSERT_EQ(manager.revoke_owner(OWNER), status_t::success);
  EXPECT_FALSE(manager.get(SANDBOX_2));
  const auto orphan = manager.get(SANDBOX);
  ASSERT_TRUE(orphan);
  EXPECT_FALSE(orphan->owner_client_uuid);
  const auto adopted = manager.adopt(SANDBOX, orphan->revision, OWNER);
  ASSERT_EQ(adopted.status, status_t::success);
  EXPECT_EQ(adopted.resource->owner_client_uuid, OWNER);
}

TEST(EclipseSandboxesTest, DeletesOnlyAfterTerminationAndCleanup) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  fake.terminate_succeeds = false;
  EXPECT_EQ(manager.remove(SANDBOX, 3).status, status_t::provider_error);
  const auto failed = manager.get(SANDBOX);
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->state, state_t::failed);
  fake.terminate_succeeds = true;
  EXPECT_EQ(manager.remove(SANDBOX, failed->revision).status, status_t::success);
  EXPECT_FALSE(manager.get(SANDBOX));
}

TEST(EclipseSandboxesTest, CleanupFailureClearsDeadRuntimeAndBlocksRelaunch) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  fake.cleanup_succeeds = false;

  const auto stopped = manager.stop(SANDBOX, 3, true);
  ASSERT_EQ(stopped.status, status_t::provider_error);
  ASSERT_TRUE(stopped.resource);
  EXPECT_EQ(stopped.resource->state, state_t::failed);
  EXPECT_EQ(stopped.resource->error->code, "cleanup_failed");
  EXPECT_FALSE(stopped.resource->runtime_id);
  EXPECT_TRUE(stopped.resource->display_ids.empty());
  EXPECT_EQ(manager.start(SANDBOX, stopped.resource->revision).status, status_t::conflict);
  EXPECT_EQ(fake.launch_calls, 1);
}

TEST(EclipseSandboxesTest, DeleteSaveFailureRetainsDeletingIntentAndDisablesManager) {
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
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  const auto removed = manager.remove(SANDBOX, 1);
  EXPECT_EQ(removed.status, status_t::persistence_error);
  EXPECT_FALSE(manager.available());
  ASSERT_TRUE(manager.get(SANDBOX));
  EXPECT_EQ(manager.get(SANDBOX)->state, state_t::deleting);
  EXPECT_EQ(nlohmann::json::parse(*fake.document)["resources"][0]["state"], "deleting");
}

TEST(EclipseSandboxesTest, PartialTwoResourceRevocationPersistsProgressAndFailsClosed) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation(true)).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  ASSERT_EQ(manager.create(OWNER, creation(true)).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX_2, 1).status, status_t::success);
  fake.terminate_fail_on_call = 2;
  EXPECT_EQ(manager.revoke_owner(OWNER), status_t::provider_error);
  EXPECT_FALSE(manager.available());
  const auto first = manager.get(SANDBOX);
  const auto second = manager.get(SANDBOX_2);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(first->state, state_t::stopped);
  EXPECT_FALSE(first->owner_client_uuid);
  EXPECT_EQ(second->state, state_t::failed);
  EXPECT_TRUE(second->runtime_id);
  const auto persisted = nlohmann::json::parse(*fake.document);
  EXPECT_EQ(persisted["resources"][0]["state"], "stopped");
  EXPECT_EQ(persisted["resources"][1]["state"], "failed");
}

TEST(EclipseSandboxesTest, ReconcilesPersistentAndCleansEphemeralOnRestart) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, creation(true)).status, status_t::success);
    ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
    ASSERT_EQ(manager.create(OWNER, creation(false)).status, status_t::success);
    ASSERT_EQ(manager.start(SANDBOX_2, 1).status, status_t::success);
  }
  fake.reconciliation = {reconciliation_t::status_t::exited, 7, std::nullopt};
  manager_t restarted {fake.callbacks()};
  ASSERT_TRUE(restarted.available());
  EXPECT_FALSE(restarted.get(SANDBOX_2));
  const auto persistent = restarted.get(SANDBOX);
  ASSERT_TRUE(persistent);
  EXPECT_EQ(persistent->state, state_t::stopped);
  EXPECT_EQ(persistent->exit_code, 7);
  EXPECT_FALSE(persistent->runtime_id);
  EXPECT_GE(fake.cleanup_calls, 2);
}

TEST(EclipseSandboxesTest, ReconcilesAbnormalExitAndPublishesEveryTransition) {
  fake_t fake;
  std::vector<std::pair<std::optional<state_t>, std::optional<state_t>>> changes;
  auto callbacks = fake.callbacks();
  callbacks.on_change = [&](const std::optional<resource_t> &previous, const std::optional<resource_t> &current) {
    changes.emplace_back(previous ? std::optional {previous->state} : std::nullopt, current ? std::optional {current->state} : std::nullopt);
  };
  manager_t manager {std::move(callbacks)};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  fake.reconciliation = {reconciliation_t::status_t::exited, 7, std::nullopt};
  fake.now = 3000;

  ASSERT_EQ(manager.reconcile(), status_t::success);
  const auto stopped = manager.get(SANDBOX);
  ASSERT_TRUE(stopped);
  EXPECT_EQ(stopped->state, state_t::stopped);
  EXPECT_EQ(stopped->exit_code, 7);
  EXPECT_FALSE(stopped->runtime_id);
  EXPECT_EQ(fake.cleanup_calls, 1);
  EXPECT_EQ(changes, (std::vector<std::pair<std::optional<state_t>, std::optional<state_t>>> {
                       {std::nullopt, state_t::created},
                       {state_t::created, state_t::starting},
                       {state_t::starting, state_t::running},
                       {state_t::running, state_t::stopped},
                     }));
}

TEST(EclipseSandboxesTest, ReconcilePersistenceFailureDisablesManager) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  ASSERT_EQ(manager.start(SANDBOX, 1).status, status_t::success);
  fake.reconciliation = {reconciliation_t::status_t::exited, 7, std::nullopt};
  fake.save_succeeds = false;

  EXPECT_EQ(manager.reconcile(), status_t::persistence_error);
  EXPECT_FALSE(manager.available());
}

TEST(EclipseSandboxesTest, MalformedRecordFailsWholeDocumentClosed) {
  fake_t fake;
  {
    manager_t manager {fake.callbacks()};
    ASSERT_EQ(manager.create(OWNER, creation()).status, status_t::success);
  }
  auto persisted = nlohmann::json::parse(*fake.document);
  persisted["resources"].push_back({{"id", "bad"}});
  fake.document = persisted.dump();
  manager_t recovered {fake.callbacks()};
  EXPECT_FALSE(recovered.available());
  EXPECT_FALSE(recovered.get(SANDBOX));
  EXPECT_EQ(recovered.create(OWNER, creation()).status, status_t::unavailable);
}

TEST(EclipseSandboxesTest, SerializesConcurrentCreation) {
  fake_t fake;
  manager_t manager {fake.callbacks()};
  std::vector<std::future<result_t>> futures;
  for (int index = 0; index < 2; ++index) {
    futures.push_back(std::async(std::launch::async, [&]() {
      return manager.create(OWNER, creation());
    }));
  }
  for (auto &future : futures) {
    EXPECT_EQ(future.get().status, status_t::success);
  }
  EXPECT_EQ(manager.list().resources.size(), std::size_t {2});
}
