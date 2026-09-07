#include "stream_session.h"

#include "audio_renderer.h"
#include "input_forwarder.h"
#include "network_route.h"
#include "stream_statistics.h"
#include "video_renderer.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace terra {
namespace {
std::recursive_mutex connectionMutex;
std::mutex activeMutex;
std::condition_variable activeCondition;
std::size_t activeLeaseCount = 0;

std::string portDiagnostic(unsigned int portFlags) {
    if (portFlags == 0) return {};

    char ports[256]{};
    LiStringifyPortFlags(portFlags, ", ", ports, sizeof(ports));
    const auto failed = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, portFlags);
    if (failed == ML_TEST_RESULT_INCONCLUSIVE) {
        return std::string{" Connectivity test was inconclusive; verify Sol firewall access for "} +
               ports + ".";
    }
    if (failed == 0) {
        return std::string{" Local port test passed; verify Sol firewall or routing for "} + ports +
               ".";
    }

    char blockedPorts[256]{};
    LiStringifyPortFlags(failed, ", ", blockedPorts, sizeof(blockedPorts));
    return std::string{" Local network may be blocking "} + blockedPorts + ".";
}

bool usesDefaultStreamingPorts(const StreamSessionConfig& config) {
    return config.launch.sessionUrl.find(":48010") != std::string::npos;
}
}

std::atomic<StreamSession*> StreamSession::active_ = nullptr;

StreamSession::StreamSession(StreamSessionConfig config, Listener listener,
                               DisconnectListener disconnectListener,
                               OverlayListener overlayListener,
                               StatisticsListener statisticsListener)
    : config_(std::move(config)),
      listener_(std::move(listener)),
      disconnectListener_(std::move(disconnectListener)),
      overlayListener_(std::move(overlayListener)),
      statisticsListener_(std::move(statisticsListener)),
      hdrMode_(false),
      statistics_(std::make_shared<StreamStatistics>()) {}

StreamSession::~StreamSession() {
    stop();
}

