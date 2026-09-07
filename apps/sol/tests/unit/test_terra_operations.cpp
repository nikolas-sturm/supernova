/**
 * @file tests/unit/test_terra_operations.cpp
 * @brief Tests for Terra V1 operations and idempotency storage.
 */

// standard includes
#include <atomic>
#include <future>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// local includes
#include <src/terra_operations.h>

namespace {
  constexpr const char *CLIENT = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";  ///< Standard test client UUID.
  constexpr const char *OPERATION = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";  ///< Standard test operation UUID.

  /**
   * @brief In-memory persistent document fixture.
   */
  struct persistence_t {
    std::mutex mutex;  ///< Protects document.
    std::optional<std::string> document;  ///< Last persisted document.
    bool succeeds = true;  ///< Save callback result.

    /**
     * @brief Load current document.
     *
     * @return Stored document.
     */
    std::optional<std::string> load() {
      std::scoped_lock lock {mutex};
      return document;
    }

    /**
     * @brief Save document when enabled.
     *
     * @param value Complete document.
     * @return Configured success value.
     */
    bool save(const std::string &value) {
      std::scoped_lock lock {mutex};
      if (succeeds) {
        document = value;
      }
      return succeeds;
    }
  };

  /**
   * @brief Build callbacks backed by fixture values.
   *
   * @param persistence Persistence fixture.
   * @param now Mutable test clock.
   * @param ids UUID sequence.
   * @return Store callbacks.
   */
  terra_operations::callbacks_t callbacks(persistence_t &persistence, std::int64_t &now, std::vector<std::string> ids = {OPERATION}) {
    auto index = std::make_shared<std::atomic_size_t>(0);
    auto shared_ids = std::make_shared<std::vector<std::string>>(std::move(ids));
    return {
      [&]() {
        return persistence.load();
      },
      [&](const std::string &value) {
        return persistence.save(value);
      },
      [&]() {
        return now;
      },
      [index, shared_ids]() {
        return shared_ids->at(index->fetch_add(1));
      },
    };
  }

  /**
   * @brief Build standard stored response.
   *
   * @param operation New operation.
   * @return Accepted response.
   */
  nlohmann::json response(const terra_operations::operation_t &operation) {
    return {{"status", 202}, {"operation", terra_operations::to_json(operation)}};
  }
}  // namespace

TEST(TerraOperationsTest, CreatesReplaysAndConflictsByCanonicalBody) {
  persistence_t persistence;
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now)};
  const auto first = store.submit(CLIENT, "launch", "key", nlohmann::json {{"b", 2}, {"a", 1}}, response);
  const auto replay = store.submit(CLIENT, "launch", "key", nlohmann::json {{"a", 1}, {"b", 2}}, response);
  const auto conflict = store.submit(CLIENT, "launch", "key", nlohmann::json {{"a", 2}}, response);

  ASSERT_EQ(first.status, terra_operations::submission_status_t::created);
  ASSERT_EQ(replay.status, terra_operations::submission_status_t::replayed);
  EXPECT_EQ(first.operation->id, OPERATION);
  EXPECT_EQ(first.operation->client_uuid, CLIENT);
  EXPECT_EQ(first.operation->action, "launch");
  EXPECT_EQ(replay.operation->id, OPERATION);
  EXPECT_EQ(replay.response, first.response);
  EXPECT_EQ(conflict.status, terra_operations::submission_status_t::conflict);
  EXPECT_FALSE(conflict.operation);
}

TEST(TerraOperationsTest, ScopesKeysByClientAndAction) {
  persistence_t persistence;
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now, {OPERATION, "cccccccc-cccc-cccc-cccc-cccccccccccc", "dddddddd-dddd-dddd-dddd-dddddddddddd"})};

  EXPECT_EQ(store.submit(CLIENT, "launch", "key", nlohmann::json::object(), response).status, terra_operations::submission_status_t::created);
  EXPECT_EQ(store.submit(CLIENT, "stop", "key", nlohmann::json::object(), response).status, terra_operations::submission_status_t::created);
  EXPECT_EQ(store.submit("eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee", "launch", "key", nlohmann::json::object(), response).status, terra_operations::submission_status_t::created);
}

