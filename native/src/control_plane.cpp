#include "control_plane.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include <ixwebsocket/IXUuid.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "gamestream_client.h"
#include "stream_session.h"

namespace eclipse {
namespace {
using Json = nlohmann::json;

const std::filesystem::path& ensureDataPath(const std::filesystem::path& path) {
    std::filesystem::create_directories(path);
    return path;
}

std::string trim(std::string value) {
    const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string randomHex(std::size_t byteCount) {
    std::random_device source;
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (std::size_t index = 0; index < byteCount; ++index) {
        output << std::setw(2) << (source() & 0xffU);
    }
    return output.str();
}

Json hostJson(const HostRecord& host) {
    return {
        {"id", host.id},
        {"name", host.name},
        {"address", host.address},
        {"serverName", host.serverName},
        {"serverUniqueId", host.serverUniqueId},
        {"appVersion", host.appVersion},
        {"gfeVersion", host.gfeVersion},
        {"serverState", host.serverState},
        {"status", host.status},
        {"error", host.error},
        {"serverCertificate", host.serverCertificate},
        {"httpsPort", host.httpsPort},
        {"currentGameId", host.currentGameId},
        {"serverCodecModeSupport", host.serverCodecModeSupport},
        {"lastSeenAt", host.lastSeenAt},
        {"paired", host.paired},
    };
}

HostRecord parseHost(const Json& value) {
    HostRecord host;
    host.id = value.value("id", "");
    host.name = value.value("name", "");
    host.address = value.value("address", "");
    host.serverName = value.value("serverName", "");
    host.serverUniqueId = value.value("serverUniqueId", "");
    host.appVersion = value.value("appVersion", "");
    host.gfeVersion = value.value("gfeVersion", "");
    host.serverState = value.value("serverState", "");
    host.serverCertificate = value.value("serverCertificate", "");
    host.status = "offline";
    const auto httpsPort = value.value("httpsPort", 0);
    if (httpsPort > 0 && httpsPort <= 65535) {
        host.httpsPort = static_cast<std::uint16_t>(httpsPort);
    }
    host.lastSeenAt = value.value("lastSeenAt", 0);
    host.currentGameId = value.value("currentGameId", 0);
    host.serverCodecModeSupport = value.value("serverCodecModeSupport", 0);
    host.paired = value.value("paired", false);
    return host;
}

std::string imageDataUrl(const std::string& bytes) {
    constexpr std::size_t maximumBytes = 8 * 1024 * 1024;
    if (bytes.empty() || bytes.size() > maximumBytes) {
        throw std::runtime_error("Application artwork is empty or too large.");
    }

    std::string mime;
    if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
        mime = "image/png";
    } else if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xff &&
               static_cast<unsigned char>(bytes[1]) == 0xd8 &&
               static_cast<unsigned char>(bytes[2]) == 0xff) {
        mime = "image/jpeg";
    } else {
        throw std::runtime_error("Sunshine returned unsupported application artwork.");
    }

    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const auto written = EVP_EncodeBlock(
        reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    if (written <= 0) {
        throw std::runtime_error("Cannot encode application artwork.");
    }
    encoded.resize(static_cast<std::size_t>(written));
    return "data:" + mime + ";base64," + encoded;
}

std::string cacheKey(std::string value) {
    std::replace_if(value.begin(), value.end(),
                    [](const unsigned char character) {
                        return !std::isalnum(character) && character != '-' && character != '_';
                    },
                    '_');
    return value;
}
}  // namespace

ControlPlane::ControlPlane(std::filesystem::path dataPath)
    : dataPath_(std::move(dataPath)),
      statePath_(dataPath_ / "client-state.json"),
      gameStream_(std::make_unique<GameStreamClient>(ensureDataPath(dataPath_))) {
    load();
}

ControlPlane::~ControlPlane() {
    std::unique_ptr<StreamSession> transport;
    {
        std::scoped_lock lock{mutex_};
        sessionListener_ = {};
        transport = std::move(transport_);
    }
    transport.reset();
}

std::vector<HostRecord> ControlPlane::hosts() const {
    std::scoped_lock lock{mutex_};
    return hosts_;
}

std::vector<std::string> ControlPlane::hostIds() const {
    std::scoped_lock lock{mutex_};
    std::vector<std::string> ids;
    ids.reserve(hosts_.size());
    for (const auto& host : hosts_) {
        ids.push_back(host.id);
    }
    return ids;
}

HostRecord ControlPlane::addHost(std::string name, std::string address) {
    name = trim(std::move(name));
    if (name.empty()) {
        throw std::invalid_argument("Display name is required.");
    }
    const auto normalizedAddress = GameStreamClient::normalizeAddress(std::move(address));

    std::scoped_lock lock{mutex_};
    auto existing = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& host) {
        return host.address == normalizedAddress;
    });
    if (existing != hosts_.end()) {
        existing->name = std::move(name);
        existing->status = "probing";
        existing->error.clear();
        saveLocked();
        return *existing;
    }

    HostRecord host;
    host.id = ix::uuid4();
    host.name = std::move(name);
    host.address = normalizedAddress;
    host.status = "probing";
    hosts_.push_back(host);
    saveLocked();
    return host;
}

