#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "identity.h"
#include "stream_settings.h"

namespace terra {

inline constexpr int kBaselineCodecModeSupport = 0x00000001;

struct HostDisplayMode {
    int width = 0;
    int height = 0;
    int refreshRate = 0;

    auto operator<=>(const HostDisplayMode&) const = default;
};

struct ServerInfo {
    std::string serverName;
    std::string serverUniqueId;
    std::string appVersion;
    std::string gfeVersion;
    std::string serverState;
    std::uint16_t httpsPort = 0;
    int currentGameId = 0;
    int serverCodecModeSupport = kBaselineCodecModeSupport;
    std::uint64_t maxLumaPixelsHevc = 0;
    std::vector<HostDisplayMode> displayModes;
    std::string macAddress;
    bool paired = false;
    int eclipseApiVersion = 0;  ///< Advertised Eclipse API version, or zero when unavailable.
    std::uint16_t eclipseApiPort = 0;              ///< Advertised Eclipse API HTTPS port.
    std::vector<std::string> eclipseCapabilities;  ///< Unique advertised Eclipse capabilities.
};

/** @brief Access profile requested while pairing with Sol. */
enum class PairingAccess {
    gaming,       ///< Streaming, catalog, session, and telemetry access.
    workstation,  ///< All stable Eclipse API scopes.
};

/** @brief Named Sol launch profile available for one Catalog V2 application. */
struct LaunchProfileSummary {
    std::string id;    ///< Stable launch-profile UUID.
    std::string name;  ///< User-visible profile name.
    bool isDefault = false;  ///< Whether Sol selects this profile when none is explicit.
};

struct GameStreamApp {
    int id = 0;
    std::string name;
    bool hdrSupported = false;
    bool appCollectorGame = false;
    std::string uuid;                            ///< Stable Catalog V2 application identifier.
    std::string kind = "unknown";                ///< Catalog V2 application kind.
    std::string description;                     ///< User-visible application description.
    std::string source;                          ///< Catalog source identifier.
    std::string publisher;                       ///< Application publisher.
    std::vector<std::string> tags;               ///< Catalog tags.
    std::vector<std::string> inputRequirements;  ///< Required input classes.
    std::vector<LaunchProfileSummary> launchProfiles;  ///< Selectable Sol launch profiles.
    bool installed = true;                       ///< Whether application is installed.
    bool updateAvailable = false;                ///< Whether host reports an update.
    std::string assetId;                         ///< Preferred poster or icon asset identifier.
    std::string assetPath;                       ///< Authenticated Catalog V2 asset path.
    std::uint64_t assetRevision = 0;             ///< Asset cache revision.
    std::string displayProfileId;                ///< Default display profile UUID.
    std::string streamProfileId;                 ///< Default stream profile UUID.
    std::string sandboxProfileId;                ///< Default sandbox profile UUID.
};

struct LaunchResult {
    int appId = 0;
    bool resumed = false;
    std::string sessionUrl;
    std::array<unsigned char, 16> remoteInputKey{};
    std::array<unsigned char, 16> remoteInputIv{};
    std::string logicalSessionId;  ///< Stable Eclipse session identifier when
                                   ///< supplied by Sol.
    std::string childStreamId;  ///< Unique Eclipse child-stream identifier.
};

/** @brief Parsed response from an Eclipse JSON API request. */
struct ApiResponse {
    int status = 0;                              ///< HTTP response status.
    std::map<std::string, std::string> headers;  ///< HTTP response headers.
    nlohmann::json body;  ///< Parsed response body, or null for an empty response.
};

/** @brief Structured Eclipse API HTTP error. */
class ApiError : public std::runtime_error {
public:
    /**
     * @brief Construct a structured API error.
     *
     * @param status HTTP response status.
     * @param code Stable API error code.
     * @param message Human-readable error message.
     * @param details Optional structured error details.
     */
    ApiError(int status, std::string code, std::string message, nlohmann::json details);

    /** @return HTTP response status. */
    [[nodiscard]] int status() const noexcept;
    /** @return Stable API error code. */
    [[nodiscard]] const std::string& code() const noexcept;
    /** @return Structured API error details. */
    [[nodiscard]] const nlohmann::json& details() const noexcept;

private:
    int status_;
    std::string code_;
    nlohmann::json details_;
};

class GameStreamClient {
public:
    explicit GameStreamClient(const std::filesystem::path& dataPath);

