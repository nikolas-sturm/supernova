/**
 * @file src/terra_api.h
 * @brief Shared types and constants for the versioned Terra extension API.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <istream>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

/**
 * @brief Versioned Terra extension API primitives.
 * @details Private names use Terra; existing wire and persisted names are retained.
 * See docs/sol.md for compatibility contracts.
 */
namespace terra_api {
  constexpr std::uint32_t API_VERSION = 1;  ///< Highest Terra API version supported by this host.

  /**
   * @brief Validate required Terra request schema metadata.
   *
   * @param value Parsed request body.
   * @return `true` when body is an object containing unsigned integer schema version one.
   */
  inline bool valid_request_schema(const nlohmann::json &value) {
    return value.is_object() && value.contains("schemaVersion") && value.at("schemaVersion").is_number_unsigned() && value.at("schemaVersion") == API_VERSION;
  }

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

  constexpr std::array<std::string_view, 14> KNOWN_CAPABILITIES {
    "client-permissions",
    "catalog-v2",
    "session-ids",
    "structured-errors",
    "events-v1",
    "displays-v1",
    "virtual-displays-v1",
    "telemetry-v1",
    "workspaces-v1",
    "profiles-v1",
    "peripherals-v1",
    "sandboxes-v1",
    "discovery-v1",
    "multi-display-streaming-v1",
  };  ///< Stable Terra API capability names, including unavailable features.

  constexpr std::array<std::string_view, 4> CAPABILITIES {
    "client-permissions",
    "session-ids",
    "structured-errors",
    "catalog-v2",
  };  ///< Terra API capabilities not dependent on runtime manager health.

