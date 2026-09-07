/**
 * @file tests/unit/platform/windows/test_terra_sandbox_provider.cpp
 * @brief Focused Windows Terra sandbox-provider capability tests.
 */

// test includes
#include "../../../tests_common.h"

#ifdef _WIN32
  // standard includes
  #include <chrono>
  #include <thread>

  // local includes
  #include <src/terra_sandboxes.h>
  #include <src/platform/windows/terra_sandbox_provider.h>

namespace {
  using terra_sandboxes::launch_request_t;
  using nlohmann::json;

  constexpr const char *APP = "eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee";  ///< Test application UUID.

  /**
   * @brief Build complete policy representing provider-supported unrestricted host access.
   * @return Normalized supported policy.
   */
  json supported_policy() {
    json policy;
    EXPECT_TRUE(terra_sandboxes::normalize_policy({{"allowedAppUuids", {APP}}}, policy));
    policy["filesystem"] = {{"readOnlyRoots", json::array()}, {"writableRoots", json::array()}, {"denyOther", false}};
    policy["network"] = {{"mode", "full"}, {"allowedHosts", json::array()}, {"allowedPorts", json::array()}};
    policy["gpu"] = {{"mode", "full"}, {"encoder", true}};
    policy["displays"] = {{"allowedIds", json::array()}, {"virtualOnly", false}};
    policy["clipboard"] = "bidirectional";
    return policy;
  }

  /**
   * @brief Build provider callbacks without application or secret resolution.
   * @return Windows sandbox callbacks.
   */
  terra_sandboxes::callbacks_t callbacks() {
    return terra::windows::sandbox::make_callbacks({{}, {}, {}});
  }

  /**
   * @brief Evaluate provider capability for one policy.
   * @param policy Effective policy.
   * @param reason Receives rejection diagnostic.
   * @return Provider capability result.
   */
  bool capable(const json &policy, std::string &reason) {
    auto provider_callbacks = callbacks();
    return provider_callbacks.provider_capable(launch_request_t {"", {}, policy, std::nullopt, false}, reason);
  }
}  // namespace

TEST(TerraWindowsSandboxProviderTest, AcceptsOnlyExplicitlySupportedResourceLimits) {
  auto policy = supported_policy();
  policy["resources"] = {{"cpuPercent", 50}, {"memoryBytes", 1024 * 1024}, {"processCount", 2}, {"storageBytes", nullptr}};
  policy["timeoutMs"] = 60000;
  std::string reason;
  EXPECT_TRUE(capable(policy, reason)) << reason;

  policy["resources"]["storageBytes"] = 4096;
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Storage quotas cannot be enforced");
}

TEST(TerraWindowsSandboxProviderTest, RejectsFilesystemAndNetworkFiltering) {
  std::string reason;
  auto policy = supported_policy();
  policy["filesystem"]["denyOther"] = true;
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Filesystem isolation cannot be enforced");

  policy = supported_policy();
  policy["network"]["mode"] = "none";
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Network filtering requires unavailable WFP enforcement");

  policy = supported_policy();
  policy["network"]["allowedHosts"] = {"example.test"};
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Network filtering requires unavailable WFP enforcement");
}

TEST(TerraWindowsSandboxProviderTest, RejectsGpuDisplayClipboardAndHostIsolation) {
  std::string reason;
  for (const auto *mode : {"none", "compute", "display"}) {
    auto policy = supported_policy();
    policy["gpu"]["mode"] = mode;
    EXPECT_FALSE(capable(policy, reason));
    EXPECT_EQ(reason, "GPU mode or encoder isolation cannot be enforced");
  }

  auto policy = supported_policy();
  policy["displays"]["virtualOnly"] = true;
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Display isolation cannot be enforced");

  policy = supported_policy();
  policy["clipboard"] = "none";
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Clipboard isolation cannot be enforced");

  policy = supported_policy();
  policy["hostIntegration"] = "restricted";
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Restricted host integration cannot be enforced");
}