void StreamSession::start() {
    if (started_) throw std::runtime_error("Stream transport is already connected.");
    if (stopRequested_) throw std::runtime_error("Stream connection was cancelled.");
    const auto route = detectRouteReachability(config_.address);
    const auto networkConfig = streamNetworkConfiguration(route);
    std::scoped_lock lock{connectionMutex};
    if (started_) {
        throw std::runtime_error("Stream transport is already connected.");
    }
    if (stopRequested_) {
        throw std::runtime_error("Stream connection was cancelled.");
    }
    terminated_.store(false);
    if (!activate(this)) {
        throw std::runtime_error("Another stream transport is already active.");
    }
    activated_.store(true);
    if (stopRequested_) {
        deactivate(this);
        activated_.store(false);
        throw std::runtime_error("Stream connection was cancelled.");
    }
    {
        std::scoped_lock failureLock{startupFailureMutex_};
        startupFailureMessage_.clear();
    }

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = config_.settings.width;
    streamConfig.height = config_.settings.height;
    streamConfig.fps = config_.settings.fps;
    streamConfig.bitrate = config_.settings.bitrateKbps;
    streamConfig.packetSize = networkConfig.packetSize;
    switch (networkConfig.location) {
        case StreamingLocation::local:
            streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
            break;
        case StreamingLocation::remote:
            streamConfig.streamingRemotely = STREAM_CFG_REMOTE;
            break;
        case StreamingLocation::automatic:
            streamConfig.streamingRemotely = STREAM_CFG_AUTO;
            break;
    }
    streamConfig.audioConfiguration = terraAudioConfiguration(config_.settings.audioConfig);
    streamConfig.supportedVideoFormats = config_.videoFormat;
    streamConfig.colorSpace = config_.settings.enableHdr ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
    streamConfig.colorRange = COLOR_RANGE_LIMITED;
    streamConfig.encryptionFlags = ENCFLG_ALL;
    std::copy(config_.launch.remoteInputKey.begin(), config_.launch.remoteInputKey.end(),
              streamConfig.remoteInputAesKey);
    std::copy(config_.launch.remoteInputIv.begin(), config_.launch.remoteInputIv.end(),
              streamConfig.remoteInputAesIv);
    if (stopRequested_) {
        deactivate(this);
        activated_.store(false);
        throw std::runtime_error("Stream connection was cancelled.");
    }

    SERVER_INFORMATION serverInfo;
    LiInitializeServerInformation(&serverInfo);
    serverInfo.address = config_.address.c_str();
    serverInfo.serverInfoAppVersion = config_.appVersion.c_str();
    serverInfo.serverInfoGfeVersion = config_.gfeVersion.empty() ? nullptr : config_.gfeVersion.c_str();
    serverInfo.rtspSessionUrl = config_.launch.sessionUrl.c_str();
    serverInfo.serverCodecModeSupport = config_.serverCodecModeSupport;

    CONNECTION_LISTENER_CALLBACKS connectionCallbacks;
    LiInitializeConnectionCallbacks(&connectionCallbacks);
    connectionCallbacks.stageStarting = stageStarting;
    connectionCallbacks.stageFailed = stageFailed;
    connectionCallbacks.connectionStarted = connectionStarted;
    connectionCallbacks.connectionTerminated = connectionTerminated;
    connectionCallbacks.logMessage = logMessage;
    connectionCallbacks.rumble = rumble;
    connectionCallbacks.connectionStatusUpdate = connectionStatusUpdate;
    connectionCallbacks.setHdrMode = setHdrMode;
    connectionCallbacks.rumbleTriggers = rumbleTriggers;
    connectionCallbacks.setMotionEventState = setMotionEventState;
    connectionCallbacks.setControllerLED = setControllerLed;

    DECODER_RENDERER_CALLBACKS videoCallbacks;
    LiInitializeVideoCallbacks(&videoCallbacks);
    videoCallbacks.setup = videoSetup;
    videoCallbacks.cleanup = videoCleanup;
    videoCallbacks.submitDecodeUnit = submitVideo;

    AUDIO_RENDERER_CALLBACKS audioCallbacks;
    LiInitializeAudioCallbacks(&audioCallbacks);
    audioCallbacks.init = audioInit;
    audioCallbacks.cleanup = audioCleanup;
    audioCallbacks.decodeAndPlaySample = receiveAudio;
    audioCallbacks.capabilities = CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION;

    publish("connecting", "Starting Moonlight transport.");
    if (stopRequested_) {
        deactivate(this);
        activated_.store(false);
        throw std::runtime_error("Stream connection was cancelled.");
    }
    const auto result = LiStartConnection(&serverInfo, &streamConfig, &connectionCallbacks,
                                          &videoCallbacks, &audioCallbacks, nullptr, 0, nullptr, 0);
    if (result != 0) {
        deactivate(this);
        activated_.store(false);
        {
            std::scoped_lock failureLock{startupFailureMutex_};
            if (!startupFailureMessage_.empty()) {
                throw std::runtime_error(startupFailureMessage_);
            }
        }
        throw std::runtime_error("Moonlight transport failed with error " +
                                 std::to_string(result) + ".");
    }
    if (stopRequested_) {
        LiStopConnection();
        deactivate(this);
        activated_.store(false);
        throw std::runtime_error("Stream connection was cancelled.");
    }
    started_ = true;
}

void StreamSession::requestStop() {
    stopRequested_.store(true);
    connectionEstablished_.store(false);
    terminated_.store(true);
    if (activated_.load()) LiInterruptConnection();
}

void StreamSession::stop() {
    requestStop();
    std::scoped_lock lock{connectionMutex};
    if (!started_.exchange(false)) {
        deactivate(this);
        activated_.store(false);
        return;
    }
    {
        std::scoped_lock videoLock{videoMutex_};
        inputEnabled_ = false;
        if (video_) video_->setInputEnabled(inputEnabled_);
    }
    LiStopConnection();
    deactivate(this);
    activated_.store(false);
}

bool StreamSession::stopRequested() const noexcept { return stopRequested_.load(); }

void StreamSession::resumeOverlay() {
    std::scoped_lock lock{videoMutex_};
    if (video_) video_->resumeOverlay();
}

void StreamSession::publish(std::string state, std::string message) const noexcept {
    try {
        if (listener_) listener_({std::move(state), std::move(message)});
    } catch (...) {
    }
}

