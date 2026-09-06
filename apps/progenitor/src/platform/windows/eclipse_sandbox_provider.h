/**
 * @file src/platform/windows/eclipse_sandbox_provider.h
 * @brief Fail-closed Windows process provider for Eclipse sandboxes.
 */
#pragma once

// standard includes
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

// local includes
#include "src/eclipse_sandboxes.h"

namespace eclipse::windows::sandbox {
  /**
   * @brief Resolve one authorized application and optional launch profile.
   */
  using application_resolver_t = std::function<std::optional<eclipse_sandboxes::application_t>(const std::string &, const std::optional<std::string> &)>;

  /**
   * @brief Resolve one environment secret reference without exposing it to persistence.
   */
  using secret_resolver_t = std::function<std::optional<std::string>(const std::string &)>;

  /**
   * @brief Windows sandbox-provider dependencies.
   *
   * Supported launches use an exact executable path and optional `launch_data`
   * object containing only `arguments` (a string array) and `workingDirectory`
   * (an absolute string path or null). Environment entries come exclusively from
   * effective-policy `environment.values`; no parent environment is inherited.
   */
  struct configuration_t {
    std::filesystem::path persistence_path;  ///< Atomic sandbox-manager persistence document path.
    application_resolver_t resolve_application;  ///< Authorized application and launch-profile resolver.
    secret_resolver_t resolve_secret;  ///< Optional resolver required by `secretRef` environment values.
  };

  /**
   * @brief Provider runtime health observation.
   */
  struct health_t {
    bool available;  ///< Whether provider can establish isolation and launch processes today.
    std::string reason_code;  ///< Stable unavailable reason; empty when available.
    std::string reason;  ///< Non-sensitive diagnostic; empty when available.
  };

  /**
   * @brief Probe provider isolation prerequisites without launching a process.
   *
   * Verifies that a non-elevated restricted primary token can be established and
   * that a kill-on-close Job Object accepts the provider limit configuration.
   * Call before advertising sandbox capabilities so the host reports accurate
   * unavailability reasons.
   *
   * @return Current provider health.
   */
  [[nodiscard]] health_t health();

  /**
   * @brief Build production Windows sandbox callbacks.
   *
   * Provider supports only policies whose non-resource isolation settings request
   * no filtering: configured executable only, unrestricted filesystem/network/GPU/
   * display/clipboard access, no provider-managed input/peripheral/host integration,
   * denied elevation, no persistent data, and delete cleanup. CPU, aggregate memory,
   * active-process, and wall-clock limits are enforced with a kill-on-close Job Object
   * plus a deadline watchdog. Every other policy is rejected by `provider_capable`.
   *
   * @param configuration Persistence and resolution dependencies.
   * @return Callbacks suitable for `eclipse_sandboxes::manager_t`.
   */
  [[nodiscard]] eclipse_sandboxes::callbacks_t make_callbacks(configuration_t configuration);
}  // namespace eclipse::windows::sandbox