HostRecord ControlPlane::probeHost(const std::string& id) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == id;
        });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        host->status = "probing";
        host->error.clear();
        current = *host;
        clientId = clientId_;
    }

    try {
        const auto info = gameStream_->probe(current.address, current.httpsPort, clientId,
                                             current.serverCertificate);
        current.serverName = info.serverName;
        current.serverUniqueId = info.serverUniqueId;
        current.appVersion = info.appVersion;
        current.gfeVersion = info.gfeVersion;
        current.serverState = info.serverState;
        current.httpsPort = info.httpsPort;
        current.currentGameId = info.currentGameId;
        current.serverCodecModeSupport = info.serverCodecModeSupport;
        current.paired = info.paired;
        current.status = "online";
        current.error.clear();
        current.lastSeenAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    } catch (const std::exception& exception) {
        current.status = "offline";
        current.error = exception.what();
    }

    std::scoped_lock lock{mutex_};
    const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
        return value.id == id;
    });
    if (host == hosts_.end()) {
        return current;
    }
    *host = current;
    saveLocked();
    return current;
}

std::vector<GameStreamApp> ControlPlane::loadApps(const std::string& hostId) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        if (host->status != "online" || !host->paired || host->serverCertificate.empty()) {
            throw std::runtime_error("Host must be paired and online to load applications.");
        }
        current = *host;
        clientId = clientId_;
    }

    auto result = gameStream_->apps(current.address, current.httpsPort, clientId,
                                    current.serverCertificate);
    std::stable_sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    {
        std::scoped_lock lock{mutex_};
        apps_[hostId] = result;
    }
    return result;
}

std::string ControlPlane::boxArtDataUrl(const std::string& hostId, int appId) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        const auto apps = apps_.find(hostId);
        if (host == hosts_.end() || apps == apps_.end() ||
            std::none_of(apps->second.begin(), apps->second.end(),
                         [&](const GameStreamApp& app) { return app.id == appId; })) {
            throw std::invalid_argument("Application no longer exists in this library.");
        }
        current = *host;
        clientId = clientId_;
    }

    const auto cacheDirectory = dataPath_ / "box-art" / cacheKey(hostId);
    const auto cachePath = cacheDirectory / (std::to_string(appId) + ".asset");
    std::string bytes;
    if (std::filesystem::exists(cachePath)) {
        std::ifstream input{cachePath, std::ios::binary};
        bytes.assign(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
        try {
            return imageDataUrl(bytes);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(cachePath, ignored);
        }
    }

    bytes = gameStream_->boxArt(current.address, current.httpsPort, clientId,
                                current.serverCertificate, appId);
    const auto dataUrl = imageDataUrl(bytes);
    std::filesystem::create_directories(cacheDirectory);
    const auto temporaryPath = cachePath.string() + ".tmp";
    {
        std::ofstream output{temporaryPath, std::ios::binary | std::ios::trunc};
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            throw std::runtime_error("Cannot cache application artwork.");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporaryPath, cachePath, error);
    if (error) {
        std::filesystem::remove(temporaryPath, error);
    }
    return dataUrl;
}