void StreamSession::queueStatus(std::string state, std::string message) noexcept {
    try {
        std::scoped_lock lock{statusMutex_};
        if (pendingStatuses_.size() >= 16) pendingStatuses_.erase(pendingStatuses_.begin());
        pendingStatuses_.push_back({std::move(state), std::move(message)});
    } catch (...) {
    }
}

void StreamSession::requestDisconnect(std::string state, std::string message,
                                      bool hostEnded, bool userEnded) noexcept {
    if (terminated_.exchange(true)) return;
    stopRequested_.store(true);
    try {
        if (disconnectListener_) {
            disconnectListener_({std::move(state), std::move(message)}, hostEnded, userEnded);
        }
    } catch (...) {
    }
}

StreamSession::ActiveLease::~ActiveLease() {
    reset();
}

void StreamSession::ActiveLease::reset() noexcept {
    if (!session_) return;
    session_ = nullptr;
    bool notify = false;
    {
        std::scoped_lock lock{activeMutex};
        notify = --activeLeaseCount == 0;
    }
    if (notify) activeCondition.notify_all();
}

StreamSession::ActiveLease::ActiveLease(ActiveLease&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)) {}

StreamSession::ActiveLease StreamSession::active() {
    std::scoped_lock lock{activeMutex};
    auto* session = active_.load();
    if (session) ++activeLeaseCount;
    return ActiveLease{session};
}

void StreamSession::publishAndRelease(ActiveLease lease, std::string state,
                                      std::string message) noexcept {
    Listener listener;
    if (lease) listener = lease->listener_;
    lease.reset();
    if (!listener) return;
    try {
        listener({std::move(state), std::move(message)});
    } catch (...) {
    }
}

void StreamSession::publishQueuedAndRelease(ActiveLease lease) noexcept {
    Listener listener;
    std::vector<StreamSessionEvent> events;
    if (lease) {
        listener = lease->listener_;
        std::scoped_lock lock{lease->statusMutex_};
        events.swap(lease->pendingStatuses_);
    }
    lease.reset();
    if (!listener) return;
    for (auto& event : events) {
        try {
            listener(event);
        } catch (...) {
        }
    }
}

void StreamSession::disconnectAndRelease(ActiveLease lease, StreamSessionEvent event,
                                         bool hostEnded, bool userEnded) noexcept {
    DisconnectListener listener;
    if (lease) {
        lease->stopRequested_.store(true);
        listener = lease->disconnectListener_;
    }
    lease.reset();
    if (!listener) return;
    try {
        listener(std::move(event), hostEnded, userEnded);
    } catch (...) {
    }
}

bool StreamSession::activate(StreamSession* session) {
    std::scoped_lock lock{activeMutex};
    if (active_.load()) return false;
    active_.store(session);
    return true;
}

void StreamSession::deactivate(StreamSession* session) {
    std::unique_lock lock{activeMutex};
    if (active_.load() != session) return;
    active_.store(nullptr);
    activeCondition.wait(lock, [] { return activeLeaseCount == 0; });
}

void StreamSession::stageStarting(int stage) {
    auto session = active();
    if (!session) return;
    if (session->stopRequested_.load()) {
        session.reset();
        LiInterruptConnection();
        return;
    }
    publishAndRelease(std::move(session), "connecting",
                      std::string{"Connecting: "} + LiGetStageName(stage) + ".");
}

void StreamSession::stageFailed(int stage, int errorCode) {
    if (auto session = active(); session) {
        auto message = std::string{"Connection failed during "} + LiGetStageName(stage) +
                       " (error " + std::to_string(errorCode) + ").";
        if (session->config_.settings.detectBlockedConnections) {
            if (usesDefaultStreamingPorts(session->config_)) {
                message += portDiagnostic(LiGetPortFlagsFromStage(stage));
            } else {
                message += " Automated port testing is unavailable for custom Sol ports.";
            }
        }
        {
            std::scoped_lock failureLock{session->startupFailureMutex_};
            session->startupFailureMessage_ = message;
        }
        publishAndRelease(std::move(session), "error", std::move(message));
    }
}

void StreamSession::connectionStarted() {
    if (auto session = active(); session) {
        session->connectionEstablished_.store(true);
        {
            std::scoped_lock lock{session->videoMutex_};
            if (session->terminated_.load()) return;
            session->inputEnabled_ = true;
            if (session->video_) session->video_->setInputEnabled(session->inputEnabled_);
        }
        publishAndRelease(std::move(session), "connected",
                          "Moonlight transport connected; waiting for encoded video.");
    }
}

