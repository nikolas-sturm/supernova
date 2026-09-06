#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXUuid.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "control_plane.h"
#include "mdns_discovery.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if ECLIPSE_HAS_MOONLIGHT_COMMON
extern "C" {
#include <Limelight.h>
}
#endif

namespace {
using Json = nlohmann::json;

struct ExtensionContext {
    std::string port;
    std::string accessToken;
    std::string connectToken;
    std::string extensionId;
};

void configureDpiAwareness() noexcept {
#if defined(_WIN32)
    using SetDpiAwarenessContext = BOOL(WINAPI*)(HANDLE);
    const auto user32 = GetModuleHandleW(L"user32.dll");
    const auto procedure = user32 ? GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
    if (procedure) {
        const auto setDpiAwarenessContext =
            reinterpret_cast<SetDpiAwarenessContext>(procedure);
        if (setDpiAwarenessContext(reinterpret_cast<HANDLE>(-4))) return;
    }
    SetProcessDPIAware();
#endif
}

std::filesystem::path parseDataPath(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view{argv[index]} == "--data-path") {
            return argv[index + 1];
        }
    }
    return std::filesystem::current_path() / ".eclipse-data";
}

ExtensionContext parseContext(const std::string& input) {
    const auto payload = Json::parse(input);
    return {
        payload.at("nlPort").get<std::string>(),
        payload.at("nlToken").get<std::string>(),
        payload.at("nlConnectToken").get<std::string>(),
        payload.at("nlExtensionId").get<std::string>(),
    };
}

Json makeStatus() {
#if ECLIPSE_HAS_MOONLIGHT_COMMON
    static_cast<void>(LiGetMillis());
#endif
#if defined(_WIN32) && defined(ECLIPSE_HAS_WINDOWS_VIDEO) || \
    defined(__linux__) && defined(ECLIPSE_HAS_LINUX_VIDEO)
    constexpr bool streamingAvailable = true;
#else
    constexpr bool streamingAvailable = false;
#endif

    return {
        {"schemaVersion", 1},
        {"state", "ready"},
        {"detail", "Secure host library and native session launch ready."},
        {"moonlightQtRevision", ECLIPSE_MOONLIGHT_QT_REVISION},
        {"moonlightCommonRevision", ECLIPSE_MOONLIGHT_COMMON_REVISION},
        {"streamingAvailable", streamingAvailable},
    };
}

Json hostJson(const eclipse::HostRecord& host) {
    Json displayModes = Json::array();
    for (const auto& mode : host.displayModes) {
        displayModes.push_back({{"width", mode.width},
                                {"height", mode.height},
                                {"refreshRate", mode.refreshRate}});
    }
    return {
        {"id", host.id},
        {"name", host.name},
        {"address", host.address},
        {"serverName", host.serverName},
        {"serverUniqueId", host.serverUniqueId},
        {"appVersion", host.appVersion},
        {"serverState", host.serverState},
        {"status", host.status},
        {"error", host.error},
        {"httpsPort", host.httpsPort},
        {"currentGameId", host.currentGameId},
        {"serverCodecModeSupport", host.serverCodecModeSupport},
        {"maxLumaPixelsHevc", host.maxLumaPixelsHevc},
        {"displayModes", std::move(displayModes)},
        {"lastSeenAt", host.lastSeenAt},
        {"paired", host.paired},
        {"wakeable", !host.wakeMacAddress.empty()},
    };
}

Json appJson(const eclipse::GameStreamApp& app) {
    return {
        {"id", app.id},
        {"name", app.name},
        {"hdrSupported", app.hdrSupported},
        {"appCollectorGame", app.appCollectorGame},
    };
}

const char* displayModeName(eclipse::DisplayMode mode) {
    switch (mode) {
        case eclipse::DisplayMode::fullscreen: return "fullscreen";
        case eclipse::DisplayMode::borderless: return "borderless";
        case eclipse::DisplayMode::windowed: return "windowed";
    }
    return "windowed";
}

const char* audioConfigName(eclipse::AudioConfig config) {
    switch (config) {
        case eclipse::AudioConfig::surround51: return "5.1";
        case eclipse::AudioConfig::surround71: return "7.1";
        case eclipse::AudioConfig::stereo: return "stereo";
    }
    return "stereo";
}