TEST(TerraWindowsSandboxProviderTest, RejectsDeviceElevationAndPersistentDataRequests) {
  std::string reason;
  auto policy = supported_policy();
  policy["peripherals"]["deviceIds"] = {"usb:1"};
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Peripheral or device isolation cannot be enforced");

  policy = supported_policy();
  policy["elevation"] = "allow";
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Elevation is never supported");

  policy = supported_policy();
  policy["persistentData"] = {{"enabled", true}, {"name", "data"}};
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Persistent sandbox data cannot be enforced");
}

TEST(TerraWindowsSandboxProviderTest, RequiresSecretResolverAndWindowsUniqueEnvironmentNames) {
  std::string reason;
  auto policy = supported_policy();
  policy["environment"] = {{"allowedNames", {"TOKEN"}}, {"values", {{"TOKEN", {{"secretRef", "vault:item"}}}}}};
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Environment secret resolver is unavailable");

  policy["environment"] = {{"allowedNames", {"PATH", "Path"}}, {"values", json::object()}};
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Environment names must be unique under Windows semantics");

  auto provider_callbacks = terra::windows::sandbox::make_callbacks({{}, {}, [](const std::string &) -> std::optional<std::string> {
                                                                         return "secret";
                                                                       }});
  policy["environment"] = {{"allowedNames", {"TOKEN"}}, {"values", {{"TOKEN", {{"secretRef", "vault:item"}}}}}};
  EXPECT_TRUE(provider_callbacks.provider_capable(launch_request_t {"", {}, policy, std::nullopt, false}, reason)) << reason;

  auto request = launch_request_t {"", {APP, std::nullopt, "C:\\Windows\\System32\\cmd.exe", {{"environment", {{"OTHER", "value"}}}}}, supported_policy(), std::nullopt, false};
  EXPECT_FALSE(provider_callbacks.provider_capable(request, reason));
  EXPECT_EQ(reason, "Launch environment name is not allowed by sandbox policy");
}

TEST(TerraWindowsSandboxProviderTest, RejectsUnsupportedExecutableAndLaunchDataSemantics) {
  std::string reason;
  auto policy = supported_policy();
  policy["executablePolicy"] = "allowlist";
  EXPECT_FALSE(capable(policy, reason));
  EXPECT_EQ(reason, "Executable allowlist policy cannot be enforced");

  auto provider_callbacks = callbacks();
  launch_request_t request {"", {APP, std::nullopt, "relative.exe", json::object()}, supported_policy(), std::nullopt, false};
  EXPECT_FALSE(provider_callbacks.provider_capable(request, reason));
  EXPECT_EQ(reason, "Executable must be an absolute UTF-8 path");

  request.application.executable = "C:\\Windows\\System32\\cmd.exe";
  request.application.launch_data = {{"shell", true}};
  EXPECT_FALSE(provider_callbacks.provider_capable(request, reason));
  EXPECT_EQ(reason, "Launch data contains an unsupported field");
}