void StreamSession::connectionTerminated(int errorCode) {
    DisconnectListener disconnectListener;
    StreamSessionEvent event;
    bool hostEnded = false;
    {
        auto session = active();
        if (!session) return;
        if (!session->connectionEstablished_.exchange(false)) return;
        {
            std::scoped_lock lock{session->videoMutex_};
            session->inputEnabled_ = false;
            if (session->video_) session->video_->setInputEnabled(session->inputEnabled_);
        }
        if (session->terminated_.exchange(true)) return;
        session->stopRequested_.store(true);
        hostEnded = errorCode == ML_ERROR_GRACEFUL_TERMINATION;
        event = {
            hostEnded ? "terminated" : "error",
            hostEnded ? "Host ended the stream."
                       : "Stream transport terminated with error " +
                             std::to_string(errorCode) + ".",
        };
        if (!hostEnded && session->config_.settings.detectBlockedConnections) {
            if (usesDefaultStreamingPorts(session->config_)) {
                event.message +=
                    portDiagnostic(LiGetPortFlagsFromTerminationErrorCode(errorCode));
            } else {
                event.message +=
                    " Automated port testing is unavailable for custom Sol ports.";
            }
        }
        disconnectListener = session->disconnectListener_;
    }
    try {
        if (disconnectListener) disconnectListener(std::move(event), hostEnded, false);
    } catch (...) {
    }
}

void StreamSession::logMessage(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::fputs("[moonlight-common] ", stderr);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void StreamSession::setHdrMode(bool enabled) {
    if (auto session = active(); session) {
        session->hdrMode_.store(enabled);
        std::scoped_lock lock{session->videoMutex_};
        if (!session->terminated_.load() && session->video_) {
            session->video_->setHdrMode(enabled);
        }
    }
}

void StreamSession::rumble(unsigned short controllerNumber, unsigned short lowFrequency,
                           unsigned short highFrequency) {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->videoMutex_};
        if (!session->terminated_.load() && session->video_) {
            session->video_->setGamepadRumble(controllerNumber, lowFrequency, highFrequency);
        }
    }
}

void StreamSession::rumbleTriggers(std::uint16_t controllerNumber, std::uint16_t leftTrigger,
                                   std::uint16_t rightTrigger) {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->videoMutex_};
        if (!session->terminated_.load() && session->video_) {
            session->video_->setGamepadTriggerRumble(controllerNumber, leftTrigger, rightTrigger);
        }
    }
}

void StreamSession::setMotionEventState(std::uint16_t controllerNumber, std::uint8_t motionType,
                                        std::uint16_t reportRateHz) {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->videoMutex_};
        if (!session->terminated_.load() && session->video_) {
            session->video_->setGamepadMotionEventState(controllerNumber, motionType,
                                                        reportRateHz);
        }
    }
}

void StreamSession::setControllerLed(std::uint16_t controllerNumber, std::uint8_t red,
                                     std::uint8_t green, std::uint8_t blue) {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->videoMutex_};
        if (!session->terminated_.load() && session->video_) {
            session->video_->setGamepadLed(controllerNumber, red, green, blue);
        }
    }
}

void StreamSession::connectionStatusUpdate(int status) {
    auto session = active();
    if (!session || !session->config_.settings.connectionWarnings) return;
    if (status == CONN_STATUS_POOR) {
        publishAndRelease(std::move(session), "receiving",
                          "Encoded stream received; network quality is poor.");
    } else if (status == CONN_STATUS_OKAY) {
        publishAndRelease(std::move(session), "rendering", "Network quality recovered.");
    }
}

void StreamSession::createVideoRendererLocked() {
    auto settings = config_.settings;
    if (videoDisplayOverride_) settings.displayIndex = *videoDisplayOverride_;
    video_ = std::make_unique<VideoRenderer>(
        std::move(settings),
        [this](std::string state, std::string message) {
            queueStatus(std::move(state), std::move(message));
        },
        [this] { requestDisconnect("terminated", "Render window closed.", false, true); },
        statistics_,
        [this](const StreamWindowBounds& bounds) {
            try {
                if (overlayListener_) overlayListener_(bounds);
            } catch (...) {
            }
        });
    video_->initialize(negotiatedVideoFormat_, negotiatedWidth_, negotiatedHeight_,
                       negotiatedFrameRate_);
    hdrMode_.store(LiGetCurrentHostDisplayHdrMode());
    video_->setHdrMode(hdrMode_.load());
    video_->setInputEnabled(inputEnabled_);
}

