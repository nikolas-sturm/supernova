#include "audio_renderer.h"
#include "stream_session.h"
#include "stream_worker_protocol.h"
#include "video_renderer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
std::atomic_bool enteredStart = false;
std::atomic_bool interrupted = false;
std::atomic_bool blockStart = false;
CONNECTION_LISTENER_CALLBACKS savedCallbacks{};
AUDIO_RENDERER_CALLBACKS savedAudioCallbacks{};
DECODER_RENDERER_CALLBACKS savedVideoCallbacks{};
STREAM_CONFIGURATION savedStreamConfiguration{};
std::atomic_bool forceAudioLoss = false;
std::atomic_bool forceVideoRecovery = false;
std::optional<int> nextRecoveryDisplay;
int lastRendererDisplay = -1;
int audioInitializations = 0;
int startResult = 0;
int failedStage = STAGE_NONE;
unsigned int connectivityResult = 0;
int connectivityTests = 0;
int motionStateCallbacks = 0;
int ledCallbacks = 0;
int overlayCloses = 0;
int overlayHiddenAcknowledgements = 0;
terra::VideoRenderer::OverlayListener savedOverlayListener;
terra::VideoRenderer::OverlayCaptureListener savedOverlayCaptureListener;
terra::StreamOverlayState lastInitialOverlayState;
terra::StreamSession* cancelBeforeFirstStage = nullptr;

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

terra::StreamSessionConfig testConfig() {
    terra::StreamSessionConfig config;
    config.hostId = "host";
    config.appId = 1;
    config.appName = "Game";
    config.address = "127.0.0.1";
    config.appVersion = "7.1.0.0";
    config.serverCodecModeSupport = SCM_H264;
    config.videoFormat = VIDEO_FORMAT_H264;
    config.launch.sessionUrl = "rtsp://127.0.0.1:48010";
    return config;
}

void waitForStart() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!enteredStart.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error("Timed out waiting for connection start.");
        }
        std::this_thread::yield();
    }
}
}

