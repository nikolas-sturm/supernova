/**
 * @file tests/unit/test_nvhttp_client_auth.cpp
 * @brief Test exact paired-client certificate authorization and persistence.
 */

#include "../certificate_test_utils.h"
#include "../tests_common.h"

// standard includes
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

// local includes
#include <src/config.h>
#include <src/nvhttp.h>

namespace fs = std::filesystem;

/**
 * @brief Isolate paired-client authorization tests from the user's Sol state.
 */
class ClientAuthorizationTest: public BaseTest {
protected:
  /**
   * @brief Redirect persisted client state to the test build directory.
   */
  void SetUp() override {
    BaseTest::SetUp();
    original_state_file = config::nvhttp.file_state;
    original_fresh_state = config::sol.flags[config::flag::FRESH_STATE];
    state_file = fs::path {SOL_TEST_BIN_DIR} / "client_authorization_state.json";

    config::nvhttp.file_state = state_file.string();
    config::sol.flags[config::flag::FRESH_STATE] = false;
    nvhttp::test_support::reset_client_state();

    std::error_code error;
    fs::remove(state_file, error);
  }

  /**
   * @brief Remove test state and restore the caller's configuration.
   */
  void TearDown() override {
    nvhttp::test_support::reset_client_state();
    std::error_code error;
    fs::remove(state_file, error);

    config::nvhttp.file_state = original_state_file;
    config::sol.flags[config::flag::FRESH_STATE] = original_fresh_state;
    BaseTest::TearDown();
  }

  fs::path state_file;  ///< Task-specific persisted state fixture.
  std::string original_state_file;  ///< State-file setting restored after each test.
  bool original_fresh_state;  ///< Fresh-state flag restored after each test.
};

TEST_F(ClientAuthorizationTest, CanonicalIdentityFailsClosedAndTracksEnableState) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto crlf_certificate = test_utils::certificates::to_crlf_pem(paired_credentials.x509);
  const auto unknown_credentials = crypto::gen_creds("Sol Unknown Client", 2048);
  const auto uuid = nvhttp::test_support::add_client("paired", crlf_certificate, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), paired_credentials.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(crlf_certificate));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(unknown_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate("not a certificate"));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DerivedLeafDoesNotInheritPairedAuthorization) {
  const auto paired_credentials = test_utils::certificates::generate_ca_credentials();
  const auto derived_credentials = test_utils::certificates::generate_derived_leaf(paired_credentials);
  const auto uuid = nvhttp::test_support::add_client("paired issuer", paired_credentials.x509, true);

  ASSERT_FALSE(uuid.empty());
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(paired_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(derived_credentials.x509));
}

TEST_F(ClientAuthorizationTest, MultipleClientsPersistAndUnpairIndependently) {
  const auto enabled_credentials = test_utils::certificates::generate_ca_credentials("Sol Enabled Client");
  const auto disabled_credentials = crypto::gen_creds("Sol Disabled Client", 2048);
  const auto expired_credentials = test_utils::certificates::expire_credentials(
    test_utils::certificates::generate_ca_credentials("Sol Expired Client")
  );

  const auto enabled_uuid = nvhttp::test_support::add_client("enabled", enabled_credentials.x509, true);
  const auto disabled_uuid = nvhttp::test_support::add_client("disabled", disabled_credentials.x509, false);
  const auto expired_uuid = nvhttp::test_support::add_client("expired", expired_credentials.x509, true);
  ASSERT_FALSE(enabled_uuid.empty());
  ASSERT_FALSE(disabled_uuid.empty());
  ASSERT_FALSE(expired_uuid.empty());

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  nvhttp::test_support::reset_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  nvhttp::test_support::reload_client_state();

  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));

  ASSERT_TRUE(nvhttp::set_client_enabled(disabled_uuid, true));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  ASSERT_TRUE(nvhttp::unpair_client(enabled_uuid));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(enabled_credentials.x509));

  nvhttp::erase_all_clients();
  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(disabled_credentials.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(expired_credentials.x509));
}