TEST(TerraOperationsTest, EnforcesLifecycleAndMonotonicRevision) {
  persistence_t persistence;
  std::int64_t now = 1000;
  std::vector<terra_operations::state_t> updates;
  auto configured_callbacks = callbacks(persistence, now);
  configured_callbacks.updated = [&](const terra_operations::operation_t &operation) {
    updates.push_back(operation.state);
  };
  terra_operations::store_t store {std::move(configured_callbacks)};
  const auto created = store.submit(CLIENT, "launch", "key", nullptr, response);
  EXPECT_EQ(created.operation->revision, 1);
  now = 900;
  const auto running = store.transition(OPERATION, terra_operations::state_t::running);
  ASSERT_TRUE(running);
  EXPECT_EQ(running->revision, 2);
  EXPECT_EQ(running->updated_at, 1000);
  EXPECT_FALSE(store.transition(OPERATION, terra_operations::state_t::running));
  now = 1100;
  const auto succeeded = store.transition(OPERATION, terra_operations::state_t::succeeded, "resource", nlohmann::json {{"ok", true}});
  ASSERT_TRUE(succeeded);
  EXPECT_EQ(succeeded->revision, 3);
  EXPECT_EQ(succeeded->resource_id, "resource");
  EXPECT_TRUE(succeeded->error.is_null());
  EXPECT_FALSE(store.transition(OPERATION, terra_operations::state_t::failed, {}, nullptr, {"code", "late"}));
  EXPECT_EQ(updates, (std::vector {terra_operations::state_t::running, terra_operations::state_t::succeeded}));
}

TEST(TerraOperationsTest, RollsBackRejectedPersistence) {
  persistence_t persistence;
  std::int64_t now = 1000;
  persistence.succeeds = false;
  terra_operations::store_t store {callbacks(persistence, now)};
  EXPECT_EQ(store.submit(CLIENT, "launch", "key", nullptr, response).status, terra_operations::submission_status_t::persistence_failed);
  EXPECT_FALSE(store.get(OPERATION));
}

TEST(TerraOperationsTest, ReloadsValidRecordsAndIgnoresMalformedRecords) {
  persistence_t persistence;
  std::int64_t now = 1000;
  {
    terra_operations::store_t store {callbacks(persistence, now)};
    ASSERT_EQ(store.submit(CLIENT, "launch", "key", nullptr, response).status, terra_operations::submission_status_t::created);
  }
  auto document = nlohmann::json::parse(*persistence.document);
  document["operations"].push_back({{"id", "invalid"}});
  document["idempotency"].push_back({{"scope", "bad"}, {"bodyHash", "bad"}, {"operationId", "missing"}, {"response", nullptr}});
  persistence.document = document.dump();

  terra_operations::store_t reloaded {callbacks(persistence, now)};
  const auto operation = reloaded.get(OPERATION);
  ASSERT_TRUE(operation);
  EXPECT_EQ(operation->client_uuid, CLIENT);
  EXPECT_EQ(operation->action, "launch");
  EXPECT_EQ(reloaded.submit(CLIENT, "launch", "key", nullptr, response).status, terra_operations::submission_status_t::replayed);
}