extern "C" void LiInitializeStreamConfiguration(PSTREAM_CONFIGURATION value) {
    std::memset(value, 0, sizeof(*value));
}
extern "C" void LiInitializeServerInformation(PSERVER_INFORMATION value) {
    std::memset(value, 0, sizeof(*value));
}
extern "C" void LiInitializeConnectionCallbacks(PCONNECTION_LISTENER_CALLBACKS value) {
    std::memset(value, 0, sizeof(*value));
}
extern "C" void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS value) {
    std::memset(value, 0, sizeof(*value));
}
extern "C" void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS value) {
    std::memset(value, 0, sizeof(*value));
}
extern "C" const char* LiGetStageName(int) { return "test"; }
extern "C" unsigned int LiGetPortFlagsFromStage(int stage) {
    return stage == STAGE_RTSP_HANDSHAKE ? ML_PORT_FLAG_TCP_48010 | ML_PORT_FLAG_UDP_48010 : 0;
}
extern "C" unsigned int LiGetPortFlagsFromTerminationErrorCode(int) { return 0; }
extern "C" unsigned int LiTestClientConnectivity(const char*, unsigned short,
                                                   unsigned int) {
    ++connectivityTests;
    return connectivityResult;
}
extern "C" void LiStringifyPortFlags(unsigned int flags, const char*, char* output,
                                       int outputLength) {
    const std::string value = flags == ML_PORT_FLAG_TCP_48010 ? "TCP 48010" :
                              flags == ML_PORT_FLAG_UDP_48010 ? "UDP 48010" :
                                                               "TCP 48010, UDP 48010";
    std::snprintf(output, static_cast<std::size_t>(outputLength), "%s", value.c_str());
}
extern "C" int LiStartConnection(PSERVER_INFORMATION, PSTREAM_CONFIGURATION streamConfig,
                                    PCONNECTION_LISTENER_CALLBACKS connectionCallbacks,
                                    PDECODER_RENDERER_CALLBACKS videoCallbacks,
                                   PAUDIO_RENDERER_CALLBACKS audioCallbacks, void*,
                                   int, void*, int) {
    savedCallbacks = *connectionCallbacks;
    savedAudioCallbacks = *audioCallbacks;
    savedVideoCallbacks = *videoCallbacks;
    savedStreamConfiguration = *streamConfig;
    interrupted.store(false);
    enteredStart.store(true);
    if (cancelBeforeFirstStage) {
        auto* session = std::exchange(cancelBeforeFirstStage, nullptr);
        session->requestStop();
        interrupted.store(false);
    }
    connectionCallbacks->stageStarting(STAGE_RTSP_HANDSHAKE);
    while (blockStart.load() && !interrupted.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    if (failedStage != STAGE_NONE) connectionCallbacks->stageFailed(failedStage, startResult);
    return interrupted.load() ? -1 : startResult;
}
extern "C" void LiInterruptConnection() { interrupted.store(true); }
extern "C" void LiStopConnection() { interrupted.store(true); }
extern "C" bool LiGetEstimatedRttInfo(std::uint32_t*, std::uint32_t*) { return false; }
extern "C" bool LiGetCurrentHostDisplayHdrMode() { return false; }

namespace terra {

struct VideoRenderer::Impl {};
VideoRenderer::VideoRenderer(StreamSettings settings, StatusListener, CloseListener,
                               std::shared_ptr<StreamStatistics>, OverlayListener overlayListener,
                               OverlayCaptureListener overlayCaptureListener,
                               StreamOverlayState overlayState)
    : impl_(std::make_unique<Impl>()) {
    lastRendererDisplay = settings.displayIndex;
    savedOverlayListener = std::move(overlayListener);
    savedOverlayCaptureListener = std::move(overlayCaptureListener);
    lastInitialOverlayState = overlayState;
}
VideoRenderer::~VideoRenderer() = default;
void VideoRenderer::initialize(int, int, int, int) {}
void VideoRenderer::setInputEnabled(bool) {}
void VideoRenderer::setHdrMode(bool) {}
void VideoRenderer::closeOverlay(std::uint64_t) { ++overlayCloses; }
void VideoRenderer::acknowledgeOverlayHidden(std::uint64_t revision) {
    ++overlayHiddenAcknowledgements;
    if (savedOverlayCaptureListener) savedOverlayCaptureListener(revision, false);
}
void VideoRenderer::setGamepadRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}
void VideoRenderer::setGamepadTriggerRumble(std::uint16_t, std::uint16_t, std::uint16_t) {}
void VideoRenderer::setGamepadMotionEventState(std::uint16_t, std::uint8_t, std::uint16_t) {
    ++motionStateCallbacks;
}
void VideoRenderer::setGamepadLed(std::uint16_t, std::uint8_t, std::uint8_t, std::uint8_t) {
    ++ledCallbacks;
}
int VideoRenderer::submit(PDECODE_UNIT) { return DR_OK; }
bool VideoRenderer::recoveryRequired() const { return forceVideoRecovery.exchange(false); }
std::optional<int> VideoRenderer::recoveryDisplayIndex() const {
    return std::exchange(nextRecoveryDisplay, std::nullopt);
}

struct AudioRenderer::Impl {
    bool recoveryRequired = false;
};
AudioRenderer::AudioRenderer(StatusListener) : impl_(std::make_unique<Impl>()) {}
AudioRenderer::~AudioRenderer() = default;
void AudioRenderer::initialize(const OPUS_MULTISTREAM_CONFIGURATION&) { ++audioInitializations; }
void AudioRenderer::submit(const char*, int) {
    impl_->recoveryRequired = forceAudioLoss.exchange(false);
}
bool AudioRenderer::recoveryRequired() const noexcept { return impl_->recoveryRequired; }
bool AudioRenderer::supportsOutputChannels(int) noexcept { return true; }
}