  constexpr std::array<std::string_view, 5> INPUT_CLASSES {
    "keyboard",
    "mouse",
    "controller",
    "touch",
    "pen",
  };  ///< Stable streamed-input class names accepted during Terra pairing.

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
    std::set<std::string, std::less<>> scopes;  ///< Granted Terra API scopes.
    std::set<std::string, std::less<>> allowed_apps;  ///< Allowed stable application UUIDs; empty permits every app.
    input_permissions_t input;  ///< Granted streamed-input classes.
    std::int64_t expires_at = 0;  ///< Unix timestamp after which authentication fails; zero means no policy expiry.
  };

  /**
   * @brief Result category for bounded request-body reads.
   */
  enum class body_read_status_t {
    success,  ///< Complete body fit within configured limit.
    too_large,  ///< Body exceeded configured limit.
  };

  /**
   * @brief Result from reading a bounded request body.
   */
  struct body_read_t {
    body_read_status_t status;  ///< Read result category.
    std::string text;  ///< Complete body on success, otherwise empty.
  };

  /**
   * @brief Read a stream without buffering more than configured maximum.
   *
   * @param stream Source request stream.
   * @param maximum_bytes Maximum accepted byte count.
   * @return Complete body or `too_large` without retaining overflow bytes.
   */
  inline body_read_t read_bounded_body(std::istream &stream, const std::size_t maximum_bytes) {
    std::string text;
    std::array<char, 4096> buffer {};
    while (stream) {
      stream.read(buffer.data(), buffer.size());
      const auto read = stream.gcount();
      if (read <= 0) {
        break;
      }
      if (text.size() > maximum_bytes || static_cast<std::size_t>(read) > maximum_bytes - text.size()) {
        return {body_read_status_t::too_large, {}};
      }
      text.append(buffer.data(), static_cast<std::size_t>(read));
    }
    return {body_read_status_t::success, std::move(text)};
  }

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
   * @brief Result of parsing optional Terra pairing policy fields.
   */
  struct pairing_policy_t {
    bool valid = true;  ///< Whether supplied fields and values satisfy API v1 policy syntax.
    bool explicit_policy = false;  ///< Whether both policy fields were supplied.
    client_permissions_t permissions = legacy_client_permissions();  ///< Requested policy or historical defaults.
  };

  constexpr bool is_known_scope(std::string_view scope);

  /**
   * @brief Parse certificate-bound scope and input policy from a pairing request.
   *
   * @param scopes Comma-separated scopes, or no value when omitted.
   * @param inputs Comma-separated input classes, or no value when omitted.
   * @return Parsed policy. Supplying exactly one field or an unknown value makes it invalid.
   */
  inline pairing_policy_t parse_pairing_policy(
    const std::optional<std::string_view> scopes,
    const std::optional<std::string_view> inputs
  ) {
    pairing_policy_t result;
    if (scopes.has_value() != inputs.has_value()) {
      result.valid = false;
      return result;
    }
    if (!scopes) {
      return result;
    }

    result.explicit_policy = true;
    result.permissions = {};
    result.permissions.input.keyboard = false;
    result.permissions.input.mouse = false;
    result.permissions.input.controller = false;
    result.permissions.input.touch = false;
    result.permissions.input.pen = false;

    const auto for_each_value = [](const std::string_view values, const auto &callback) {
      std::size_t start = 0;
      while (start <= values.size()) {
        const auto separator = values.find(',', start);
        const auto value = values.substr(start, separator == std::string_view::npos ? values.size() - start : separator - start);
        if (!value.empty()) {
          callback(value);
        }
        if (separator == std::string_view::npos) {
          break;
        }
        start = separator + 1;
      }
    };

    for_each_value(*scopes, [&](const std::string_view scope) {
      if (is_known_scope(scope)) {
        result.permissions.scopes.emplace(scope);
      } else {
        result.valid = false;
      }
    });
    for_each_value(*inputs, [&](const std::string_view input) {
      if (input == "keyboard") {
        result.permissions.input.keyboard = true;
      } else if (input == "mouse") {
        result.permissions.input.mouse = true;
      } else if (input == "controller") {
        result.permissions.input.controller = true;
      } else if (input == "touch") {
        result.permissions.input.touch = true;
      } else if (input == "pen") {
        result.permissions.input.pen = true;
      } else {
        result.valid = false;
      }
    });
    return result;
  }

  /**
   * @brief Check whether a request explicitly opted into Terra API v1 fields.
   *
   * @param version Query-string API version value.
   * @return `true` only for version `1`.
   */
  constexpr bool api_v1_requested(const std::string_view version) {
    return version == "1";
  }

  /**
   * @brief Check whether a scope name is part of Terra API v1.
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

  /**
   * @brief Serialize currently operational capability names for GameStream server information.
   *
   * @return Comma-separated capability names in protocol order.
   */
  inline std::string capabilities_csv() {
    std::string result;
    for (const auto capability : CAPABILITIES) {
      if (!result.empty()) {
        result += ',';
      }
      result += capability;
    }
    return result;
  }

  /**
   * @brief Check whether a platform MAC address can be used for Wake-on-LAN.
   *
   * @param mac Colon-separated six-octet MAC address.
   * @return `true` for a nonzero unicast MAC address.
   */
  inline bool wake_on_lan_available(const std::string_view mac) {
    if (mac.size() != 17) {
      return false;
    }

    std::array<unsigned int, 6> octets {};
    for (std::size_t octet = 0; octet < octets.size(); ++octet) {
      const auto offset = octet * 3;
      if (!std::isxdigit(static_cast<unsigned char>(mac[offset])) || !std::isxdigit(static_cast<unsigned char>(mac[offset + 1]))) {
        return false;
      }
      if (octet + 1 < octets.size() && mac[offset + 2] != ':') {
        return false;
      }

      octets[octet] = static_cast<unsigned int>(std::stoul(std::string {mac.substr(offset, 2)}, nullptr, 16));
    }

    const bool all_zero = std::ranges::all_of(octets, [](const auto octet) {
      return octet == 0;
    });
    return !all_zero && (octets.front() & 1U) == 0;
  }

}  // namespace terra_api
