#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "gamestream_client.h"
#include "stream_settings.h"

namespace eclipse {

struct HostRecord {
    std::string id;
    std::string name;
    std::string address;
    std::string serverName;
    std::string serverUniqueId;
    std::string appVersion;
    std::string gfeVersion;
    std::string serverState;
    std::string status;
    std::string error;
    std::string serverCertificate;
    std::uint16_t httpsPort = 0;
    int currentGameId = 0;
    int serverCodecModeSupport = 0;
    std::int64_t lastSeenAt = 0;
    bool paired = false;
};

struct SessionRecord {
    std::string hostId;
    int appId = 0;
    std::string appName;
    std::string sessionUrl;
    bool resumed = false;
    std::array<unsigned char, 16> remoteInputKey{};
    std::array<unsigned char, 16> remoteInputIv{};
};

struct SessionUpdate {
    std::string hostId;
    int appId = 0;
    std::string appName;
    std::string state;
    std::string message;
    bool resumed = false;
};

class ControlPlane {
public:
    explicit ControlPlane(std::filesystem::path dataPath);
    ~ControlPlane();

    [[nodiscard]] std::vector<HostRecord> hosts() const;
    [[nodiscard]] std::vector<std::string> hostIds() const;
    HostRecord addHost(std::string name, std::string address);
    HostRecord probeHost(const std::string& id);
    HostRecord pairHost(const std::string& id, const std::string& pin);
    [[nodiscard]] std::vector<GameStreamApp> loadApps(const std::string& hostId);
    [[nodiscard]] std::string boxArtDataUrl(const std::string& hostId, int appId);
    [[nodiscard]] SessionRecord launchApp(const std::string& hostId, int appId,
                                          const StreamSettings& settings);
    void cancelSession(const std::string& hostId);
    void setSessionListener(std::function<void(const SessionUpdate&)> listener);
    void removeHost(const std::string& id);

private:
    void load();
    void saveLocked() const;

    std::filesystem::path dataPath_;
    std::filesystem::path statePath_;
    std::unique_ptr<GameStreamClient> gameStream_;
    std::unique_ptr<class StreamSession> transport_;
    std::string clientId_;
    std::mutex sessionMutex_;
    mutable std::mutex mutex_;
    std::vector<HostRecord> hosts_;
    std::unordered_map<std::string, std::vector<GameStreamApp>> apps_;
    std::optional<SessionRecord> session_;
    std::function<void(const SessionUpdate&)> sessionListener_;
};

}  // namespace eclipse
