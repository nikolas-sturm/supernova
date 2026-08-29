#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "identity.h"
#include "stream_settings.h"

namespace eclipse {

struct ServerInfo {
    std::string serverName;
    std::string serverUniqueId;
    std::string appVersion;
    std::string gfeVersion;
    std::string serverState;
    std::uint16_t httpsPort = 0;
    int currentGameId = 0;
    int serverCodecModeSupport = 0;
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
                                       bool resume, const StreamSettings& settings) const;
    void cancel(const std::string& address, std::uint16_t httpsPort,
                const std::string& clientId, const std::string& serverCertificate) const;

private:
    Identity identity_;
};

}  // namespace eclipse
