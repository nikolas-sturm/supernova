#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "gamestream_client.h"
#include "stream_settings.h"
#include "video_renderer.h"

extern "C" {
#include <Limelight.h>
}

namespace terra {

struct StreamSessionConfig {
    std::string hostId;
    int appId = 0;
    std::string appName;
    std::string address;
    std::string appVersion;
    std::string gfeVersion;
    int serverCodecModeSupport = 0;
    int videoFormat = VIDEO_FORMAT_H264;
    StreamSettings settings;
    LaunchResult launch;
};

struct StreamSessionEvent {
    std::string state;
    std::string message;
};

struct StreamStatisticsSample;

class StreamSession {
public:
    using Listener = std::function<void(const StreamSessionEvent&)>;
    using DisconnectListener =
        std::function<void(StreamSessionEvent event, bool hostEnded, bool userEnded)>;
    using OverlayListener = std::function<void(const StreamWindowBounds&)>;
    using StatisticsListener = std::function<void(const StreamStatisticsSample&)>;

    explicit StreamSession(StreamSessionConfig config, Listener listener,
                             DisconnectListener disconnectListener,
                             OverlayListener overlayListener = {},
                             StatisticsListener statisticsListener = {});
    ~StreamSession();

    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    void start();
    void requestStop();
    void stop();
    void closeOverlay(std::uint64_t revision);
    void acknowledgeOverlayHidden(std::uint64_t revision);
    [[nodiscard]] bool stopRequested() const noexcept;

private:
    struct OverlayState {
        std::uint64_t revision = 0;
        bool visible = false;
        bool captureSuspended = false;
    };

    class ActiveLease {
    public:
        ActiveLease() = default;
        ~ActiveLease();
        ActiveLease(ActiveLease&& other) noexcept;
        ActiveLease& operator=(ActiveLease&&) = delete;
        ActiveLease(const ActiveLease&) = delete;
        ActiveLease& operator=(const ActiveLease&) = delete;

        explicit operator bool() const noexcept { return session_ != nullptr; }
        StreamSession* get() const noexcept { return session_; }
        StreamSession* operator->() const noexcept { return session_; }
        void reset() noexcept;

    private:
        friend class StreamSession;
        explicit ActiveLease(StreamSession* session) : session_(session) {}
        StreamSession* session_ = nullptr;
    };

    void publish(std::string state, std::string message) const noexcept;
    void queueStatus(std::string state, std::string message) noexcept;
    void requestDisconnect(std::string state, std::string message, bool hostEnded,
                           bool userEnded) noexcept;
    void createVideoRendererLocked();
    bool recoverVideoRendererLocked(std::string& error);
    void createAudioRendererLocked();
    bool recoverAudioRendererLocked(std::string& error);

    static ActiveLease active();
    static void publishAndRelease(ActiveLease lease, std::string state,
                                  std::string message) noexcept;
    static void publishQueuedAndRelease(ActiveLease lease) noexcept;
    static void disconnectAndRelease(ActiveLease lease, StreamSessionEvent event,
                                     bool hostEnded, bool userEnded) noexcept;
    static bool activate(StreamSession* session);
    static void deactivate(StreamSession* session);
    static void stageStarting(int stage);
    static void stageFailed(int stage, int errorCode);
    static void connectionStarted();
    static void connectionTerminated(int errorCode);
    static void logMessage(const char* format, ...);
    static void setHdrMode(bool enabled);
    static void rumble(unsigned short controllerNumber, unsigned short lowFrequency,
                       unsigned short highFrequency);
    static void rumbleTriggers(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                               std::uint16_t rightTrigger);
    static void setMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                    std::uint16_t reportRateHz);
    static void setControllerLed(std::uint16_t controllerNumber, std::uint8_t red,
                                 std::uint8_t green, std::uint8_t blue);
    static void connectionStatusUpdate(int status);
    static int videoSetup(int format, int width, int height, int frameRate, void*, int);
    static void videoCleanup();
    static int submitVideo(PDECODE_UNIT decodeUnit);
    static int audioInit(int, const POPUS_MULTISTREAM_CONFIGURATION, void*, int);
    static void audioCleanup();
    static void receiveAudio(char*, int);

    StreamSessionConfig config_;
    Listener listener_;
    DisconnectListener disconnectListener_;
    OverlayListener overlayListener_;
    StatisticsListener statisticsListener_;
    std::atomic_bool stopRequested_{false};
    std::atomic_bool started_{false};
    std::atomic_bool activated_{false};
    std::atomic_bool connectionEstablished_{false};
    std::atomic_bool terminated_{false};
    std::atomic_bool hdrMode_{false};
    std::mutex videoMutex_;
    std::mutex overlayStateMutex_;
    std::mutex audioMutex_;
    std::mutex startupFailureMutex_;
    std::mutex statusMutex_;
    std::vector<StreamSessionEvent> pendingStatuses_;
    std::string startupFailureMessage_;
    int negotiatedVideoFormat_ = 0;
    int negotiatedWidth_ = 0;
    int negotiatedHeight_ = 0;
    int negotiatedFrameRate_ = 0;
    int videoRecoveryAttempts_ = 0;
    std::optional<int> videoDisplayOverride_;
    std::chrono::steady_clock::time_point videoRecoveryWindow_{};
    bool inputEnabled_ = false;
    OverlayState overlayState_;
    StreamWindowBounds overlayBounds_;
    std::unique_ptr<class VideoRenderer> video_;
    std::unique_ptr<class AudioRenderer> audio_;
    OPUS_MULTISTREAM_CONFIGURATION audioConfig_{};
    bool audioConfigured_ = false;
    std::chrono::steady_clock::time_point nextAudioRecovery_{};
    std::chrono::steady_clock::time_point nextRttSample_{};
    std::shared_ptr<class StreamStatistics> statistics_;
    static std::atomic<StreamSession*> active_;
};

}  // namespace terra