bool StreamSession::recoverVideoRendererLocked(std::string& error) {
    if (negotiatedVideoFormat_ == 0 || negotiatedWidth_ <= 0 || negotiatedHeight_ <= 0 ||
        negotiatedFrameRate_ <= 0) {
        error = "Video format is no longer configured.";
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    if (videoRecoveryWindow_ == std::chrono::steady_clock::time_point{} ||
        now - videoRecoveryWindow_ > std::chrono::seconds{10}) {
        videoRecoveryWindow_ = now;
        videoRecoveryAttempts_ = 0;
    }
    if (videoRecoveryAttempts_ >= 2) {
        error = "Video renderer failed repeatedly after recovery.";
        return false;
    }
    ++videoRecoveryAttempts_;
    if (const auto displayIndex = video_->recoveryDisplayIndex()) {
        videoDisplayOverride_ = *displayIndex;
    }
    video_.reset();
    try {
        createVideoRendererLocked();
        return true;
    } catch (const std::exception& exception) {
        video_.reset();
        error = exception.what();
        return false;
    } catch (...) {
        video_.reset();
        error = "unknown error";
        return false;
    }
}

void StreamSession::createAudioRendererLocked() {
    audio_ = std::make_unique<AudioRenderer>(
        [this](std::string state, std::string message) {
            queueStatus(std::move(state), std::move(message));
        });
    audio_->initialize(audioConfig_);
}

bool StreamSession::recoverAudioRendererLocked(std::string& error) {
    if (!audioConfigured_ || std::chrono::steady_clock::now() < nextAudioRecovery_) return false;
    nextAudioRecovery_ = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    audio_.reset();
    try {
        createAudioRendererLocked();
        return true;
    } catch (const std::exception& exception) {
        audio_.reset();
        error = exception.what();
        return false;
    } catch (...) {
        audio_.reset();
        error = "unknown error";
        return false;
    }
}

int StreamSession::videoSetup(int format, int width, int height, int frameRate, void*, int) {
    auto lease = active();
    if (!lease) return DR_OK;
    auto* session = lease.get();
    std::string state = "connected";
    std::string message;
    int result = DR_OK;
    {
        std::scoped_lock lock{session->videoMutex_};
        try {
            session->negotiatedVideoFormat_ = format;
            session->negotiatedWidth_ = width;
            session->negotiatedHeight_ = height;
            session->negotiatedFrameRate_ = frameRate;
            session->createVideoRendererLocked();
            message = "Video negotiated at " + std::to_string(width) + "x" +
                      std::to_string(height) + " @ " + std::to_string(frameRate) + " FPS.";
        } catch (const std::exception& exception) {
            session->video_.reset();
            state = "error";
            message = exception.what();
            result = -1;
        }
    }
    session->queueStatus(std::move(state), std::move(message));
    publishQueuedAndRelease(std::move(lease));
    return result;
}

void StreamSession::videoCleanup() {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->videoMutex_};
        session->video_.reset();
        session->negotiatedVideoFormat_ = 0;
        session->negotiatedWidth_ = 0;
        session->negotiatedHeight_ = 0;
        session->negotiatedFrameRate_ = 0;
    }
}