const char* systemKeyCaptureName(eclipse::SystemKeyCapture capture) {
    switch (capture) {
        case eclipse::SystemKeyCapture::fullscreen: return "fullscreen";
        case eclipse::SystemKeyCapture::always: return "always";
        case eclipse::SystemKeyCapture::off: return "off";
    }
    return "off";
}

const char* codecName(int videoFormat) {
    if ((videoFormat & VIDEO_FORMAT_MASK_AV1) != 0) return "AV1";
    if ((videoFormat & VIDEO_FORMAT_MASK_H265) != 0) return "HEVC";
    return "H.264";
}

eclipse::StreamSettings parseStreamSettings(const Json& value) {
    eclipse::StreamSettings settings;
    settings.width = value.at("width").get<int>();
    settings.height = value.at("height").get<int>();
    settings.fps = value.at("fps").get<int>();
    settings.bitrateKbps = value.at("bitrateKbps").get<int>();
    if (settings.width < 640 || settings.width > 7680 || settings.height < 360 ||
        settings.height > 4320 || settings.fps < 1 || settings.fps > 240 ||
        settings.bitrateKbps < 500 || settings.bitrateKbps > 500000) {
        throw std::invalid_argument("Stream dimensions, frame rate, or bitrate are invalid.");
    }

    const auto displayMode = value.at("displayMode").get<std::string>();
    if (displayMode == "fullscreen") {
        settings.displayMode = eclipse::DisplayMode::fullscreen;
    } else if (displayMode == "borderless") {
        settings.displayMode = eclipse::DisplayMode::borderless;
    } else if (displayMode != "windowed") {
        throw std::invalid_argument("Display mode is invalid.");
    }
    settings.displayIndex = value.value("displayIndex", 0);
    if (settings.displayIndex < 0 || settings.displayIndex > 15) {
        throw std::invalid_argument("Display index is invalid.");
    }

    const auto audioConfig = value.at("audioConfig").get<std::string>();
    if (audioConfig == "5.1") {
        settings.audioConfig = eclipse::AudioConfig::surround51;
    } else if (audioConfig == "7.1") {
        settings.audioConfig = eclipse::AudioConfig::surround71;
    } else if (audioConfig != "stereo") {
        throw std::invalid_argument("Audio configuration is invalid.");
    }
    const auto decoder = value.at("videoDecoder").get<std::string>();
    if (decoder != "automatic" && decoder != "hardware") {
        throw std::invalid_argument("This build currently supports hardware video decode only.");
    }
    const auto codec = value.at("videoCodec").get<std::string>();
    if (codec == "h264") {
        settings.videoCodec = eclipse::VideoCodec::h264;
    } else if (codec == "hevc") {
        settings.videoCodec = eclipse::VideoCodec::hevc;
    } else if (codec == "av1") {
        settings.videoCodec = eclipse::VideoCodec::av1;
    } else if (codec != "automatic") {
        throw std::invalid_argument("Video codec is invalid.");
    }

    settings.enableVsync = value.at("enableVsync").get<bool>();
    settings.muteHostAudio = value.at("muteHostAudio").get<bool>();
    settings.gameOptimizations = value.at("gameOptimizations").get<bool>();
    settings.quitAppAfter = value.at("quitAppAfter").get<bool>();
    settings.connectionWarnings = value.at("connectionWarnings").get<bool>();
    settings.detectBlockedConnections = value.at("detectBlockedConnections").get<bool>();
    settings.showPerformanceStats = value.at("showPerformanceStats").get<bool>();
    settings.keepAwake = value.at("keepAwake").get<bool>();
    settings.enableHdr = value.at("enableHdr").get<bool>();
    settings.enableYuv444 = value.at("enableYuv444").get<bool>();
    settings.input.absoluteMouseMode = value.at("absoluteMouseMode").get<bool>();
    const auto captureSystemKeys = value.at("captureSystemKeys").get<std::string>();
    if (captureSystemKeys == "fullscreen") {
        settings.input.captureSystemKeys = eclipse::SystemKeyCapture::fullscreen;
    } else if (captureSystemKeys == "always") {
        settings.input.captureSystemKeys = eclipse::SystemKeyCapture::always;
    } else if (captureSystemKeys != "off") {
        throw std::invalid_argument("System-key capture mode is invalid.");
    }
    settings.input.fullscreen = settings.displayMode != eclipse::DisplayMode::windowed;
    settings.input.touchscreenTrackpad = value.at("touchscreenTrackpad").get<bool>();
    settings.input.swapMouseButtons = value.at("swapMouseButtons").get<bool>();
    settings.input.reverseScrollDirection = value.at("reverseScrollDirection").get<bool>();
    settings.input.swapFaceButtons = value.at("swapFaceButtons").get<bool>();
    settings.input.forceGamepad = value.at("forceGamepad").get<bool>();
    settings.input.backgroundGamepad = value.at("backgroundGamepad").get<bool>();
    return settings;
}

