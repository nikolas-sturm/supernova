/**
 * @file tests/unit/test_eclipse_sandbox_https.cpp
 * @brief Direct HTTPS route validation for the Eclipse sandbox API.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <atomic>
  #include <chrono>
  #include <cstdio>
  #include <filesystem>
  #include <format>
  #include <fstream>
  #include <sstream>
  #include <string>
  #include <thread>

  // lib includes
  #include <nlohmann/json.hpp>
  #include <Simple-Web-Server/client_https.hpp>

  // local includes
  #include <src/config.h>
  #include <src/nvhttp.h>

namespace fs = std::filesystem;

namespace {
  /**
   * @brief Isolated paired-client HTTPS fixture exercising production sandbox routes.
   */
  class EclipseSandboxHttpsTest: public BaseTest {
  public:
    /**
     * @brief One parsed HTTPS response.
     */
    struct response_t {
      std::string status;  ///< HTTP status text, for example `200 OK`.
      nlohmann::json body;  ///< Parsed JSON body, null when not JSON.
    };

  protected:
    /**
     * @brief Start an isolated production nvhttp server with paired test clients.
     */
    void SetUp() override {
      BaseTest::SetUp();
      original_state_file = config::nvhttp.file_state;
      original_cert = config::nvhttp.cert;
      original_key = config::nvhttp.pkey;
      original_fresh_state = config::sunshine.flags[config::flag::FRESH_STATE];

      fixture_dir = fs::path {SUNSHINE_TEST_BIN_DIR} / "eclipse_sandbox_https";
      std::error_code error;
      fs::create_directories(fixture_dir, error);
      state_file = fixture_dir / "client_state.json";
      host_cert = fixture_dir / "host_cert.pem";
      host_key = fixture_dir / "host_key.pem";
      scoped_cert = fixture_dir / "scoped_cert.pem";
      scoped_key = fixture_dir / "scoped_key.pem";
      unscoped_cert = fixture_dir / "unscoped_cert.pem";
      unscoped_key = fixture_dir / "unscoped_key.pem";
      unknown_cert = fixture_dir / "unknown_cert.pem";
      unknown_key = fixture_dir / "unknown_key.pem";

      config::nvhttp.file_state = state_file.string();
      config::nvhttp.cert = host_cert.string();
      config::nvhttp.pkey = host_key.string();
      config::sunshine.flags[config::flag::FRESH_STATE] = false;

      const auto host = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS Test Host");
      write_pem(host_cert, host.x509);
      write_pem(host_key, host.pkey);

      nvhttp::test_support::reset_client_state();
      fs::remove(state_file, error);

      const auto scoped = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS Scoped Client");
      scoped_uuid = nvhttp::test_support::add_client("sandbox-scoped", scoped.x509, true);
      ASSERT_FALSE(scoped_uuid.empty());
      eclipse_api::client_permissions_t permissions;
      permissions.expires_at = 4102444800;
      permissions.scopes.emplace("catalog.read");
      permissions.scopes.emplace("sandbox.manage");
      ASSERT_TRUE(nvhttp::set_client_permissions(scoped_uuid, permissions));
      write_pem(scoped_cert, scoped.x509);
      write_pem(scoped_key, scoped.pkey);

      const auto unscoped = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS Unscoped Client");
      unscoped_uuid = nvhttp::test_support::add_client("sandbox-unscoped", unscoped.x509, true);
      ASSERT_FALSE(unscoped_uuid.empty());
      eclipse_api::client_permissions_t restricted;
      restricted.expires_at = 4102444800;
      ASSERT_TRUE(nvhttp::set_client_permissions(unscoped_uuid, restricted));
      write_pem(unscoped_cert, unscoped.x509);
      write_pem(unscoped_key, unscoped.pkey);

      const auto unknown = test_utils::certificates::generate_ca_credentials("Sunshine HTTPS Unknown Client");
      write_pem(unknown_cert, unknown.x509);
      write_pem(unknown_key, unknown.pkey);

      server_thread = std::jthread([this]() {
        nvhttp::test_support::run_servers(0, 0, [this](const unsigned short assigned) {
          port = assigned;
        },
                                          stop_flag);
      });
      for (int attempt = 0; attempt < 500 && port == 0; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
      }
      ASSERT_NE(port, 0) << "HTTPS server failed to start";

      scoped_client = make_client(scoped_cert, scoped_key);
      unscoped_client = make_client(unscoped_cert, unscoped_key);
      unknown_client = make_client(unknown_cert, unknown_key);
    }

    /**
     * @brief Stop the server, stop the task pool, and restore configuration.
     */
    void TearDown() override {
      if (server_thread.joinable()) {
        stop_flag = true;
        server_thread.join();
      }
      scoped_client.reset();
      unscoped_client.reset();
      unknown_client.reset();
      nvhttp::test_support::reset_client_state();
      std::error_code error;
      fs::remove(state_file, error);

      config::nvhttp.file_state = original_state_file;
      config::nvhttp.cert = original_cert;
      config::nvhttp.pkey = original_key;
      config::sunshine.flags[config::flag::FRESH_STATE] = original_fresh_state;
      BaseTest::TearDown();
    }

    /**
     * @brief Send one request through a test client and parse the JSON body.
     * @param client Configured HTTPS client.
     * @param method HTTP method.
     * @param path Request path with optional query string.
     * @param content Optional request body.
     * @param headers Optional request headers.
     * @return Status and parsed body.
     */
    static response_t send(SimpleWeb::Client<SimpleWeb::HTTPS> &client, const std::string &method, const std::string &path, const std::string &content = {}, const SimpleWeb::CaseInsensitiveMultimap &headers = {}) {
      auto response = client.request(method, path, content, headers);
      std::ostringstream body_stream;
      body_stream << response->content.rdbuf();
      return {response->status_code, nlohmann::json::parse(body_stream.str(), nullptr, false)};
    }

    /**
     * @brief Poll one operation URL until it reaches a terminal state.
     * @param operation_url Operation resource path.
     * @return Completed operation response; asserts failure on timeout.
     */
    response_t await_operation(const std::string &operation_url) {
      for (int attempt = 0; attempt < 1000; ++attempt) {
        auto response = scoped_client->request("GET", operation_url);
        std::ostringstream body_stream;
        body_stream << response->content.rdbuf();
        const auto body = nlohmann::json::parse(body_stream.str(), nullptr, false);
        if (response->status_code == "200 OK" && (body["operation"]["state"] == "succeeded" || body["operation"]["state"] == "failed")) {
          EXPECT_EQ(body["operation"]["state"], "succeeded") << body.dump();
          return {response->status_code, body};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds {10});
      }
      ADD_FAILURE() << "Operation did not complete in time";
      return {"503 Service Unavailable", {}};
    }

    /**
     * @brief Build the complete provider-supported relaxed sandbox policy.
     * @return Full sandbox-profile configuration accepted by the native provider.
     */
    static nlohmann::json relaxed_sandbox_configuration() {
      return {
        {"allowedAppUuids", nlohmann::json::array()},
        {"executablePolicy", "configured-only"},
        {"filesystem", {{"readOnlyRoots", nlohmann::json::array()}, {"writableRoots", nlohmann::json::array()}, {"denyOther", false}}},
        {"environment", {{"allowedNames", nlohmann::json::array()}, {"values", nlohmann::json::object()}}},
        {"network", {{"mode", "full"}, {"allowedHosts", nlohmann::json::array()}, {"allowedPorts", nlohmann::json::array()}}},
        {"resources", {{"cpuPercent", nullptr}, {"memoryBytes", nullptr}, {"processCount", nullptr}, {"storageBytes", nullptr}}},
        {"gpu", {{"mode", "full"}, {"encoder", true}}},
        {"displays", {{"allowedIds", nlohmann::json::array()}, {"virtualOnly", false}}},
        {"input", {{"classes", nlohmann::json::array()}}},
        {"peripherals", {{"deviceIds", nlohmann::json::array()}, {"classes", nlohmann::json::array()}}},
        {"clipboard", "bidirectional"},
        {"hostIntegration", "none"},
        {"elevation", "deny"},
        {"persistentData", {{"enabled", false}, {"name", nullptr}}},
        {"timeoutMs", 0},
        {"cleanupPolicy", "delete"},
      };
    }

    /**
     * @brief Create one sandbox profile and wait for the durable operation.
     * @param name Unique profile name.
     * @return Completed operation response.
     */
    response_t create_sandbox_profile(const std::string &name) {
      const nlohmann::json body {
        {"schemaVersion", 1},
        {"type", "sandbox"},
        {"name", name},
        {"shared", false},
        {"configuration", relaxed_sandbox_configuration()},
      };
      SimpleWeb::CaseInsensitiveMultimap headers;
      headers.emplace("Content-Type", "application/json");
      headers.emplace("Idempotency-Key", std::format("profile-{}", name));
      const auto submitted = send(*scoped_client, "POST", "/eclipse/v1/profiles", body.dump(), headers);
      EXPECT_EQ(submitted.status, "202 Accepted") << submitted.body.dump();
      return await_operation(submitted.body["operationUrl"].get<std::string>());
    }

    /**
     * @brief Build a unique idempotency key so repeated runs never replay stale operations.
     * @param action Stable action name.
     * @return Idempotency key unique to this run.
     */
    static std::string unique_key(const std::string_view action) {
      return std::format("{}-{}", action, std::chrono::steady_clock::now().time_since_epoch().count());
    }

    std::filesystem::path fixture_dir;  ///< Isolated fixture directory.
    std::filesystem::path state_file;  ///< Redirected paired-client state file.
    std::filesystem::path host_cert;  ///< Server certificate file.
    std::filesystem::path host_key;  ///< Server private key file.
    std::filesystem::path scoped_cert;  ///< Scoped client certificate file.
    std::filesystem::path scoped_key;  ///< Scoped client key file.
    std::filesystem::path unscoped_cert;  ///< Unscoped client certificate file.
    std::filesystem::path unscoped_key;  ///< Unscoped client key file.
    std::filesystem::path unknown_cert;  ///< Unpaired client certificate file.
    std::filesystem::path unknown_key;  ///< Unpaired client key file.
    std::string scoped_uuid;  ///< Paired client with sandbox.manage.
    std::string unscoped_uuid;  ///< Paired client without sandbox.manage.
    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> scoped_client;  ///< Client for scoped requests.
    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> unscoped_client;  ///< Client for scope-denied requests.
    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> unknown_client;  ///< Client with unpaired certificate.
    std::jthread server_thread;  ///< Production server thread.
    std::atomic_bool stop_flag {false};  ///< Cooperative server shutdown flag.
    unsigned short port = 0;  ///< Bound HTTPS port.
    std::string original_state_file;  ///< State-file setting restored after each test.
    std::string original_cert;  ///< Certificate setting restored after each test.
    std::string original_key;  ///< Key setting restored after each test.
    bool original_fresh_state = false;  ///< Fresh-state flag restored after each test.

  private:
    /**
     * @brief Write one PEM file into the fixture directory.
     * @param path Destination path.
     * @param pem PEM text.
     */
    static void write_pem(const std::filesystem::path &path, const std::string &pem) {
      std::ofstream stream {path, std::ios::binary};
      ASSERT_TRUE(stream.is_open());
      stream << pem;
    }

    /**
     * @brief Build one HTTPS client presenting a client certificate.
     * @param certificate Client certificate file.
     * @param key Client private key file.
     * @return Configured client.
     */
    std::unique_ptr<SimpleWeb::Client<SimpleWeb::HTTPS>> make_client(const std::filesystem::path &certificate, const std::filesystem::path &key) {
      auto client = std::make_unique<SimpleWeb::Client<SimpleWeb::HTTPS>>(std::format("localhost:{}", port), false, certificate.string(), key.string());
      client->config.timeout = 10;
      return client;
    }
  };

  TEST_F(EclipseSandboxHttpsTest, RejectsClientWithoutCertificateAtHandshake) {
    SimpleWeb::Client<SimpleWeb::HTTPS> anonymous {std::format("localhost:{}", port), false};
    anonymous.config.timeout = 10;
    bool rejected = false;
    try {
      const auto response = anonymous.request("GET", "/eclipse/v1/sandboxes");
      rejected = response->status_code == "401 Unauthorized";
    } catch (const std::exception &) {
      rejected = true;
    }
    EXPECT_TRUE(rejected);
  }

  TEST_F(EclipseSandboxHttpsTest, RejectsUnpairedClientCertificate) {
    auto response = unknown_client->request("GET", "/eclipse/v1/sandboxes");
    std::ostringstream body_stream;
    body_stream << response->content.rdbuf();
    EXPECT_EQ(response->status_code, "200 OK");
    EXPECT_NE(body_stream.str().find("status_code=\"401\""), std::string::npos);
    EXPECT_NE(body_stream.str().find("not authorized"), std::string::npos);
  }

  TEST_F(EclipseSandboxHttpsTest, RejectsMissingSandboxScope) {
    const auto response = send(*unscoped_client, "GET", "/eclipse/v1/sandboxes");
    EXPECT_EQ(response.status, "403 Forbidden");
    EXPECT_EQ(response.body["error"]["code"], "permission_denied");
  }

  TEST_F(EclipseSandboxHttpsTest, ListsEmptySandboxCollection) {
    const auto response = send(*scoped_client, "GET", "/eclipse/v1/sandboxes");
    EXPECT_EQ(response.status, "200 OK");
    EXPECT_TRUE(response.body["sandboxes"].is_array());
    EXPECT_TRUE(response.body["changed"]);
    EXPECT_TRUE(response.body["fullSnapshot"]);
  }

  TEST_F(EclipseSandboxHttpsTest, RejectsMalformedSinceParameter) {
    const auto response = send(*scoped_client, "GET", "/eclipse/v1/sandboxes?since=abc");
    EXPECT_EQ(response.status, "400 Bad Request");
    EXPECT_EQ(response.body["error"]["code"], "invalid_argument");
  }

  TEST_F(EclipseSandboxHttpsTest, RejectsUnknownSandboxResource) {
    const auto response = send(*scoped_client, "GET", "/eclipse/v1/sandboxes/99999999-9999-9999-9999-999999999999");
    EXPECT_EQ(response.status, "404 Not Found");
    EXPECT_EQ(response.body["error"]["code"], "sandbox_not_found");
  }

  TEST_F(EclipseSandboxHttpsTest, CreateRequiresIdempotencyKey) {
    const auto profile_operation = create_sandbox_profile(std::format("HttpsCreateKey-{}", unique_key("")));
    const auto profile_id = profile_operation.body["operation"]["result"]["profile"]["id"].get<std::string>();
    const nlohmann::json body {
      {"schemaVersion", 1},
      {"profileId", profile_id},
      {"workspaceId", nullptr},
      {"appUuid", nullptr},
      {"persistent", false},
      {"name", "NoKey"},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    const auto response = send(*scoped_client, "POST", "/eclipse/v1/sandboxes", body.dump(), headers);
    EXPECT_EQ(response.status, "400 Bad Request");
    EXPECT_EQ(response.body["error"]["code"], "idempotency_key_required");
  }

  TEST_F(EclipseSandboxHttpsTest, CreateRejectsUnknownProfileAndWorkspaceAssociation) {
    const nlohmann::json unknown_profile {
      {"schemaVersion", 1},
      {"profileId", "99999999-9999-9999-9999-999999999999"},
      {"workspaceId", nullptr},
      {"appUuid", nullptr},
      {"persistent", false},
      {"name", "UnknownProfile"},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Idempotency-Key", unique_key("sandbox-unknown-profile"));
    auto response = send(*scoped_client, "POST", "/eclipse/v1/sandboxes", unknown_profile.dump(), headers);
    EXPECT_EQ(response.status, "404 Not Found");
    EXPECT_EQ(response.body["error"]["code"], "resource_not_found");

    const auto profile_operation = create_sandbox_profile(std::format("HttpsUnknownAssociation-{}", unique_key("")));
    const auto profile_id = profile_operation.body["operation"]["result"]["profile"]["id"].get<std::string>();
    const nlohmann::json workspace_bound {
      {"schemaVersion", 1},
      {"profileId", profile_id},
      {"workspaceId", "88888888-8888-8888-8888-888888888888"},
      {"appUuid", nullptr},
      {"persistent", false},
      {"name", "WorkspaceBound"},
    };
    headers.erase("Idempotency-Key");
    headers.emplace("Idempotency-Key", unique_key("sandbox-workspace-bound"));
    response = send(*scoped_client, "POST", "/eclipse/v1/sandboxes", workspace_bound.dump(), headers);
    EXPECT_EQ(response.status, "422 Unprocessable Entity");
    EXPECT_EQ(response.body["error"]["code"], "unsupported_configuration");
  }

  TEST_F(EclipseSandboxHttpsTest, DeleteRequiresStrongRevisionPrecondition) {
    const auto profile_operation = create_sandbox_profile(std::format("HttpsDeletePrecondition-{}", unique_key("")));
    const auto profile_id = profile_operation.body["operation"]["result"]["profile"]["id"].get<std::string>();
    const nlohmann::json body {
      {"schemaVersion", 1},
      {"profileId", profile_id},
      {"workspaceId", nullptr},
      {"appUuid", nullptr},
      {"persistent", false},
      {"name", "Precondition"},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Idempotency-Key", unique_key("sandbox-precondition"));
    const auto submitted = send(*scoped_client, "POST", "/eclipse/v1/sandboxes", body.dump(), headers);
    ASSERT_EQ(submitted.status, "202 Accepted");
    const auto operation = await_operation(submitted.body["operationUrl"].get<std::string>());
    const auto sandbox_id = operation.body["operation"]["result"]["sandbox"]["id"].get<std::string>();

    const auto missing = send(*scoped_client, "DELETE", std::format("/eclipse/v1/sandboxes/{}", sandbox_id));
    EXPECT_EQ(missing.status, "428 Precondition Required");
    EXPECT_EQ(missing.body["error"]["code"], "precondition_required");
  }

  TEST_F(EclipseSandboxHttpsTest, LifecycleRoundTripThroughDurableOperations) {
    const auto profile_operation = create_sandbox_profile(std::format("HttpsLifecycle-{}", unique_key("")));
    ASSERT_EQ(profile_operation.body["operation"]["state"], "succeeded");
    const auto profile_id = profile_operation.body["operation"]["result"]["profile"]["id"].get<std::string>();

    const nlohmann::json body {
      {"schemaVersion", 1},
      {"profileId", profile_id},
      {"workspaceId", nullptr},
      {"appUuid", nullptr},
      {"persistent", false},
      {"name", "Lifecycle"},
    };
    SimpleWeb::CaseInsensitiveMultimap headers;
    headers.emplace("Content-Type", "application/json");
    headers.emplace("Idempotency-Key", unique_key("sandbox-lifecycle-create"));
    const auto submitted = send(*scoped_client, "POST", "/eclipse/v1/sandboxes", body.dump(), headers);
    ASSERT_EQ(submitted.status, "202 Accepted");
    EXPECT_EQ(submitted.body["operation"]["state"], "pending");
    const auto operation = await_operation(submitted.body["operationUrl"].get<std::string>());
    const auto sandbox_id = operation.body["operation"]["result"]["sandbox"]["id"].get<std::string>();
    EXPECT_EQ(operation.body["operation"]["resourceId"], sandbox_id);

    const auto fetched = send(*scoped_client, "GET", std::format("/eclipse/v1/sandboxes/{}", sandbox_id));
    ASSERT_EQ(fetched.status, "200 OK");
    EXPECT_EQ(fetched.body["sandbox"]["id"], sandbox_id);
    EXPECT_EQ(fetched.body["sandbox"]["state"], "created");
    EXPECT_EQ(fetched.body["sandbox"]["revision"], 1);
    EXPECT_TRUE(fetched.body["sandbox"]["workspaceId"].is_null());

    const auto listed = send(*scoped_client, "GET", "/eclipse/v1/sandboxes");
    ASSERT_EQ(listed.status, "200 OK");
    bool visible = false;
    for (const auto &sandbox : listed.body["sandboxes"]) {
      visible = visible || sandbox["id"] == sandbox_id;
    }
    EXPECT_TRUE(visible);

    headers.erase("Idempotency-Key");
    headers.emplace("Idempotency-Key", unique_key("sandbox-lifecycle-delete"));
    headers.emplace("If-Match", "\"1\"");
    const auto deleted = send(*scoped_client, "DELETE", std::format("/eclipse/v1/sandboxes/{}", sandbox_id), {}, headers);
    ASSERT_EQ(deleted.status, "202 Accepted");
    const auto delete_operation = await_operation(deleted.body["operationUrl"].get<std::string>());
    ASSERT_EQ(delete_operation.body["operation"]["state"], "succeeded");

    const auto gone = send(*scoped_client, "GET", std::format("/eclipse/v1/sandboxes/{}", sandbox_id));
    EXPECT_EQ(gone.status, "404 Not Found");
    EXPECT_EQ(gone.body["error"]["code"], "sandbox_not_found");
  }
}  // namespace

#else
TEST(EclipseSandboxHttpsTest, WindowsOnly) {
  GTEST_SKIP() << "Eclipse sandbox routes are Windows-specific";
}
#endif