    [[nodiscard]] static std::string normalizeAddress(std::string address);
    [[nodiscard]] static std::string streamHost(const std::string& address);
    [[nodiscard]] ServerInfo probe(const std::string& address, std::uint16_t httpsPort,
                                   const std::string& clientId,
                                   const std::string& serverCertificate = {}) const;
    /**
     * @brief Pair this identity with a Sol host.
     *
     * @param address Host address.
     * @param httpsPort GameStream HTTPS port.
     * @param appVersion Host GameStream version.
     * @param clientId GameStream client identifier.
     * @param pin Four-digit pairing PIN.
     * @param access Eclipse API access profile to request.
     * @return Pinned host certificate in PEM form.
     */
    [[nodiscard]] std::string pair(const std::string& address, std::uint16_t httpsPort,
                                   const std::string& appVersion, const std::string& clientId,
                                   const std::string& pin,
                                   PairingAccess access = PairingAccess::gaming) const;
    [[nodiscard]] std::vector<GameStreamApp> apps(const std::string& address,
                                                  std::uint16_t httpsPort,
                                                  const std::string& clientId,
                                                  const std::string& serverCertificate) const;
    [[nodiscard]] std::string boxArt(const std::string& address, std::uint16_t httpsPort,
                                     const std::string& clientId,
                                     const std::string& serverCertificate, int appId) const;
    /**
     * @brief Launch or resume a GameStream application.
     *
     * @param address Host address.
     * @param httpsPort GameStream HTTPS port.
     * @param clientId GameStream client identifier.
     * @param serverCertificate Pinned host certificate.
     * @param appId Legacy application identifier.
     * @param resume Whether to resume an existing stream.
     * @param settings Stream settings.
     * @param cancellation Optional cancellation flag.
     * @param appUuid Optional stable Eclipse application UUID.
     * @param launchProfileId Optional Eclipse launch-profile UUID.
     * @param workspaceId Optional Eclipse workspace UUID.
     * @param displayId Optional Eclipse virtual-display UUID.
     * @param primary Whether this child owns controller transport.
     * @return Launch transport and logical-session details.
     */
    [[nodiscard]] LaunchResult launch(const std::string& address, std::uint16_t httpsPort,
                                      const std::string& clientId,
                                      const std::string& serverCertificate, int appId, bool resume,
                                       const StreamSettings& settings,
                                        const std::atomic_bool* cancellation = nullptr,
                                        const std::string& appUuid = {},
                                        const std::string& launchProfileId = {},
                                        const std::string& workspaceId = {},
                                        const std::string& displayId = {},
                                        bool primary = true) const;

    /**
     * @brief Send a bounded JSON request to paired Sol's Eclipse API.
     *
     * @param address Host address.
     * @param apiPort Eclipse API port, or zero to use GameStream HTTPS default.
     * @param serverCertificate Pinned host certificate.
     * @param method GET, POST, PATCH, PUT, or DELETE.
     * @param path Absolute Eclipse v1 path beginning with `/eclipse/v1`.
     * @param body Optional JSON request body.
     * @param headers Additional HTTP request headers.
     * @return HTTP status, response headers, and parsed JSON body.
     * @throws ApiError When Sol returns a non-success HTTP status.
     */
    [[nodiscard]] ApiResponse
    apiRequest(const std::string& address, std::uint16_t apiPort,
               const std::string& serverCertificate, const std::string& method,
               const std::string& path, const std::optional<nlohmann::json>& body = std::nullopt,
               const std::map<std::string, std::string>& headers = {}) const;

    /**
     * @brief Download bounded bytes from paired Sol's Eclipse API.
     *
     * @param address Host address.
     * @param apiPort Eclipse API port, or zero to use GameStream HTTPS default.
     * @param serverCertificate Pinned host certificate.
     * @param path Absolute Eclipse v1 asset path.
     * @param headers Additional HTTP request headers.
     * @return Response bytes.
     * @throws ApiError When Sol returns a non-success HTTP status.
     */
    [[nodiscard]] std::string
    apiBytes(const std::string& address, std::uint16_t apiPort,
             const std::string& serverCertificate, const std::string& path,
             const std::map<std::string, std::string>& headers = {}) const;

    /**
     * @brief Stream raw server-sent event chunks from paired Sol.
     *
     * @param address Host address.
     * @param apiPort Eclipse API port, or zero to use GameStream HTTPS default.
     * @param serverCertificate Pinned host certificate.
     * @param path Absolute Eclipse v1 event path.
     * @param lastEventId Optional SSE resume identifier.
     * @param cancellation Cancellation flag monitored for request lifetime.
     * @param onChunk Callback receiving raw transport chunks.
     * @throws ApiError When Sol returns a non-success HTTP status.
     */
    void streamEvents(const std::string& address, std::uint16_t apiPort,
                      const std::string& serverCertificate, const std::string& path,
                      const std::string& lastEventId, const std::atomic_bool& cancellation,
                      const std::function<void(const std::string&)>& onChunk) const;
    void cancel(const std::string& address, std::uint16_t httpsPort, const std::string& clientId,
                const std::string& serverCertificate) const;

private:
    Identity identity_;
    mutable std::mutex apiMutex_;  ///< Serializes short-lived Eclipse API TLS connections.
};

}  // namespace terra