TEST(TerraWindowsSandboxProviderTest, LaunchesRestrictedProcessAndReportsExit) {
  auto provider_callbacks = callbacks();
  auto policy = supported_policy();
  policy["resources"] = {{"cpuPercent", 50}, {"memoryBytes", 64 * 1024 * 1024}, {"processCount", 1}, {"storageBytes", nullptr}};
  policy["timeoutMs"] = 5000;
  launch_request_t request {
    "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
    {APP, std::nullopt, "C:\\Windows\\System32\\cmd.exe", {{"arguments", {"/d", "/c", "if defined PATH (exit 9) else (exit 7)"}}}},
    policy,
    std::nullopt,
    false,
  };
  terra_sandboxes::error_t error;
  const auto launched = provider_callbacks.launch(request, error);
  ASSERT_TRUE(launched) << error.code << ": " << error.message;

  terra_sandboxes::reconciliation_t state;
  for (int attempt = 0; attempt < 100; ++attempt) {
    state = provider_callbacks.reconcile(launched->runtime_id);
    if (state.status != terra_sandboxes::reconciliation_t::status_t::running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  EXPECT_EQ(state.status, terra_sandboxes::reconciliation_t::status_t::exited);
  EXPECT_EQ(state.exit_code, 7);

  const auto terminated = provider_callbacks.terminate(launched->runtime_id, false);
  EXPECT_TRUE(terminated.terminated);
  EXPECT_FALSE(terminated.exit_code);
}

TEST(TerraWindowsSandboxProviderTest, EnforcesWallClockDeadlineThroughJob) {
  auto provider_callbacks = callbacks();
  auto policy = supported_policy();
  policy["timeoutMs"] = 100;
  launch_request_t request {
    "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
    {APP, std::nullopt, "C:\\Windows\\System32\\cmd.exe", {{"arguments", {"/d", "/c", "ping -n 30 127.0.0.1 >nul"}}}},
    policy,
    std::nullopt,
    false,
  };
  terra_sandboxes::error_t error;
  const auto launched = provider_callbacks.launch(request, error);
  ASSERT_TRUE(launched) << error.code << ": " << error.message;

  terra_sandboxes::reconciliation_t state;
  for (int attempt = 0; attempt < 200; ++attempt) {
    state = provider_callbacks.reconcile(launched->runtime_id);
    if (state.status != terra_sandboxes::reconciliation_t::status_t::running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  EXPECT_EQ(state.status, terra_sandboxes::reconciliation_t::status_t::exited);
  EXPECT_NE(state.exit_code, 0);
  EXPECT_TRUE(provider_callbacks.terminate(launched->runtime_id, false).terminated);
}

TEST(TerraWindowsSandboxProviderTest, AppliesAllowedLaunchEnvironment) {
  auto provider_callbacks = callbacks();
  auto policy = supported_policy();
  policy["environment"] = {{"allowedNames", {"TOKEN"}}, {"values", nlohmann::json::object()}};
  launch_request_t request {
    "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa",
    {APP, std::nullopt, "C:\\Windows\\System32\\cmd.exe", {{"arguments", {"/d", "/c", "if \"%TOKEN%\"==\"launch\" (exit 7) else (exit 9)"}}, {"environment", {{"TOKEN", "launch"}}}}},
    policy,
    std::nullopt,
    false,
  };
  terra_sandboxes::error_t error;
  const auto launched = provider_callbacks.launch(request, error);
  ASSERT_TRUE(launched) << error.code << ": " << error.message;

  terra_sandboxes::reconciliation_t state;
  for (int attempt = 0; attempt < 100; ++attempt) {
    state = provider_callbacks.reconcile(launched->runtime_id);
    if (state.status != terra_sandboxes::reconciliation_t::status_t::running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds {10});
  }
  EXPECT_EQ(state.status, terra_sandboxes::reconciliation_t::status_t::exited);
  EXPECT_EQ(state.exit_code, 7);
  EXPECT_TRUE(provider_callbacks.terminate(launched->runtime_id, false).terminated);
}

TEST(TerraWindowsSandboxProviderTest, MissingRuntimeTerminationIsIdempotent) {
  auto provider_callbacks = callbacks();
  const auto terminated = provider_callbacks.terminate("missing-runtime", true);
  EXPECT_TRUE(terminated.terminated);
  EXPECT_FALSE(terminated.error);
}

TEST(TerraWindowsSandboxProviderTest, HealthReportsOperationalProvider) {
  const auto state = terra::windows::sandbox::health();
  EXPECT_TRUE(state.available);
  EXPECT_TRUE(state.reason_code.empty());
  EXPECT_TRUE(state.reason.empty());
}
#else
TEST(TerraWindowsSandboxProviderTest, WindowsOnly) {
  GTEST_SKIP() << "Windows sandbox provider is Windows-specific";
}
#endif