TEST(TerraOperationsTest, FailsInterruptedOperationsDuringRestartRecovery) {
  persistence_t persistence;
  std::int64_t now = 1000;
  constexpr auto running_id = "cccccccc-cccc-cccc-cccc-cccccccccccc";
  {
    terra_operations::store_t store {callbacks(persistence, now, {OPERATION, running_id})};
    ASSERT_EQ(store.submit(CLIENT, "pending-action", "pending-key", nullptr, response).status, terra_operations::submission_status_t::created);
    ASSERT_EQ(store.submit(CLIENT, "running-action", "running-key", nullptr, response).status, terra_operations::submission_status_t::created);
    ASSERT_TRUE(store.transition(running_id, terra_operations::state_t::running));
  }

  now = 2000;
  terra_operations::store_t recovered {callbacks(persistence, now)};
  const auto pending = recovered.get(OPERATION);
  const auto running = recovered.get(running_id);
  ASSERT_TRUE(pending);
  ASSERT_TRUE(running);
  EXPECT_EQ(pending->state, terra_operations::state_t::failed);
  EXPECT_EQ(running->state, terra_operations::state_t::failed);
  EXPECT_EQ(pending->revision, 2);
  EXPECT_EQ(running->revision, 3);
  EXPECT_EQ(pending->updated_at, now);
  EXPECT_EQ(running->updated_at, now);
  EXPECT_EQ(pending->error["code"], "host_restarted");
  EXPECT_EQ(running->error["code"], "host_restarted");

  const auto persisted = nlohmann::json::parse(*persistence.document);
  EXPECT_EQ(persisted["operations"][0]["state"], "failed");
  EXPECT_EQ(persisted["operations"][1]["state"], "failed");
}

TEST(TerraOperationsTest, RejectsInvalidSubmissionIdentityAndEmptyKey) {
  persistence_t persistence;
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now, {OPERATION, OPERATION, OPERATION})};
  EXPECT_EQ(store.submit("invalid", "launch", "key", nullptr, response).status, terra_operations::submission_status_t::persistence_failed);
  EXPECT_EQ(store.submit(CLIENT, "", "key", nullptr, response).status, terra_operations::submission_status_t::persistence_failed);
  EXPECT_EQ(store.submit(CLIENT, "launch", "", nullptr, response).status, terra_operations::submission_status_t::persistence_failed);
}

TEST(TerraOperationsTest, RejectsUnknownDocumentVersionFailClosed) {
  persistence_t persistence;
  persistence.document = R"({"version":99,"operations":[],"idempotency":[]})";
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now)};
  EXPECT_FALSE(store.get(OPERATION));
}

TEST(TerraOperationsTest, IgnoresSemanticallyMalformedOperation) {
  persistence_t persistence;
  persistence.document = R"({"version":1,"operations":[{"id":"bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb","clientUuid":"aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa","action":"launch","state":"pending","resourceId":null,"createdAt":1,"updatedAt":1,"revision":1,"result":{"unexpected":true},"error":null}],"idempotency":[]})";
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now)};
  EXPECT_FALSE(store.get(OPERATION));
}

TEST(TerraOperationsTest, PrunesTerminalOperationAndIdempotencyAtMinimumRetention) {
  persistence_t persistence;
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now), 1};
  ASSERT_EQ(store.submit(CLIENT, "launch", "key", nullptr, response).status, terra_operations::submission_status_t::created);
  ASSERT_TRUE(store.transition(OPERATION, terra_operations::state_t::running));
  ASSERT_TRUE(store.transition(OPERATION, terra_operations::state_t::failed, {}, nullptr, nlohmann::json {{"code", "failed"}}));
  now += terra_operations::MIN_TERMINAL_RETENTION_MS - 1;
  EXPECT_EQ(store.prune(), 0);
  now += 1;
  EXPECT_EQ(store.prune(), 1);
  EXPECT_FALSE(store.get(OPERATION));
}

TEST(TerraOperationsTest, ConcurrentEquivalentSubmissionsCreateOnce) {
  persistence_t persistence;
  std::int64_t now = 1000;
  terra_operations::store_t store {callbacks(persistence, now)};
  std::vector<std::future<terra_operations::submission_t>> futures;
  for (int index = 0; index < 8; ++index) {
    futures.push_back(std::async(std::launch::async, [&]() {
      return store.submit(CLIENT, "launch", "key", nlohmann::json {{"x", 1}}, response);
    }));
  }
  int created = 0;
  for (auto &future : futures) {
    const auto result = future.get();
    created += result.status == terra_operations::submission_status_t::created;
    EXPECT_EQ(result.operation->id, OPERATION);
  }
  EXPECT_EQ(created, 1);
}
