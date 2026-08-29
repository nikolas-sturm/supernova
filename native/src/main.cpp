#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ixwebsocket/IXNetSystem.h>
#include <ixwebsocket/IXUuid.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "control_plane.h"

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

    return {
        {"schemaVersion", 1},
        {"state", "ready"},
        {"detail", "Secure host library and native session launch ready."},
        {"moonlightQtRevision", ECLIPSE_MOONLIGHT_QT_REVISION},
        {"moonlightCommonRevision", ECLIPSE_MOONLIGHT_COMMON_REVISION},
        {"streamingAvailable", false},
    };
}

Json hostJson(const eclipse::HostRecord& host) {
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
        {"lastSeenAt", host.lastSeenAt},
        {"paired", host.paired},
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

    if (value.at("audioConfig").get<std::string>() != "stereo") {
        throw std::invalid_argument("This build currently supports stereo audio only.");
    }
    const auto decoder = value.at("videoDecoder").get<std::string>();
    if (decoder != "automatic" && decoder != "hardware") {
        throw std::invalid_argument("This build currently supports hardware video decode only.");
    }
    const auto codec = value.at("videoCodec").get<std::string>();
    if (codec != "automatic" && codec != "h264") {
        throw std::invalid_argument("This build currently supports H.264 video only.");
    }

    settings.enableVsync = value.at("enableVsync").get<bool>();
    settings.muteHostAudio = value.at("muteHostAudio").get<bool>();
    settings.gameOptimizations = value.at("gameOptimizations").get<bool>();
    settings.connectionWarnings = value.at("connectionWarnings").get<bool>();
    settings.keepAwake = value.at("keepAwake").get<bool>();
    settings.input.absoluteMouseMode = value.at("absoluteMouseMode").get<bool>();
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
        const std::string input{std::istreambuf_iterator<char>{std::cin},
                                std::istreambuf_iterator<char>{}};
        const auto context = parseContext(input);
        eclipse::ControlPlane controlPlane{parseDataPath(argc, argv)};
        const auto url = "ws://localhost:" + context.port + "?extensionId=" + context.extensionId +
                         "&connectToken=" + context.connectToken;

        ix::initNetSystem();

        ix::WebSocket socket;
        std::mutex mutex;
        std::condition_variable closedCondition;
        std::atomic_bool closed = false;
        std::mutex workerMutex;
        std::vector<std::jthread> workers;

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
            }
        });

        const auto launchApp = [&](const std::string& hostId, int appId,
                                   const eclipse::StreamSettings& settings) {
            publishSession(hostId, appId, "", "launching", "Requesting native host session.");
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

        const auto cancelSession = [&](const std::string& hostId) {
            publishSession(hostId, 0, "", "stopping", "Stopping host session.");
            std::scoped_lock lock{workerMutex};
            workers.emplace_back([&, hostId] {
                try {
                    controlPlane.cancelSession(hostId);
                    if (!closed) {
                        publishHosts();
                        publishSession(hostId, 0, "", "stopped", "Host session stopped.");
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
                            cancelSession(payload.at("data").at("hostId").get<std::string>());
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

        workers.clear();
        socket.stop();
        ix::uninitNetSystem();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[eclipse-core] fatal: " << exception.what() << std::endl;
        return 1;
    }
}
