#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gamestream_client.h"
#include "stream_statistics.h"
#include "stream_settings.h"

namespace terra {

struct StreamSessionEvent;

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
    int serverCodecModeSupport = kBaselineCodecModeSupport;
    std::uint64_t maxLumaPixelsHevc = 0;
    std::vector<HostDisplayMode> displayModes;
    std::string wakeMacAddress;
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

struct StreamOverlayRequest {
    std::string hostId;
    int appId = 0;
    std::string appName;
    std::uint64_t generation = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    double scaleFactor = 1.0;
    bool wayland = false;
    bool fullscreen = false;
    StreamSettings settings;
    int videoFormat = 0;
    std::uint16_t controllerMask = 0;
};

struct StreamStatisticsUpdate {
    std::string hostId;
    std::uint64_t generation = 0;
    StreamStatisticsSample sample;
};

class ControlPlane {
public:
    explicit ControlPlane(std::filesystem::path dataPath);
    ~ControlPlane();

    [[nodiscard]] std::vector<HostRecord> hosts() const;
    [[nodiscard]] std::vector<std::string> hostIds() const;
    HostRecord addHost(std::string name, std::string address);
    HostRecord discoverHost(std::string name, std::string address);
    HostRecord probeHost(const std::string& id);
    HostRecord pairHost(const std::string& id, const std::string& pin);
    void wakeHost(const std::string& id);
    [[nodiscard]] std::vector<GameStreamApp> loadApps(const std::string& hostId);
    [[nodiscard]] std::string boxArtDataUrl(const std::string& hostId, int appId);
    [[nodiscard]] SessionRecord launchApp(const std::string& hostId, int appId,
                                          const StreamSettings& settings);
    void stopSession(const std::string& hostId, bool quitHost);
    void resumeStreamOverlay(const std::string& hostId, std::uint64_t generation);
    void setSessionListener(std::function<void(const SessionUpdate&)> listener);
    void setStreamOverlayListener(std::function<void(const StreamOverlayRequest&)> listener);
    void setStreamStatisticsListener(std::function<void(const StreamStatisticsUpdate&)> listener);
    void removeHost(const std::string& id);

private:
    struct DisconnectRequest {
        std::uint64_t generation = 0;
        bool hostEnded = false;
        bool quitHost = false;
        std::string hostId;
        int appId = 0;
        std::string appName;
        bool resumed = false;
        std::string state;
        std::string message;
    };

    void load();
    void saveLocked() const;
    void requestSessionDisconnect(std::uint64_t generation, std::string hostId, int appId,
                                  std::string appName, bool resumed,
                                  StreamSessionEvent event, bool hostEnded, bool quitHost);
    void cleanupSessions();
    void finishSessionDisconnect(const DisconnectRequest& request);

    std::filesystem::path dataPath_;
    std::filesystem::path statePath_;
    std::unique_ptr<GameStreamClient> gameStream_;
    std::shared_ptr<class StreamSession> transport_;
    std::string clientId_;
    std::mutex sessionMutex_;
    std::condition_variable disconnectCondition_;
    std::mutex disconnectMutex_;
    std::thread disconnectThread_;
    std::optional<DisconnectRequest> disconnectRequest_;
    bool shuttingDown_ = false;
    mutable std::mutex mutex_;
    std::vector<HostRecord> hosts_;
    std::unordered_map<std::string, std::vector<GameStreamApp>> apps_;
    std::optional<SessionRecord> session_;
    std::uint64_t sessionGeneration_ = 0;
    bool sessionStopRequested_ = false;
    std::atomic_bool launchCancellationRequested_{false};
    std::function<void(const SessionUpdate&)> sessionListener_;
    std::function<void(const StreamOverlayRequest&)> streamOverlayListener_;
    std::function<void(const StreamStatisticsUpdate&)> streamStatisticsListener_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastWakeRequests_;
};

}  // namespace terra
