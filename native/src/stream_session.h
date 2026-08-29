#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "gamestream_client.h"
#include "stream_settings.h"

extern "C" {
#include <Limelight.h>
}

namespace eclipse {

struct StreamSessionConfig {
    std::string hostId;
    int appId = 0;
    std::string appName;
    std::string address;
    std::string appVersion;
    std::string gfeVersion;
    int serverCodecModeSupport = 0;
    StreamSettings settings;
    LaunchResult launch;
};

struct StreamSessionEvent {
    std::string state;
    std::string message;
};

class StreamSession {
public:
    using Listener = std::function<void(const StreamSessionEvent&)>;

    explicit StreamSession(StreamSessionConfig config, Listener listener);
    ~StreamSession();

    StreamSession(const StreamSession&) = delete;
    StreamSession& operator=(const StreamSession&) = delete;

    void start();
    void stop();

private:
    void publish(std::string state, std::string message) const;

    static StreamSession* active();
    static void stageStarting(int stage);
    static void stageFailed(int stage, int errorCode);
    static void connectionStarted();
    static void connectionTerminated(int errorCode);
    static void logMessage(const char* format, ...);
    static void connectionStatusUpdate(int status);
    static int videoSetup(int format, int width, int height, int frameRate, void*, int);
    static void videoCleanup();
    static int submitVideo(PDECODE_UNIT decodeUnit);
    static int audioInit(int, const POPUS_MULTISTREAM_CONFIGURATION, void*, int);
    static void audioCleanup();
    static void receiveAudio(char*, int);

    StreamSessionConfig config_;
    Listener listener_;
    std::atomic_bool started_{false};
    std::unique_ptr<class VideoRenderer> video_;
    std::unique_ptr<class AudioRenderer> audio_;
    static std::atomic<StreamSession*> active_;
};

}  // namespace eclipse