SessionRecord ControlPlane::launchApp(const std::string& hostId, int appId,
                                       const StreamSettings& settings) {
    std::scoped_lock sessionLock{sessionMutex_};
    HostRecord current;
    GameStreamApp selected;
    std::string clientId;
    bool resume = false;
    {
        std::scoped_lock lock{mutex_};
        if (transport_ || session_) {
            throw std::runtime_error("Eclipse already has an active stream. Stop it before launching again.");
        }
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        const auto apps = apps_.find(hostId);
        if (host == hosts_.end() || apps == apps_.end()) {
            throw std::invalid_argument("Load this host's application library before launching.");
        }
        const auto app = std::find_if(apps->second.begin(), apps->second.end(),
                                      [&](const GameStreamApp& value) { return value.id == appId; });
        if (app == apps->second.end()) {
            throw std::invalid_argument("Application no longer exists in this library.");
        }
        if (host->status != "online" || !host->paired || host->serverCertificate.empty()) {
            throw std::runtime_error("Host must be paired and online to launch applications.");
        }
        if (host->serverCodecModeSupport == 0) {
            throw std::runtime_error("Host did not report streaming codec support. Refresh it first.");
        }
        if (host->currentGameId != 0 && host->currentGameId != appId) {
            throw std::runtime_error("Another application is already running on this host.");
        }
        current = *host;
        selected = *app;
        clientId = clientId_;
        resume = host->currentGameId == appId;
    }

    const auto launched = gameStream_->launch(current.address, current.httpsPort, clientId,
                                                current.serverCertificate, appId, resume, settings);
    SessionRecord session{
        .hostId = hostId,
        .appId = appId,
        .appName = selected.name,
        .sessionUrl = launched.sessionUrl,
        .resumed = launched.resumed,
        .remoteInputKey = launched.remoteInputKey,
        .remoteInputIv = launched.remoteInputIv,
    };
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        if (host != hosts_.end()) {
            host->currentGameId = appId;
            host->serverState = "SUNSHINE_SERVER_BUSY";
            saveLocked();
        }
        session_ = session;
    }

    auto transport = std::make_unique<StreamSession>(
        StreamSessionConfig{
            .hostId = hostId,
            .appId = appId,
            .appName = selected.name,
            .address = GameStreamClient::streamHost(current.address),
            .appVersion = current.appVersion,
            .gfeVersion = current.gfeVersion,
            .serverCodecModeSupport = current.serverCodecModeSupport,
            .settings = settings,
            .launch = launched,
        },
        [this, hostId, appId, appName = selected.name, resumed = launched.resumed](
            const StreamSessionEvent& event) {
            std::function<void(const SessionUpdate&)> listener;
            {
                std::scoped_lock lock{mutex_};
                listener = sessionListener_;
            }
            if (listener) {
                listener({
                    .hostId = hostId,
                    .appId = appId,
                    .appName = appName,
                    .state = event.state,
                    .message = event.message,
                    .resumed = resumed,
                });
            }
        });
    try {
        transport->start();
    } catch (...) {
        try {
            gameStream_->cancel(current.address, current.httpsPort, clientId,
                                current.serverCertificate);
        } catch (...) {
        }
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        if (host != hosts_.end()) {
            host->currentGameId = 0;
            host->serverState = "SUNSHINE_SERVER_IDLE";
            saveLocked();
        }
        session_.reset();
        throw;
    }
    {
        std::scoped_lock lock{mutex_};
        transport_ = std::move(transport);
    }
    return session;
}