int StreamSession::submitVideo(PDECODE_UNIT decodeUnit) {
    auto lease = active();
    if (lease) {
        auto* session = lease.get();
        if (session->statistics_) {
            const auto reassemblyUs = decodeUnit->enqueueTimeUs >= decodeUnit->receiveTimeUs
                                          ? decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs
                                          : 0;
            session->statistics_->recordVideoUnit(
                decodeUnit->frameNumber, decodeUnit->fullLength,
                decodeUnit->frameHostProcessingLatency, reassemblyUs);
            const auto now = std::chrono::steady_clock::now();
            if (now >= session->nextRttSample_) {
                std::uint32_t rtt = 0;
                std::uint32_t variance = 0;
                if (LiGetEstimatedRttInfo(&rtt, &variance)) {
                    session->statistics_->recordRtt(rtt, variance);
                }
                session->nextRttSample_ = now + std::chrono::seconds{1};
            }
            if (const auto sample = session->statistics_->sampleIfDue(now);
                sample && session->statisticsListener_) {
                try {
                    session->statisticsListener_(*sample);
                } catch (...) {
                }
            }
        }
        std::string message;
        std::string recoveryError;
        bool disconnect = false;
        int result = DR_NEED_IDR;
        {
            std::scoped_lock lock{session->videoMutex_};
            if (!session->video_) return DR_NEED_IDR;
            try {
                result = session->video_->submit(decodeUnit);
                if (session->video_->recoveryRequired()) {
                    if (session->recoverVideoRendererLocked(recoveryError)) {
                        message = "Video decoder and presentation device recovered.";
                    } else {
                        message = "Video renderer recovery failed: " + recoveryError;
                        disconnect = true;
                    }
                    result = DR_NEED_IDR;
                }
            } catch (const std::exception& exception) {
                message = std::string{"Video decode failed: "} + exception.what();
                if (!session->recoverVideoRendererLocked(recoveryError)) {
                    message += " Recovery failed: " + recoveryError;
                    disconnect = true;
                }
            } catch (...) {
                message = "Video decode failed with an unknown error.";
                if (!session->recoverVideoRendererLocked(recoveryError)) {
                    message += " Recovery failed: " + recoveryError;
                    disconnect = true;
                }
            }
        }
        if (disconnect) {
            disconnectAndRelease(std::move(lease), {"error", std::move(message)}, false, false);
        } else {
            if (!message.empty()) session->queueStatus("connected", std::move(message));
            publishQueuedAndRelease(std::move(lease));
        }
        return result;
    }
    return DR_NEED_IDR;
}

int StreamSession::audioInit(int, const POPUS_MULTISTREAM_CONFIGURATION config, void*, int) {
    auto lease = active();
    if (lease) {
        auto* session = lease.get();
        std::string message;
        {
            std::scoped_lock lock{session->audioMutex_};
            session->audioConfig_ = *config;
            session->audioConfigured_ = true;
            try {
                session->createAudioRendererLocked();
            } catch (const std::exception& exception) {
                session->audio_.reset();
                session->nextAudioRecovery_ =
                    std::chrono::steady_clock::now() + std::chrono::seconds{2};
                message = std::string{"Audio unavailable: "} + exception.what();
            }
        }
        if (!message.empty()) {
            session->queueStatus("connected", std::move(message));
        }
        publishQueuedAndRelease(std::move(lease));
    }
    return 0;
}

void StreamSession::audioCleanup() {
    if (auto session = active(); session) {
        std::scoped_lock lock{session->audioMutex_};
        session->audio_.reset();
        session->audioConfigured_ = false;
    }
}

void StreamSession::receiveAudio(char* data, int length) {
    auto lease = active();
    if (lease) {
        auto* session = lease.get();
        std::string message;
        {
            std::scoped_lock lock{session->audioMutex_};
            if (!session->audio_ || session->audio_->recoveryRequired()) {
                std::string recoveryError;
                if (!session->recoverAudioRendererLocked(recoveryError) &&
                    !recoveryError.empty()) {
                    message = "Audio recovery failed: " + recoveryError;
                }
            }
            if (session->audio_) {
                try {
                    session->audio_->submit(data, length);
                    if (session->audio_->recoveryRequired()) {
                        std::string recoveryError;
                        if (!session->recoverAudioRendererLocked(recoveryError) &&
                            !recoveryError.empty()) {
                            message = "Audio recovery failed: " + recoveryError;
                        }
                    }
                } catch (const std::exception& exception) {
                    session->audio_.reset();
                    message = std::string{"Audio decode failed: "} + exception.what();
                } catch (...) {
                    session->audio_.reset();
                    message = "Audio decode failed with an unknown error.";
                }
            }
        }
        if (!message.empty()) {
            session->queueStatus("connected", std::move(message));
        }
        publishQueuedAndRelease(std::move(lease));
    }
}

}  // namespace terra