void broadcast(ix::WebSocket& socket, const ExtensionContext& context, const std::string& event,
               const Json& data) {
    socket.send(Json{
                    {"id", ix::uuid4()},
                    {"method", "app.broadcast"},
                    {"accessToken", context.accessToken},
                    {"data", {{"event", event}, {"data", data}}},
                }
                    .dump());
}
}  // namespace

int main(int argc, char** argv) {
    try {
        configureDpiAwareness();
        const std::string input{std::istreambuf_iterator<char>{std::cin},
                                std::istreambuf_iterator<char>{}};
        const auto context = parseContext(input);
        const auto url = "ws://localhost:" + context.port + "?extensionId=" + context.extensionId +
                         "&connectToken=" + context.connectToken;

        ix::initNetSystem();

        ix::WebSocket socket;
        eclipse::ControlPlane controlPlane{parseDataPath(argc, argv)};
        std::mutex mutex;
        std::condition_variable closedCondition;
        std::atomic_bool closed = false;
        std::mutex workerMutex;
        std::vector<std::jthread> workers;
        std::unique_ptr<eclipse::MdnsDiscovery> discovery;
        std::mutex discoveryMutex;
        std::condition_variable discoveryCondition;
        std::deque<eclipse::MdnsService> discoveryQueue;
        std::unordered_set<std::string> queuedDiscoveries;
        std::atomic_bool discoveryEnabled = false;

        const auto publishHosts = [&] {
            Json hosts = Json::array();
            for (const auto& host : controlPlane.hosts()) {
                hosts.push_back(hostJson(host));
            }
            broadcast(socket, context, "eclipse.hosts.changed",
                      {{"schemaVersion", 1}, {"hosts", std::move(hosts)}});
        };

        const auto publishHostError = [&](const std::string& message) {
            broadcast(socket, context, "eclipse.host.error",
                      {{"schemaVersion", 1}, {"message", message}});
        };

        const auto probeHost = [&](const std::string& hostId) {
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId] {
                try {
                    controlPlane.probeHost(hostId);
                    if (!closed) {
                        publishHosts();
                    }
                } catch (const std::exception& exception) {
                    if (!closed) {
                        publishHostError(exception.what());
                    }
                }
            });
        };

        std::jthread discoveryWorker([&](std::stop_token stopToken) {
            while (!stopToken.stop_requested()) {
                eclipse::MdnsService service;
                {
                    std::unique_lock lock{discoveryMutex};
                    discoveryCondition.wait(lock, [&] {
                        return stopToken.stop_requested() || !discoveryQueue.empty();
                    });
                    if (stopToken.stop_requested()) return;
                    service = std::move(discoveryQueue.front());
                    discoveryQueue.pop_front();
                }
                const auto key = service.address + ":" + std::to_string(service.port);
                const auto completeDiscovery = [&] {
                    std::scoped_lock lock{discoveryMutex};
                    queuedDiscoveries.erase(key);
                };
                if (!discoveryEnabled) {
                    completeDiscovery();
                    continue;
                }
                auto endpoint = service.address;
                if (endpoint.find(':') != std::string::npos) endpoint = "[" + endpoint + "]";
                if (service.port != 47989) endpoint += ":" + std::to_string(service.port);
                try {
                    controlPlane.discoverHost(
                        service.name.empty() ? service.hostname : service.name,
                        std::move(endpoint));
                    if (!closed) publishHosts();
                } catch (const std::exception&) {
                    // DNS-SD advertisements are untrusted until a Sunshine probe succeeds.
                }
                completeDiscovery();
            }
        });

        discovery = std::make_unique<eclipse::MdnsDiscovery>([&](const auto& service) {
            const auto key = service.address + ":" + std::to_string(service.port);
            {
                std::scoped_lock lock{discoveryMutex};
                if (!discoveryEnabled || queuedDiscoveries.contains(key) ||
                    discoveryQueue.size() >= 16 || queuedDiscoveries.size() >= 64) {
                    return;
                }
                queuedDiscoveries.insert(key);
                discoveryQueue.push_back(service);
            }
            discoveryCondition.notify_one();
        });

        const auto publishPairing = [&](const std::string& hostId, const std::string& state,
                                        const std::string& message) {
            broadcast(socket, context, "eclipse.pairing.changed",
                      {{"schemaVersion", 1},
                       {"hostId", hostId},
                       {"state", state},
                       {"message", message}});
        };

        const auto pairHost = [&](const std::string& hostId, const std::string& pin) {
            publishPairing(hostId, "pairing",
                           "Enter the displayed PIN in Sunshine's web interface.");
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId, pin] {
                try {
                    controlPlane.pairHost(hostId, pin);
                    if (!closed) {
                        publishHosts();
                        publishPairing(hostId, "paired", "Server identity verified and pinned.");
                    }
                } catch (const std::exception& exception) {
                    if (!closed) {
                        publishHosts();
                        publishPairing(hostId, "error", exception.what());
                    }
                }
            });
        };

        const auto publishApps = [&](const std::string& hostId, const std::string& state,
                                     const std::vector<eclipse::GameStreamApp>& apps,
                                     const std::string& message = {}) {
            Json values = Json::array();
            for (const auto& app : apps) {
                values.push_back(appJson(app));
            }
            broadcast(socket, context, "eclipse.apps.changed",
                      {{"schemaVersion", 1},
                       {"hostId", hostId},
                       {"state", state},
                       {"apps", std::move(values)},
                       {"message", message}});
        };

        const auto loadApps = [&](const std::string& hostId) {
            publishApps(hostId, "loading", {});
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId] {
                try {
                    controlPlane.probeHost(hostId);
                    if (!closed) publishHosts();
                    const auto apps = controlPlane.loadApps(hostId);
                    if (closed) return;
                    publishApps(hostId, "ready", apps);
                    for (const auto& app : apps) {
                        try {
                            const auto dataUrl = controlPlane.boxArtDataUrl(hostId, app.id);
                            if (!closed) {
                                broadcast(socket, context, "eclipse.app.art.changed",
                                          {{"schemaVersion", 1},
                                           {"hostId", hostId},
                                           {"appId", app.id},
                                           {"dataUrl", dataUrl},
                                           {"error", ""}});
                            }
                        } catch (const std::exception& exception) {
                            if (!closed) {
                                broadcast(socket, context, "eclipse.app.art.changed",
                                          {{"schemaVersion", 1},
                                           {"hostId", hostId},
                                           {"appId", app.id},
                                           {"dataUrl", ""},
                                           {"error", exception.what()}});
                            }
                        }
                    }
                } catch (const std::exception& exception) {
                    if (!closed) publishApps(hostId, "error", {}, exception.what());
                }
            });
        };

        const auto publishSession = [&](const std::string& hostId, int appId,
                                        const std::string& appName, const std::string& state,
                                        const std::string& message, bool resumed = false) {
            broadcast(socket, context, "eclipse.session.changed",
                      {{"schemaVersion", 1},
                       {"hostId", hostId},
                       {"appId", appId},
                       {"appName", appName},
                       {"state", state},
                       {"message", message},
                       {"resumed", resumed}});
        };

        controlPlane.setSessionListener([&](const eclipse::SessionUpdate& update) {
            if (!closed) {
                publishSession(update.hostId, update.appId, update.appName, update.state,
                               update.message, update.resumed);
                if (update.state == "terminated") publishHosts();
            }
        });

        controlPlane.setStreamOverlayListener([&](const eclipse::StreamOverlayRequest& request) {
            if (!closed) {
                broadcast(socket, context, "eclipse.stream.overlay.requested",
                          {{"schemaVersion", 1},
                           {"hostId", request.hostId},
                           {"appId", request.appId},
                           {"appName", request.appName},
                           {"generation", std::to_string(request.generation)},
                           {"bounds",
                            {{"x", request.x},
                             {"y", request.y},
                             {"width", request.width},
                             {"height", request.height},
                             {"scaleFactor", request.scaleFactor}}},
                           {"wayland", request.wayland},
                           {"fullscreen", request.fullscreen},
                           {"stream",
                            {{"width", request.settings.width},
                             {"height", request.settings.height},
                             {"fps", request.settings.fps},
                             {"bitrateKbps", request.settings.bitrateKbps},
                             {"codec", codecName(request.videoFormat)},
                             {"displayMode", displayModeName(request.settings.displayMode)},
                             {"displayIndex", request.settings.displayIndex},
                             {"enableVsync", request.settings.enableVsync},
                             {"audioConfig", audioConfigName(request.settings.audioConfig)},
                             {"muteHostAudio", request.settings.muteHostAudio},
                             {"gameOptimizations", request.settings.gameOptimizations},
                             {"quitAppAfter", request.settings.quitAppAfter},
                             {"enableHdr", request.settings.enableHdr},
                             {"enableYuv444", request.settings.enableYuv444},
                             {"absoluteMouseMode", request.settings.input.absoluteMouseMode},
                             {"captureSystemKeys",
                              systemKeyCaptureName(request.settings.input.captureSystemKeys)},
                             {"touchscreenTrackpad", request.settings.input.touchscreenTrackpad},
                             {"swapMouseButtons", request.settings.input.swapMouseButtons},
                             {"reverseScrollDirection",
                              request.settings.input.reverseScrollDirection},
                             {"swapFaceButtons", request.settings.input.swapFaceButtons},
                             {"forceGamepad", request.settings.input.forceGamepad},
                             {"backgroundGamepad", request.settings.input.backgroundGamepad},
                             {"controllerMask", request.controllerMask}}}});
            }
        });

        controlPlane.setStreamStatisticsListener([&](const eclipse::StreamStatisticsUpdate& update) {
            if (!closed) {
                const auto& statistics = update.sample.statistics;
                broadcast(socket, context, "eclipse.stream.statistics",
                          {{"schemaVersion", 1},
                           {"hostId", update.hostId},
                           {"generation", std::to_string(update.generation)},
                           {"sequence", update.sample.sequence},
                           {"elapsedMs", update.sample.elapsedMs},
                           {"statistics",
                            {{"totalFps", statistics.totalFps},
                             {"receivedFps", statistics.receivedFps},
                             {"decodedFps", statistics.decodedFps},
                             {"presentedFps", statistics.presentedFps},
                             {"bitrateMbps", statistics.bitrateMbps},
                             {"frameLossPercent", statistics.frameLossPercent},
                             {"jitterLossPercent", statistics.jitterLossPercent},
                             {"minimumHostLatencyMs", statistics.minimumHostLatencyMs},
                             {"maximumHostLatencyMs", statistics.maximumHostLatencyMs},
                             {"averageHostLatencyMs", statistics.averageHostLatencyMs},
                             {"averageReassemblyMs", statistics.averageReassemblyMs},
                             {"averageDecodeMs", statistics.averageDecodeMs},
                             {"averagePresentMs", statistics.averagePresentMs},
                             {"averageQueueDelayMs", statistics.averageQueueDelayMs},
                             {"hasHostLatency", statistics.hasHostLatency},
                             {"queueDrops", statistics.queueDrops},
                             {"rttMs", statistics.rttMs},
                             {"rttVarianceMs", statistics.rttVarianceMs}}}});
            }
        });

        const auto launchApp = [&](const std::string& hostId, int appId,
                                    const eclipse::StreamSettings& settings) {
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId, appId, settings] {
                try {
                    static_cast<void>(controlPlane.launchApp(hostId, appId, settings));
                    if (!closed) {
                        publishHosts();
                    }
                } catch (const std::exception& exception) {
                    if (!closed) {
                        publishHosts();
                        publishSession(hostId, appId, "", "error", exception.what());
                    }
                }
            });
        };

        const auto stopSession = [&](const std::string& hostId, bool quitHost) {
            publishSession(hostId, 0, "", "stopping",
                           quitHost ? "Stopping host application." : "Disconnecting stream.");
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId, quitHost] {
                try {
                    controlPlane.stopSession(hostId, quitHost);
                    if (!closed) {
                        publishHosts();
                        publishSession(hostId, 0, "", "stopped",
                                       quitHost ? "Host application stopped."
                                                : "Stream disconnected.");
                    }
                } catch (const std::exception& exception) {
                    if (!closed) {
                        publishSession(hostId, 0, "", "error", exception.what());
                    }
                }
            });
        };

        socket.disableAutomaticReconnection();
        socket.setUrl(url);
        socket.setOnMessageCallback([&](const ix::WebSocketMessagePtr& message) {
            switch (message->type) {
                case ix::WebSocketMessageType::Open:
                    std::cout << "[eclipse-core] connected" << std::endl;
                    broadcast(socket, context, "eclipse.core.status", makeStatus());
                    publishHosts();
                    for (const auto& hostId : controlPlane.hostIds()) {
                        probeHost(hostId);
                    }
                    break;
                case ix::WebSocketMessageType::Message: {
                    const auto payload = Json::parse(message->str, nullptr, false);
                    if (payload.is_discarded()) {
                        break;
                    }

                    const auto event = payload.value("event", "");
                    if (event == "core.status") {
                        broadcast(socket, context, "eclipse.core.status", makeStatus());
                    } else if (event == "hosts.list") {
                        publishHosts();
                    } else if (event == "discovery.configure") {
                        if (payload.at("data").at("enabled").get<bool>()) {
                            discoveryEnabled = true;
                            discovery->start();
                        } else {
                            discoveryEnabled = false;
                            discovery->stop();
                            std::scoped_lock lock{discoveryMutex};
                            discoveryQueue.clear();
                            queuedDiscoveries.clear();
                        }
                    } else if (event == "host.add") {
                        try {
                            const auto& data = payload.at("data");
                            const auto host = controlPlane.addHost(data.at("name").get<std::string>(),
                                                                   data.at("address").get<std::string>());
                            publishHosts();
                            probeHost(host.id);
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "host.refresh") {
                        try {
                            probeHost(payload.at("data").at("id").get<std::string>());
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "host.wake") {
                        try {
                            controlPlane.wakeHost(payload.at("data").at("id").get<std::string>());
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "host.pair") {
                        try {
                            const auto& data = payload.at("data");
                            pairHost(data.at("id").get<std::string>(),
                                     data.at("pin").get<std::string>());
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "host.apps") {
                        try {
                            loadApps(payload.at("data").at("id").get<std::string>());
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "app.launch") {
                        try {
                            const auto& data = payload.at("data");
                            launchApp(data.at("hostId").get<std::string>(),
                                      data.at("appId").get<int>(),
                                      parseStreamSettings(data.at("settings")));
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "session.cancel") {
                        try {
                            const auto& data = payload.at("data");
                            stopSession(data.at("hostId").get<std::string>(),
                                        data.at("quitHost").get<bool>());
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "stream.overlay.closed") {
                        try {
                            const auto& data = payload.at("data");
                            std::size_t parsed = 0;
                            const auto generationText = data.at("generation").get<std::string>();
                            const auto generation = std::stoull(generationText, &parsed);
                            if (parsed != generationText.size()) {
                                throw std::invalid_argument("Stream overlay generation is invalid.");
                            }
                            controlPlane.resumeStreamOverlay(data.at("hostId").get<std::string>(),
                                                             generation);
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    } else if (event == "host.remove") {
                        try {
                            controlPlane.removeHost(payload.at("data").at("id").get<std::string>());
                            publishHosts();
                        } catch (const std::exception& exception) {
                            publishHostError(exception.what());
                        }
                    }
                    break;
                }
                case ix::WebSocketMessageType::Error:
                    std::cerr << "[eclipse-core] connection error: "
                              << message->errorInfo.reason << std::endl;
                    closed = true;
                    closedCondition.notify_one();
                    break;
                case ix::WebSocketMessageType::Close:
                    closed = true;
                    closedCondition.notify_one();
                    break;
                default:
                    break;
            }
        });

        socket.start();

        {
            std::unique_lock lock{mutex};
            closedCondition.wait(lock, [&closed] { return closed.load(); });
        }

        discovery->stop();
        discoveryEnabled = false;
        discoveryWorker.request_stop();
        discoveryCondition.notify_all();
        discoveryWorker.join();
        workers.clear();
        socket.stop();
        ix::uninitNetSystem();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[eclipse-core] fatal: " << exception.what() << std::endl;
        return 1;
    }
}
