#include "stream_session.h"

#include "audio_renderer.h"
#include "video_renderer.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>

namespace eclipse {
namespace {
std::mutex connectionMutex;
}

std::atomic<StreamSession*> StreamSession::active_ = nullptr;

StreamSession::StreamSession(StreamSessionConfig config, Listener listener)
    : config_(std::move(config)), listener_(std::move(listener)) {}

StreamSession::~StreamSession() {
    stop();
}

void StreamSession::start() {
    std::scoped_lock lock{connectionMutex};
    if (started_) {
        throw std::runtime_error("Stream transport is already connected.");
    }
    StreamSession* expected = nullptr;
    if (!active_.compare_exchange_strong(expected, this)) {
        throw std::runtime_error("Another stream transport is already active.");
    }

    STREAM_CONFIGURATION streamConfig;
    LiInitializeStreamConfiguration(&streamConfig);
    streamConfig.width = config_.settings.width;
    streamConfig.height = config_.settings.height;
    streamConfig.fps = config_.settings.fps;
    streamConfig.bitrate = config_.settings.bitrateKbps;
    streamConfig.packetSize = 1392;
    streamConfig.streamingRemotely = STREAM_CFG_AUTO;
    streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
    streamConfig.colorSpace = COLORSPACE_REC_709;
    streamConfig.colorRange = COLOR_RANGE_LIMITED;
    streamConfig.encryptionFlags = ENCFLG_ALL;
    std::copy(config_.launch.remoteInputKey.begin(), config_.launch.remoteInputKey.end(),
              streamConfig.remoteInputAesKey);
    std::copy(config_.launch.remoteInputIv.begin(), config_.launch.remoteInputIv.end(),
              streamConfig.remoteInputAesIv);

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
    connectionCallbacks.connectionStatusUpdate = connectionStatusUpdate;

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
    const auto result = LiStartConnection(&serverInfo, &streamConfig, &connectionCallbacks,
                                          &videoCallbacks, &audioCallbacks, nullptr, 0, nullptr, 0);
    if (result != 0) {
        active_.store(nullptr);
        throw std::runtime_error("Moonlight transport failed with error " +
                                 std::to_string(result) + ".");
    }
    started_ = true;
}

void StreamSession::stop() {
    std::scoped_lock lock{connectionMutex};
    if (!started_.exchange(false)) {
        StreamSession* expected = this;
        active_.compare_exchange_strong(expected, nullptr);
        return;
    }
    LiStopConnection();
    StreamSession* expected = this;
    active_.compare_exchange_strong(expected, nullptr);
}

void StreamSession::publish(std::string state, std::string message) const {
    if (listener_) listener_({std::move(state), std::move(message)});
}

StreamSession* StreamSession::active() {
    return active_.load();
}

void StreamSession::stageStarting(int stage) {
    if (auto* session = active()) {
        session->publish("connecting", std::string{"Connecting: "} + LiGetStageName(stage) + ".");
    }
}

void StreamSession::stageFailed(int stage, int errorCode) {
    if (auto* session = active()) {
        session->publish("error", std::string{"Connection failed during "} + LiGetStageName(stage) +
                                      " (error " + std::to_string(errorCode) + ").");
    }
}

void StreamSession::connectionStarted() {
    if (auto* session = active()) {
        session->publish("connected", "Moonlight transport connected; waiting for encoded video.");
    }
}

void StreamSession::connectionTerminated(int errorCode) {
    if (auto* session = active()) {
        session->publish(errorCode == ML_ERROR_GRACEFUL_TERMINATION ? "terminated" : "error",
                         errorCode == ML_ERROR_GRACEFUL_TERMINATION
                             ? "Host ended the stream."
                             : "Stream transport terminated with error " +
                                   std::to_string(errorCode) + ".");
    }
}

void StreamSession::logMessage(const char* format, ...) {
    std::va_list arguments;
    va_start(arguments, format);
    std::fputs("[moonlight-common] ", stderr);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
}

void StreamSession::connectionStatusUpdate(int status) {
    if (status == CONN_STATUS_POOR) {
        if (auto* session = active()) {
            if (!session->config_.settings.connectionWarnings) return;
            session->publish("receiving", "Encoded stream received; network quality is poor.");
        }
    }
}

int StreamSession::videoSetup(int format, int width, int height, int frameRate, void*, int) {
    if (auto* session = active()) {
        try {
            session->video_ = std::make_unique<VideoRenderer>(
                session->config_.settings,
                [session](std::string state, std::string message) {
                    session->publish(std::move(state), std::move(message));
                });
            session->video_->initialize(format, width, height, frameRate);
            session->publish("connected", "Video negotiated at " + std::to_string(width) + "x" +
                                              std::to_string(height) + " @ " +
                                              std::to_string(frameRate) + " FPS.");
        } catch (const std::exception& exception) {
            session->video_.reset();
            session->publish("error", exception.what());
            return -1;
        }
    }
    return DR_OK;
}

void StreamSession::videoCleanup() {
    if (auto* session = active()) session->video_.reset();
}

int StreamSession::submitVideo(PDECODE_UNIT decodeUnit) {
    if (auto* session = active(); session && session->video_) {
        return session->video_->submit(decodeUnit);
    }
    return DR_NEED_IDR;
}

int StreamSession::audioInit(int, const POPUS_MULTISTREAM_CONFIGURATION config, void*, int) {
    if (auto* session = active()) {
        try {
            session->audio_ = std::make_unique<AudioRenderer>(
                [session](std::string state, std::string message) {
                    session->publish(std::move(state), std::move(message));
                });
            session->audio_->initialize(*config);
        } catch (const std::exception& exception) {
            session->audio_.reset();
            session->publish("connected", std::string{"Audio unavailable: "} + exception.what());
        }
    }
    return 0;
}

void StreamSession::audioCleanup() {
    if (auto* session = active()) session->audio_.reset();
}

void StreamSession::receiveAudio(char* data, int length) {
    if (auto* session = active(); session && session->audio_) {
        session->audio_->submit(data, length);
    }
}

}  // namespace eclipse