int main() {
    {
        bool stoppedFromListener = false;
        terra::StreamSession* sessionPointer = nullptr;
        terra::StreamSession session(
            testConfig(),
            [&](const terra::StreamSessionEvent& event) {
                if (event.message == "Starting Moonlight transport.") {
                    sessionPointer->stop();
                    stoppedFromListener = true;
                }
            },
            [](auto, bool, bool) {});
        sessionPointer = &session;
        bool cancelled = false;
        try {
            session.start();
        } catch (const std::runtime_error&) {
            cancelled = true;
        }
        expect(stoppedFromListener && cancelled,
               "Startup listener stop deadlocked or failed to cancel connection.");
    }

    {
        terra::StreamSession session(testConfig(), [](const auto&) {}, [](auto, bool, bool) {});
        cancelBeforeFirstStage = &session;
        bool cancelled = false;
        try {
            session.start();
        } catch (const std::runtime_error&) {
            cancelled = true;
        }
        expect(cancelled && interrupted.load(),
               "Cancellation before first connection stage was lost.");
    }

    {
        enteredStart.store(false);
        blockStart.store(true);
        std::atomic_bool cancelled = false;
        terra::StreamSession session(testConfig(), [](const auto&) {}, [](auto, bool, bool) {});
        std::thread starter([&] {
            try {
                session.start();
            } catch (const std::runtime_error&) {
                cancelled.store(true);
            }
        });
        waitForStart();
        session.requestStop();
        starter.join();
        expect(cancelled.load(), "Pending connection was not cancelled.");
        expect(session.stopRequested(), "Cancelled session did not retain stop state.");
    }

    {
        blockStart.store(false);
        startResult = -55;
        failedStage = STAGE_RTSP_HANDSHAKE;
        connectivityResult = ML_PORT_FLAG_UDP_48010;
        connectivityTests = 0;
        auto config = testConfig();
        config.settings.detectBlockedConnections = true;
        std::vector<terra::StreamSessionEvent> failureEvents;
        terra::StreamSession session(config,
                                       [&](const auto& event) { failureEvents.push_back(event); },
                                       [](auto, bool, bool) {});
        std::string failure;
        try {
            session.start();
        } catch (const std::runtime_error& exception) {
            failure = exception.what();
        }
        expect(connectivityTests == 1, "Mapped connection failure did not test related ports.");
        expect(failure.find("Local network may be blocking UDP 48010") != std::string::npos,
               "Blocked-port diagnostic was not retained by startup exception.");
        expect(!failureEvents.empty() && failureEvents.back().message == failure,
               "Published stage failure and startup exception diverged.");
        startResult = 0;
        failedStage = STAGE_NONE;
        connectivityResult = 0;
    }

    enteredStart.store(false);
    blockStart.store(false);
    int disconnects = 0;
    bool hostEnded = false;
    std::string terminalState;
    std::vector<terra::StreamSessionEvent> events;
    std::optional<terra::StreamWindowBounds> overlayBounds;
    {
        terra::StreamSession session(
            testConfig(), [&](const auto& event) { events.push_back(event); },
            [&](terra::StreamSessionEvent event, bool ended, bool) {
                ++disconnects;
                hostEnded = ended;
                terminalState = std::move(event.state);
            },
            [&](const terra::StreamWindowBounds& bounds) { overlayBounds = bounds; });
        session.start();
        expect(savedStreamConfiguration.packetSize == 1392 &&
                   savedStreamConfiguration.streamingRemotely == STREAM_CFG_LOCAL,
               "Local route did not select LAN packet policy.");
        expect(savedStreamConfiguration.audioConfiguration == AUDIO_CONFIGURATION_STEREO,
               "Stereo audio configuration was not negotiated.");
        expect(savedCallbacks.connectionTerminated != nullptr,
               "Termination callback was not registered.");
        savedCallbacks.connectionTerminated(-1);
        expect(disconnects == 0, "Pre-connection termination was not ignored.");
        savedCallbacks.connectionStarted();
        expect(savedVideoCallbacks.setup(VIDEO_FORMAT_H264, 1280, 720, 60, nullptr, 0) == DR_OK,
                "Video renderer callback setup failed.");
        expect(static_cast<bool>(savedOverlayListener),
               "Stream overlay callback was not installed on video renderer.");
        savedOverlayListener({.revision = 3, .visible = true,
                              .x = -100, .y = 40, .width = 1280, .height = 720,
                              .scaleFactor = 1.25, .wayland = true, .fullscreen = true});
        expect(overlayBounds && overlayBounds->revision == 3 && overlayBounds->visible &&
                   overlayBounds->x == -100 && overlayBounds->width == 1280 &&
                   overlayBounds->scaleFactor == 1.25 && overlayBounds->wayland &&
                   overlayBounds->fullscreen,
               "Stream overlay state did not reach session listener.");
        session.closeOverlay(3);
        expect(overlayBounds && overlayBounds->revision == 4 && !overlayBounds->visible,
               "Session did not publish hidden state before queueing renderer close.");
        session.acknowledgeOverlayHidden(4);
        expect(overlayCloses == 1 && overlayHiddenAcknowledgements == 1,
               "Stream overlay commands did not reach video renderer.");
        expect(savedCallbacks.setMotionEventState != nullptr &&
                   savedCallbacks.setControllerLED != nullptr,
               "Advanced controller feedback callbacks were not registered.");
        savedCallbacks.setMotionEventState(0, LI_MOTION_TYPE_GYRO, 120);
        savedCallbacks.setControllerLED(0, 10, 20, 30);
        expect(motionStateCallbacks == 1 && ledCallbacks == 1,
               "Advanced controller feedback did not reach video input backend.");
        savedCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
        expect(events.back().state == "receiving", "Poor network status was not published.");
        savedCallbacks.connectionStatusUpdate(CONN_STATUS_OKAY);
        expect(events.back().state == "rendering" &&
                   events.back().message == "Network quality recovered.",
               "Network recovery did not clear warning state.");
        savedCallbacks.connectionTerminated(ML_ERROR_GRACEFUL_TERMINATION);
        savedCallbacks.connectionTerminated(-1);
        expect(disconnects == 1, "Termination callback was delivered more than once.");
        expect(hostEnded, "Graceful host termination was not identified.");
        expect(terminalState == "terminated", "Graceful termination used wrong state.");

        OPUS_MULTISTREAM_CONFIGURATION audioConfig{};
        audioConfig.sampleRate = 48'000;
        audioConfig.channelCount = 2;
        audioConfig.streams = 1;
        audioConfig.coupledStreams = 1;
        audioConfig.samplesPerFrame = 240;
        expect(savedAudioCallbacks.init(0, &audioConfig, nullptr, 0) == 0,
               "Audio callback initialization failed.");
        expect(audioInitializations == 1, "Audio renderer was not initialized.");
        forceAudioLoss.store(true);
        char sample = 0;
        savedAudioCallbacks.decodeAndPlaySample(&sample, 1);
        expect(audioInitializations == 2, "Lost audio device was not recovered.");
        savedAudioCallbacks.cleanup();

        session.stop();
        savedCallbacks.connectionTerminated(-1);
        expect(disconnects == 1, "Termination callback was delivered after stop.");
    }

    {
        terra::StreamSession* activeSession = nullptr;
        bool stoppedFromListener = false;
        terra::StreamSession session(
            testConfig(),
            [&](const terra::StreamSessionEvent& event) {
                if (event.state == "receiving") {
                    activeSession->stop();
                    stoppedFromListener = true;
                }
            },
            [](auto, bool, bool) {});
        activeSession = &session;
        session.start();
        savedCallbacks.connectionStarted();
        savedCallbacks.connectionStatusUpdate(CONN_STATUS_POOR);
        expect(stoppedFromListener,
               "Session listener deadlocked while stopping from a Moonlight callback.");
    }

    {
        auto config = testConfig();
        config.settings.displayIndex = 0;
        terra::StreamSession session(config, [](const auto&) {}, [](auto, bool, bool) {});
        session.start();
        savedCallbacks.connectionStarted();
        expect(savedVideoCallbacks.setup(VIDEO_FORMAT_H264, 1280, 720, 60, nullptr, 0) == DR_OK,
               "Display recovery renderer setup failed.");
        nextRecoveryDisplay = 1;
        savedOverlayListener({.revision = 7, .visible = true});
        forceVideoRecovery.store(true);
        DECODE_UNIT unit{};
        static_cast<void>(savedVideoCallbacks.submitDecodeUnit(&unit));
        expect(lastRendererDisplay == 1,
               "Renderer recovery did not preserve runtime display migration.");
        expect(lastInitialOverlayState.revision == 7 && lastInitialOverlayState.visible &&
                   lastInitialOverlayState.captureSuspended,
               "Renderer recovery did not preserve active overlay capture suspension.");
        savedOverlayListener({.revision = 8, .visible = false});
        session.acknowledgeOverlayHidden(8);
        forceVideoRecovery.store(true);
        static_cast<void>(savedVideoCallbacks.submitDecodeUnit(&unit));
        expect(lastInitialOverlayState.revision == 8 && !lastInitialOverlayState.visible &&
                   !lastInitialOverlayState.captureSuspended,
               "Renderer recovery retained capture suspension after matching hidden acknowledgement.");
        session.stop();
    }

    {
        int recoveryDisconnects = 0;
        terra::StreamSession session(
            testConfig(), [](const auto&) {},
            [&](auto, bool, bool) { ++recoveryDisconnects; });
        session.start();
        savedCallbacks.connectionStarted();
        expect(savedVideoCallbacks.setup(VIDEO_FORMAT_H264, 1280, 720, 60, nullptr, 0) == DR_OK,
               "Recovery test renderer setup failed.");
        DECODE_UNIT unit{};
        for (int attempt = 0; attempt < 3; ++attempt) {
            forceVideoRecovery.store(true);
            static_cast<void>(savedVideoCallbacks.submitDecodeUnit(&unit));
        }
        expect(recoveryDisconnects == 1,
               "Repeated renderer failures did not terminate recovery loop.");
    }

    for (const auto [audioConfig, expected] : {
             std::pair{terra::AudioConfig::surround51, AUDIO_CONFIGURATION_51_SURROUND},
             std::pair{terra::AudioConfig::surround71, AUDIO_CONFIGURATION_71_SURROUND},
         }) {
        auto config = testConfig();
        config.settings.audioConfig = audioConfig;
        terra::StreamSession session(config, [](const auto&) {}, [](auto, bool, bool) {});
        session.start();
        expect(savedStreamConfiguration.audioConfiguration == expected,
               "Surround audio configuration was not negotiated.");
        session.stop();
    }

    {
        terra::StreamSession activeSession(testConfig(), [](const auto&) {},
                                             [](auto, bool, bool) {});
        activeSession.start();
        bool rejected = false;
        {
            terra::StreamSession rejectedSession(testConfig(), [](const auto&) {},
                                                   [](auto, bool, bool) {});
            try {
                rejectedSession.start();
            } catch (const std::runtime_error&) {
                rejected = true;
            }
        }
        expect(rejected, "Concurrent session was not rejected.");
        expect(!interrupted.load(), "Rejected session interrupted active transport.");
        activeSession.stop();
    }

    {
        terra::StreamSession* sessionPointer = nullptr;
        terra::StreamSession session(
            testConfig(), [](const auto&) {},
            [&](auto, bool, bool) { sessionPointer->stop(); });
        sessionPointer = &session;
        session.start();
        savedCallbacks.connectionStarted();
        savedCallbacks.connectionTerminated(ML_ERROR_GRACEFUL_TERMINATION);
        expect(session.stopRequested(), "Synchronous disconnect did not stop session.");
    }

    {
        auto config = testConfig();
        config.audioEnabled = false;
        config.controllerEnabled = false;
        config.launch.logicalSessionId = "11111111-1111-4111-8111-111111111111";
        config.launch.childStreamId = "22222222-2222-4222-8222-222222222222";
        const auto parsed = terra::parseStreamWorkerConfig(terra::streamWorkerConfigJson(config));
        expect(!parsed.audioEnabled && !parsed.controllerEnabled,
               "Worker role flags did not survive serialization.");
        expect(parsed.launch.logicalSessionId == config.launch.logicalSessionId &&
                   parsed.launch.childStreamId == config.launch.childStreamId,
               "Worker stream identifiers did not survive serialization.");

        std::stringstream framed;
        expect(terra::writeStreamWorkerFrame(framed, {{"type", "stop"}}),
               "Worker frame write failed.");
        const auto frame = terra::readStreamWorkerFrame(framed);
        expect(frame && frame->value("type", "") == "stop", "Worker frame round trip failed.");
    }
}
