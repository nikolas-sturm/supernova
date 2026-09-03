/**
 * @file src/eclipse_api.h
 * @brief Shared types and constants for the versioned Eclipse extension API.
 */
#pragma once

// standard includes
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>

/**
 * @brief Versioned Eclipse extension API primitives.
 */
namespace eclipse_api {
  constexpr std::uint32_t API_VERSION = 1;  ///< Highest Eclipse API version supported by this host.

  constexpr std::array<std::string_view, 10> SCOPES {
    "catalog.read",
    "stream.launch",
    "session.control",
    "telemetry.read",
    "display.read",
    "display.manage",
    "virtual-display.manage",
    "peripheral.forward",
    "sandbox.manage",
    "host.control",
  };  ///< Stable certificate-bound authorization scope names.

  constexpr std::array<std::string_view, 4> CAPABILITIES {
    "client-permissions",
    "catalog-v2",
    "session-ids",
    "structured-errors",
  };  ///< Implemented Eclipse API capability names.

  /**
   * @brief Per-input-class permissions applied before host input dispatch.
   */
  struct input_permissions_t {
    bool keyboard = true;  ///< Whether keyboard and text packets may reach the host.
    bool mouse = true;  ///< Whether mouse movement, buttons, and scrolling may reach the host.
    bool controller = true;  ///< Whether controller packets may reach the host.
    bool touch = true;  ///< Whether direct touch packets may reach the host.
    bool pen = true;  ///< Whether pen packets may reach the host.
  };

  /**
   * @brief Persisted policy bound to one paired client certificate.
   */
  struct client_permissions_t {
    std::set<std::string, std::less<>> scopes;  ///< Granted Eclipse API scopes.
    std::set<std::string, std::less<>> allowed_apps;  ///< Allowed stable application UUIDs; empty permits every app.
    input_permissions_t input;  ///< Granted streamed-input classes.
    std::int64_t expires_at = 0;  ///< Unix timestamp after which authentication fails; zero means no policy expiry.
  };

  /**
   * @brief Build permissions matching historical paired-client behavior.
   *
   * @return Policy granting every current scope and input class.
   */
  inline client_permissions_t legacy_client_permissions() {
    client_permissions_t permissions;
    for (const auto scope : SCOPES) {
      permissions.scopes.emplace(scope);
    }
    return permissions;
  }

  /**
   * @brief Check whether a scope name is part of Eclipse API v1.
   *
   * @param scope Scope name to validate.
   * @return `true` when the scope is recognized.
   */
  constexpr bool is_known_scope(const std::string_view scope) {
    for (const auto known_scope : SCOPES) {
      if (scope == known_scope) {
        return true;
      }
    }
    return false;
  }
}  // namespace eclipse_api