TEST_F(ClientAuthorizationTest, DuplicateCertificateIdentityFailsClosed) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  ASSERT_FALSE(nvhttp::test_support::add_client("first", credentials.x509, true).empty());
  ASSERT_FALSE(nvhttp::test_support::add_client("second", credentials.x509, true).empty());

  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, ConcurrentStateChangesRemainConsistent) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("concurrent", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  std::atomic_bool operations_succeeded {true};
  std::vector<std::jthread> workers;
  for (std::size_t worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&operations_succeeded, uuid, certificate = credentials.x509, worker]() {
      for (std::size_t iteration = 0; iteration < 8; ++iteration) {
        if (!nvhttp::set_client_enabled(uuid, (worker + iteration) % 2 == 0)) {
          operations_succeeded = false;
        }
        static_cast<void>(nvhttp::test_support::authorize_client_certificate(certificate));
      }
    });
  }
  workers.clear();

  EXPECT_TRUE(operations_succeeded);
  ASSERT_TRUE(nvhttp::set_client_enabled(uuid, false));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, PersistsScopedPermissionsAndApplicationAllowlist) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("scoped", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  terra_api::client_permissions_t permissions;
  permissions.scopes.emplace("catalog.read");
  permissions.allowed_apps.emplace("11111111-1111-1111-1111-111111111111");
  permissions.input.keyboard = false;
  permissions.input.pen = false;
  permissions.expires_at = 4102444800;
  ASSERT_TRUE(nvhttp::set_client_permissions(uuid, permissions));

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  const auto persisted = nvhttp::test_support::client_permissions(uuid);
  ASSERT_TRUE(persisted.has_value());
  EXPECT_EQ(persisted->scopes, permissions.scopes);
  EXPECT_EQ(persisted->allowed_apps, permissions.allowed_apps);
  EXPECT_FALSE(persisted->input.keyboard);
  EXPECT_FALSE(persisted->input.pen);
  EXPECT_EQ(persisted->expires_at, permissions.expires_at);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RejectsExpiredClientPolicy) {
  const auto credentials = test_utils::certificates::generate_ca_credentials();
  const auto uuid = nvhttp::test_support::add_client("expired policy", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());

  auto permissions = terra_api::legacy_client_permissions();
  permissions.expires_at = 1;
  ASSERT_TRUE(nvhttp::set_client_permissions(uuid, std::move(permissions)));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}

TEST_F(ClientAuthorizationTest, RotatesCertificateWithoutChangingClientIdentity) {
  const auto original = test_utils::certificates::generate_ca_credentials("Original Client");
  const auto replacement = test_utils::certificates::generate_ca_credentials("Replacement Client");
  const auto uuid = nvhttp::test_support::add_client("rotating", original.x509, true);
  ASSERT_FALSE(uuid.empty());

  ASSERT_TRUE(nvhttp::rotate_client_certificate(uuid, replacement.x509));
  EXPECT_FALSE(nvhttp::test_support::authorize_client_certificate(original.x509));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(replacement.x509));
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), replacement.x509);

  nvhttp::test_support::reset_client_state();
  nvhttp::test_support::reload_client_state();
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), replacement.x509);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(replacement.x509));
}

TEST_F(ClientAuthorizationTest, RejectsAtomicUpdateWithoutChangingAnyClientField) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Atomic Update Client");
  const auto uuid = nvhttp::test_support::add_client("atomic", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto original_permissions = nvhttp::get_client_permissions(uuid);
  ASSERT_TRUE(original_permissions.has_value());

  terra_api::client_permissions_t restricted_permissions;
  restricted_permissions.scopes.emplace("catalog.read");
  restricted_permissions.input.keyboard = false;
  nvhttp::client_update_t update;
  update.enabled = false;
  update.permissions = restricted_permissions;
  update.certificate = "not a certificate";

  EXPECT_FALSE(nvhttp::update_client(uuid, std::move(update)));
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
  EXPECT_EQ(nvhttp::get_cert_by_uuid(uuid), credentials.x509);
  const auto current_permissions = nvhttp::get_client_permissions(uuid);
  ASSERT_TRUE(current_permissions.has_value());
  EXPECT_EQ(current_permissions->scopes, original_permissions->scopes);
  EXPECT_TRUE(current_permissions->input.keyboard);
}

TEST_F(ClientAuthorizationTest, RollsBackSecurityUpdateWhenPersistenceFails) {
  const auto credentials = test_utils::certificates::generate_ca_credentials("Persistence Failure Client");
  const auto uuid = nvhttp::test_support::add_client("persistence failure", credentials.x509, true);
  ASSERT_FALSE(uuid.empty());
  const auto original_permissions = nvhttp::get_client_permissions(uuid);
  ASSERT_TRUE(original_permissions.has_value());

  const auto writable_state_file = config::nvhttp.file_state;
  auto restore_state_file = util::fail_guard([&]() {
    config::nvhttp.file_state = writable_state_file;
  });
  config::nvhttp.file_state = (state_file / "missing-parent" / "state.json").string();

  terra_api::client_permissions_t restricted_permissions;
  restricted_permissions.scopes.emplace("catalog.read");
  restricted_permissions.input.keyboard = false;
  EXPECT_FALSE(nvhttp::set_client_permissions(uuid, std::move(restricted_permissions)));

  const auto current_permissions = nvhttp::get_client_permissions(uuid);
  ASSERT_TRUE(current_permissions.has_value());
  EXPECT_EQ(current_permissions->scopes, original_permissions->scopes);
  EXPECT_TRUE(current_permissions->input.keyboard);
  EXPECT_TRUE(nvhttp::test_support::authorize_client_certificate(credentials.x509));
}