void ControlPlane::cancelSession(const std::string& hostId) {
    std::scoped_lock sessionLock{sessionMutex_};
    HostRecord current;
    std::string clientId;
    std::unique_ptr<StreamSession> transport;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == hostId;
        });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        const bool ownsSession = session_ && session_->hostId == hostId;
        const bool ownsStaleTransport = !session_ && static_cast<bool>(transport_);
        if (!ownsSession && !ownsStaleTransport && host->currentGameId == 0) {
            return;
        }
        current = *host;
        clientId = clientId_;
        if (ownsSession || ownsStaleTransport) transport = std::move(transport_);
    }

    if (transport) transport->stop();
    gameStream_->cancel(current.address, current.httpsPort, clientId, current.serverCertificate);
    std::scoped_lock lock{mutex_};
    const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
        return value.id == hostId;
    });
    if (host != hosts_.end()) {
        host->currentGameId = 0;
        host->serverState = "SUNSHINE_SERVER_IDLE";
        saveLocked();
    }
    if (session_ && session_->hostId == hostId) session_.reset();
}

void ControlPlane::setSessionListener(std::function<void(const SessionUpdate&)> listener) {
    std::scoped_lock lock{mutex_};
    sessionListener_ = std::move(listener);
}

HostRecord ControlPlane::pairHost(const std::string& id, const std::string& pin) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == id;
        });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        if (host->status != "online" || host->appVersion.empty()) {
            throw std::runtime_error("Host must be online before pairing.");
        }
        host->status = "pairing";
        host->error.clear();
        current = *host;
        clientId = clientId_;
        saveLocked();
    }

    try {
        current.serverCertificate = gameStream_->pair(current.address, current.httpsPort,
                                                      current.appVersion, clientId, pin);
        current.paired = true;
        current.status = "online";
        current.error.clear();
    } catch (const std::exception& exception) {
        current.status = current.serverName.empty() ? "offline" : "online";
        current.error = exception.what();
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == id;
        });
        if (host != hosts_.end()) {
            *host = current;
            saveLocked();
        }
        throw;
    }

    std::scoped_lock lock{mutex_};
    const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
        return value.id == id;
    });
    if (host != hosts_.end()) {
        *host = current;
        saveLocked();
    }
    return current;
}

void ControlPlane::removeHost(const std::string& id) {
    {
        std::scoped_lock lock{mutex_};
        if (session_ && session_->hostId == id) {
            throw std::runtime_error("Stop the active session before removing this host.");
        }
        std::erase_if(hosts_, [&](const HostRecord& host) { return host.id == id; });
        apps_.erase(id);
        saveLocked();
    }
    std::error_code ignored;
    std::filesystem::remove_all(dataPath_ / "box-art" / cacheKey(id), ignored);
}

void ControlPlane::load() {
    std::scoped_lock lock{mutex_};
    if (std::filesystem::exists(statePath_)) {
        try {
            std::ifstream input{statePath_};
            const auto state = Json::parse(input);
            clientId_ = state.value("clientId", "");
            for (const auto& value : state.value("hosts", Json::array())) {
                auto host = parseHost(value);
                if (!host.id.empty() && !host.name.empty() && !host.address.empty()) {
                    hosts_.push_back(std::move(host));
                }
            }
        } catch (const std::exception& exception) {
            throw std::runtime_error("Cannot read native client state: " + std::string{exception.what()});
        }
    }

    if (clientId_.empty()) {
        clientId_ = randomHex(8);
        saveLocked();
    }
}

void ControlPlane::saveLocked() const {
    Json hostValues = Json::array();
    for (const auto& host : hosts_) {
        hostValues.push_back(hostJson(host));
    }
    const Json state{{"schemaVersion", 1}, {"clientId", clientId_}, {"hosts", hostValues}};

    const auto temporaryPath = statePath_.string() + ".tmp";
    {
        std::ofstream output{temporaryPath, std::ios::trunc};
        if (!output) {
            throw std::runtime_error("Cannot write native client state.");
        }
        output << state.dump(2) << '\n';
    }

    std::error_code error;
    std::filesystem::rename(temporaryPath, statePath_, error);
    if (error) {
        error.clear();
        std::filesystem::remove(statePath_, error);
        error.clear();
        std::filesystem::rename(temporaryPath, statePath_, error);
    }
    if (error) {
        throw std::runtime_error("Cannot replace native client state: " + error.message());
    }
}

}  // namespace eclipse
