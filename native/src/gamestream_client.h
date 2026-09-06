#pragma once

#include <array>
#include <atomic>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "identity.h"
#include "stream_settings.h"

namespace eclipse {

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
};

struct GameStreamApp {
    int id = 0;
    std::string name;
    bool hdrSupported = false;
    bool appCollectorGame = false;
};

struct LaunchResult {
    int appId = 0;
    bool resumed = false;
    std::string sessionUrl;
    std::array<unsigned char, 16> remoteInputKey{};
    std::array<unsigned char, 16> remoteInputIv{};
};

class GameStreamClient {
public:
    explicit GameStreamClient(const std::filesystem::path& dataPath);

    [[nodiscard]] static std::string normalizeAddress(std::string address);
    [[nodiscard]] static std::string streamHost(const std::string& address);
    [[nodiscard]] ServerInfo probe(const std::string& address, std::uint16_t httpsPort,
                                   const std::string& clientId,
                                   const std::string& serverCertificate = {}) const;
    [[nodiscard]] std::string pair(const std::string& address, std::uint16_t httpsPort,
                                    const std::string& appVersion, const std::string& clientId,
                                    const std::string& pin) const;
    [[nodiscard]] std::vector<GameStreamApp> apps(const std::string& address,
                                                   std::uint16_t httpsPort,
                                                   const std::string& clientId,
                                                   const std::string& serverCertificate) const;
    [[nodiscard]] std::string boxArt(const std::string& address, std::uint16_t httpsPort,
                                     const std::string& clientId,
                                     const std::string& serverCertificate, int appId) const;
    [[nodiscard]] LaunchResult launch(const std::string& address, std::uint16_t httpsPort,
                                        const std::string& clientId,
                                        const std::string& serverCertificate, int appId,
                                        bool resume, const StreamSettings& settings,
                                        const std::atomic_bool* cancellation = nullptr) const;
    void cancel(const std::string& address, std::uint16_t httpsPort,
                const std::string& clientId, const std::string& serverCertificate) const;

private:
    Identity identity_;
};

}  // namespace eclipse
