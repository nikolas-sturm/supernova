#include "control_plane.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <ixwebsocket/IXUuid.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include "audio_renderer.h"
#include "gamestream_client.h"
#include "input_forwarder.h"
#include "sse_decoder.h"
#include "stream_session.h"
#include "video_renderer.h"
#include "wake_on_lan.h"

namespace terra {
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

std::vector<std::string> stringArray(const Json& value) {
    std::vector<std::string> result;
    if (!value.is_array()) return result;
    for (const auto& item : value) {
        if (item.is_string()) result.push_back(item.get<std::string>());
    }
    return result;
}

Json hostJson(const HostRecord& host) {
    Json displayModes = Json::array();
    for (const auto& mode : host.displayModes) {
        displayModes.push_back(
            {{"width", mode.width}, {"height", mode.height}, {"refreshRate", mode.refreshRate}});
    }
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
        {"maxLumaPixelsHevc", host.maxLumaPixelsHevc},
        {"displayModes", std::move(displayModes)},
        {"wakeMacAddress", host.wakeMacAddress},
        {"lastSeenAt", host.lastSeenAt},
        {"paired", host.paired},
        {"apiVersion", host.apiVersion},
        {"apiPort", host.apiPort},
        {"capabilities", host.capabilities},
        {"apiClientUuid", host.apiClientUuid},
        {"apiClientName", host.apiClientName},
        {"apiScopes", host.apiScopes},
        {"allowedApps", host.allowedApps},
        {"features", host.features},
        {"limits", host.limits},
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
    host.lastSeenAt = std::max<std::int64_t>(0, value.value("lastSeenAt", std::int64_t{0}));
    host.currentGameId = value.value("currentGameId", 0);
    host.serverCodecModeSupport = value.value("serverCodecModeSupport", kBaselineCodecModeSupport);
    host.maxLumaPixelsHevc = value.value("maxLumaPixelsHevc", std::uint64_t{0});
    if (const auto modes = value.find("displayModes"); modes != value.end() && modes->is_array()) {
        for (const auto& mode : *modes) {
            if (!mode.is_object()) continue;
            HostDisplayMode parsed{
                .width = mode.value("width", 0),
                .height = mode.value("height", 0),
                .refreshRate = mode.value("refreshRate", 0),
            };
            if (parsed.width > 0 && parsed.height > 0 && parsed.refreshRate > 0) {
                host.displayModes.push_back(parsed);
            }
        }
        std::ranges::sort(host.displayModes);
        host.displayModes.erase(std::unique(host.displayModes.begin(), host.displayModes.end()),
                                host.displayModes.end());
    }
    if (const auto wakeAddress = parseMacAddress(value.value("wakeMacAddress", ""))) {
        host.wakeMacAddress = formatMacAddress(*wakeAddress);
    }
    host.paired = value.value("paired", false);
    host.apiVersion = value.value("apiVersion", 0);
    const auto apiPort = value.value("apiPort", 0);
    if (apiPort > 0 && apiPort <= 65535) host.apiPort = static_cast<std::uint16_t>(apiPort);
    host.capabilities = stringArray(value.value("capabilities", Json::array()));
    host.apiClientUuid = value.value("apiClientUuid", "");
    host.apiClientName = value.value("apiClientName", "");
    host.apiScopes = stringArray(value.value("apiScopes", Json::array()));
    host.allowedApps = stringArray(value.value("allowedApps", Json::array()));
    host.features = value.value("features", Json::object());
    host.limits = value.value("limits", Json::object());
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
        throw std::runtime_error("Sol returned unsupported application artwork.");
    }

    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const auto written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                         reinterpret_cast<const unsigned char*>(bytes.data()),
                                         static_cast<int>(bytes.size()));
    if (written <= 0) {
        throw std::runtime_error("Cannot encode application artwork.");
    }
    encoded.resize(static_cast<std::size_t>(written));
    return "data:" + mime + ";base64," + encoded;
}

std::string cacheKey(std::string value) {
    std::replace_if(
        value.begin(), value.end(),
        [](const unsigned char character) {
            return !std::isalnum(character) && character != '-' && character != '_';
        },
        '_');
    return value;
}

bool hasCapability(const HostRecord& host, const std::string_view capability) {
    return std::ranges::contains(host.capabilities, capability);
}

std::string nullableString(const Json& value, const std::string_view name) {
    const auto item = value.find(name);
    return item != value.end() && item->is_string() ? item->get<std::string>() : std::string{};
}

GameStreamApp catalogApp(const Json& value) {
    if (!value.is_object() || !value.contains("uuid") || !value.at("uuid").is_string() ||
        !value.contains("legacyId") || !value.at("legacyId").is_number_integer() ||
        !value.contains("name") || !value.at("name").is_string()) {
        throw std::runtime_error("Sol returned an invalid Catalog V2 application.");
    }
    GameStreamApp app{
        .id = value.at("legacyId").get<int>(),
        .name = value.at("name").get<std::string>(),
        .hdrSupported = value.value("hdr", false),
        .appCollectorGame = value.value("kind", "unknown") == "game",
        .uuid = value.at("uuid").get<std::string>(),
        .kind = value.value("kind", "unknown"),
        .description = value.value("description", ""),
        .source = value.value("source", ""),
        .publisher = value.value("publisher", ""),
        .tags = stringArray(value.value("tags", Json::array())),
        .inputRequirements = stringArray(value.value("inputRequirements", Json::array())),
        .installed = value.value("installed", true),
        .updateAvailable = value.value("updateAvailable", false),
        .assetId = {},
        .assetPath = {},
        .assetRevision = 0,
        .displayProfileId = nullableString(value, "displayProfileId"),
        .streamProfileId = nullableString(value, "streamProfileId"),
        .sandboxProfileId = nullableString(value, "sandboxProfileId"),
    };
    const auto assets = value.find("assets");
    if (assets != value.end() && assets->is_object()) {
        for (const auto role : {"poster", "icon"}) {
            const auto asset = assets->find(role);
            if (asset == assets->end() || !asset->is_object()) continue;
            app.assetId = asset->value("id", "");
            app.assetPath = asset->value("url", "");
            app.assetRevision = asset->value("revision", std::uint64_t{0});
            if (!app.assetId.empty() && !app.assetPath.empty()) break;
        }
    }
    return app;
}
}  // namespace

ControlPlane::ControlPlane(std::filesystem::path dataPath) :
    dataPath_(std::move(dataPath)),
    statePath_(dataPath_ / "client-state.json"),
    gameStream_(std::make_unique<GameStreamClient>(ensureDataPath(dataPath_))) {
    load();
    disconnectThread_ = std::thread([this] { cleanupSessions(); });
}

ControlPlane::~ControlPlane() {
    {
        std::scoped_lock lock{disconnectMutex_};
        shuttingDown_ = true;
    }
    disconnectCondition_.notify_one();
    if (disconnectThread_.joinable()) disconnectThread_.join();

    std::shared_ptr<StreamSession> transport;
    std::unordered_map<std::string, std::jthread> apiEventThreads;
    {
        std::scoped_lock lock{mutex_};
        for (auto& [_, thread] : apiEventThreads_)
            thread.request_stop();
        apiEventThreads.swap(apiEventThreads_);
        sessionListener_ = {};
        streamOverlayListener_ = {};
        streamStatisticsListener_ = {};
        apiResourceListener_ = {};
        transport = std::exchange(transport_, {});
        session_.reset();
    }
    apiEventThreads.clear();
    if (transport) transport->stop();
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

HostRecord ControlPlane::discoverHost(std::string name, std::string address) {
    name = trim(std::move(name));
    if (name.empty()) throw std::invalid_argument("Discovered host name is empty.");
    const auto normalizedAddress = GameStreamClient::normalizeAddress(std::move(address));

    std::string existingId;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto existing = std::find_if(hosts_.begin(), hosts_.end(), [&](const auto& host) {
            return host.address == normalizedAddress;
        });
        if (existing != hosts_.end()) existingId = existing->id;
        clientId = clientId_;
    }
    if (!existingId.empty()) return probeHost(existingId);

    const auto info = gameStream_->probe(normalizedAddress, 0, clientId);
    HostRecord discovered{
        .id = ix::uuid4(),
        .name = std::move(name),
        .address = normalizedAddress,
        .serverName = info.serverName,
        .serverUniqueId = info.serverUniqueId,
        .appVersion = info.appVersion,
        .gfeVersion = info.gfeVersion,
        .serverState = info.serverState,
        .status = "online",
        .error = {},
        .serverCertificate = {},
        .httpsPort = info.httpsPort,
        .currentGameId = info.currentGameId,
        .serverCodecModeSupport = info.serverCodecModeSupport,
        .maxLumaPixelsHevc = info.maxLumaPixelsHevc,
        .displayModes = info.displayModes,
        .wakeMacAddress = {},
        .lastSeenAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count(),
        .paired = info.paired,
        .apiVersion = 0,
        .apiPort = 0,
        .capabilities = {},
        .apiClientUuid = {},
        .apiClientName = {},
        .apiScopes = {},
        .allowedApps = {},
        .features = Json::object(),
        .limits = Json::object(),
    };

    std::scoped_lock lock{mutex_};
    const auto duplicate = std::find_if(hosts_.begin(), hosts_.end(), [&](const auto& host) {
        return host.address == normalizedAddress ||
               (!host.serverUniqueId.empty() && host.serverUniqueId == info.serverUniqueId);
    });
    if (duplicate != hosts_.end()) return *duplicate;
    hosts_.push_back(discovered);
    saveLocked();
    return discovered;
}

HostRecord ControlPlane::probeHost(const std::string& id) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
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
        current.maxLumaPixelsHevc = info.maxLumaPixelsHevc;
        current.displayModes = info.displayModes;
        current.apiVersion = info.eclipseApiVersion;
        current.apiPort = info.eclipseApiPort;
        current.capabilities = info.eclipseCapabilities;
        if (!current.serverCertificate.empty() && !info.macAddress.empty()) {
            current.wakeMacAddress = info.macAddress;
        }
        current.paired = info.paired;
        current.status = "online";
        current.error.clear();
        if (current.paired && current.apiVersion == 1) {
            try {
                const auto capabilityResponse = gameStream_->apiRequest(
                    current.address, current.apiPort, current.serverCertificate, "GET",
                    "/eclipse/v1/capabilities");
                const auto& document = capabilityResponse.body;
                current.capabilities = stringArray(document.value("capabilities", Json::array()));
                current.features = document.value("features", Json::object());
                current.limits = document.value("limits", Json::object());
                if (const auto client = document.find("client");
                    client != document.end() && client->is_object()) {
                    current.apiClientUuid = client->value("uuid", "");
                    current.apiClientName = client->value("name", "");
                    current.apiScopes = stringArray(client->value("scopes", Json::array()));
                    current.allowedApps = stringArray(client->value("allowedApps", Json::array()));
                }
            } catch (const std::exception& exception) {
                current.error = std::string{"Sol API unavailable: "} + exception.what();
            }
        }
        current.lastSeenAt = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    } catch (const std::exception& exception) {
        current.status = "offline";
        current.error = exception.what();
    }

    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
        if (host == hosts_.end()) return current;
        *host = current;
        saveLocked();
    }
    if (current.status == "online" && current.paired && current.apiVersion == 1) {
        startApiEvents(current);
    } else {
        stopApiEvents(id);
    }
    return current;
}

std::vector<GameStreamApp> ControlPlane::loadApps(const std::string& hostId) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        if (host->status != "online" || !host->paired || host->serverCertificate.empty()) {
            throw std::runtime_error("Host must be paired and online to load applications.");
        }
        current = *host;
        clientId = clientId_;
    }

    std::vector<GameStreamApp> result;
    if (current.apiVersion == 1 && hasCapability(current, "catalog-v2")) {
        const auto response = gameStream_->apiRequest(
            current.address, current.apiPort, current.serverCertificate, "GET", "/eclipse/v1/apps");
        const auto apps = response.body.find("apps");
        if (apps == response.body.end() || !apps->is_array()) {
            throw std::runtime_error("Sol returned an invalid Catalog V2 collection.");
        }
        result.reserve(apps->size());
        for (const auto& app : *apps)
            result.push_back(catalogApp(app));
    } else {
        result = gameStream_->apps(current.address, current.httpsPort, clientId,
                                   current.serverCertificate);
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const auto& left, const auto& right) { return left.name < right.name; });
    {
        std::scoped_lock lock{mutex_};
        apps_[hostId] = result;
    }
    return result;
}

std::string ControlPlane::boxArtDataUrl(const std::string& hostId, int appId) {
    HostRecord current;
    GameStreamApp selected;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        const auto apps = apps_.find(hostId);
        if (host == hosts_.end() || apps == apps_.end()) {
            throw std::invalid_argument("Application no longer exists in this library.");
        }
        const auto app = std::ranges::find(apps->second, appId, &GameStreamApp::id);
        if (app == apps->second.end()) {
            throw std::invalid_argument("Application no longer exists in this library.");
        }
        current = *host;
        selected = *app;
        clientId = clientId_;
    }

    const auto cacheDirectory = dataPath_ / "box-art" / cacheKey(hostId);
    const auto cacheIdentity = selected.uuid.empty() ? std::to_string(appId) : selected.uuid;
    const auto cachePath = cacheDirectory / (cacheKey(cacheIdentity + "-" + selected.assetId + "-" +
                                                      std::to_string(selected.assetRevision)) +
                                             ".asset");
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

    if (!selected.assetPath.empty()) {
        bytes = gameStream_->apiBytes(current.address, current.apiPort, current.serverCertificate,
                                      selected.assetPath);
    } else {
        bytes = gameStream_->boxArt(current.address, current.httpsPort, clientId,
                                    current.serverCertificate, appId);
    }
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

nlohmann::json ControlPlane::loadApiResource(const std::string& hostId,
                                             const std::string& resource) {
    static const std::map<std::string, std::string, std::less<>> paths{
        {"capabilities", "/eclipse/v1/capabilities"},
        {"sessions", "/eclipse/v1/sessions"},
        {"telemetry", "/eclipse/v1/telemetry"},
        {"displays", "/eclipse/v1/displays"},
        {"display-topology", "/eclipse/v1/display-topology"},
        {"virtual-displays", "/eclipse/v1/virtual-displays"},
        {"profiles", "/eclipse/v1/profiles"},
        {"workspaces", "/eclipse/v1/workspaces"},
        {"sandboxes", "/eclipse/v1/sandboxes"},
        {"peripherals", "/eclipse/v1/peripherals"},
        {"peripheral-claims", "/eclipse/v1/peripherals/claims"},
    };
    const auto path = paths.find(resource);
    if (path == paths.end()) throw std::invalid_argument("Unknown Sol API resource collection.");

    HostRecord current;
    std::function<void(const ApiResourceUpdate&)> listener;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::ranges::find(hosts_, hostId, &HostRecord::id);
        if (host == hosts_.end()) throw std::invalid_argument("Host no longer exists.");
        if (host->status != "online" || !host->paired || host->apiVersion != 1) {
            throw std::runtime_error("Host API v1 must be paired and online.");
        }
        current = *host;
        listener = apiResourceListener_;
    }
    const auto response = gameStream_->apiRequest(current.address, current.apiPort,
                                                  current.serverCertificate, "GET", path->second);
    if (listener) listener({hostId, resource, response.body});
    return response.body;
}

nlohmann::json ControlPlane::mutateApiResource(const std::string& hostId, const std::string& method,
                                               const std::string& path, nlohmann::json body,
                                               const std::optional<std::uint64_t> revision,
                                               const bool idempotent) {
    HostRecord current;
    std::function<void(const ApiResourceUpdate&)> listener;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::ranges::find(hosts_, hostId, &HostRecord::id);
        if (host == hosts_.end()) throw std::invalid_argument("Host no longer exists.");
        if (host->status != "online" || !host->paired || host->apiVersion != 1) {
            throw std::runtime_error("Host API v1 must be paired and online.");
        }
        current = *host;
        listener = apiResourceListener_;
    }
    if (!body.is_object()) throw std::invalid_argument("Sol API mutation body must be an object.");
    body["schemaVersion"] = 1;
    std::map<std::string, std::string> headers;
    if (revision) headers["If-Match"] = '"' + std::to_string(*revision) + '"';
    if (idempotent) headers["Idempotency-Key"] = ix::uuid4();
    const auto response = gameStream_->apiRequest(
        current.address, current.apiPort, current.serverCertificate, method, path, body, headers);
    if (listener) listener({hostId, "mutation", response.body});
    return response.body;
}

SessionRecord ControlPlane::launchApp(const std::string& hostId, int appId,
                                      const StreamSettings& settings) {
    std::scoped_lock sessionLock{sessionMutex_};
    HostRecord current;
    GameStreamApp selected;
    std::string clientId;
    bool resume = false;
    std::uint64_t generation = 0;
    std::function<void(const SessionUpdate&)> listener;
    {
        std::scoped_lock lock{mutex_};
        if (transport_ || session_) {
            throw std::runtime_error("Terra already has an active stream. Stop it "
                                     "before launching again.");
        }
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        const auto apps = apps_.find(hostId);
        if (host == hosts_.end() || apps == apps_.end()) {
            throw std::invalid_argument("Load this host's application library before launching.");
        }
        const auto app =
            std::find_if(apps->second.begin(), apps->second.end(),
                         [&](const GameStreamApp& value) { return value.id == appId; });
        if (app == apps->second.end()) {
            throw std::invalid_argument("Application no longer exists in this library.");
        }
        if (host->status != "online" || !host->paired || host->serverCertificate.empty()) {
            throw std::runtime_error("Host must be paired and online to launch applications.");
        }
        if (host->serverCodecModeSupport == 0) {
            throw std::runtime_error(
                "Host did not report streaming codec support. Refresh it first.");
        }
        if (host->currentGameId != 0 && host->currentGameId != appId) {
            throw std::runtime_error("Another application is already running on this host.");
        }
        current = *host;
        selected = *app;
        clientId = clientId_;
        resume = host->currentGameId == appId;
        generation = ++sessionGeneration_;
        sessionStopRequested_ = false;
        launchCancellationRequested_.store(false);
        session_ = SessionRecord{
            .hostId = hostId,
            .appId = appId,
            .appName = selected.name,
            .sessionUrl = {},
            .resumed = resume,
            .logicalSessionId = {},
            .remoteInputKey = {},
            .remoteInputIv = {},
        };
        listener = sessionListener_;
    }
    if (listener) {
        try {
            listener({
                .hostId = hostId,
                .appId = appId,
                .appName = selected.name,
                .state = "launching",
                .message = "Requesting native host session.",
                .resumed = resume,
            });
        } catch (...) {
        }
    }
    int videoFormat = VIDEO_FORMAT_H264;
    LaunchResult launched;
    auto effectiveSettings = settings;
    try {
        if (effectiveSettings.audioConfig != AudioConfig::stereo &&
            !AudioRenderer::supportsOutputChannels(
                audioChannelCount(effectiveSettings.audioConfig))) {
            effectiveSettings.audioConfig = AudioConfig::stereo;
            if (listener) {
                try {
                    listener({
                        .hostId = hostId,
                        .appId = appId,
                        .appName = selected.name,
                        .state = "launching",
                        .message = "Selected surround layout is unavailable; using "
                                   "stereo audio.",
                        .resumed = resume,
                    });
                } catch (...) {
                }
            }
        }
        auto codecModeSupport = current.serverCodecModeSupport;
        if (current.maxLumaPixelsHevc == 0) {
            codecModeSupport &=
                ~(SCM_HEVC | SCM_HEVC_MAIN10 | SCM_HEVC_REXT8_444 | SCM_HEVC_REXT10_444);
        }
        videoFormat =
            selectVideoFormat(effectiveSettings.videoCodec, codecModeSupport,
                              effectiveSettings.enableHdr, effectiveSettings.enableYuv444);
        const auto requestedPixels = static_cast<std::uint64_t>(effectiveSettings.width) *
                                     static_cast<std::uint64_t>(effectiveSettings.height);
        if ((videoFormat & VIDEO_FORMAT_MASK_H265) != 0 &&
            requestedPixels > current.maxLumaPixelsHevc) {
            throw std::runtime_error("Requested resolution exceeds this host's "
                                     "reported HEVC encoder limit.");
        }
        launched = gameStream_->launch(current.address, current.httpsPort, clientId,
                                       current.serverCertificate, appId, resume, effectiveSettings,
                                       &launchCancellationRequested_, selected.uuid);
    } catch (...) {
        const auto failure = std::current_exception();
        const bool cancelled = launchCancellationRequested_.load();
        {
            std::scoped_lock lock{mutex_};
            if (sessionGeneration_ == generation) {
                session_.reset();
                sessionStopRequested_ = false;
                ++sessionGeneration_;
            }
        }
        if (cancelled) {
            try {
                static_cast<void>(probeHost(hostId));
            } catch (...) {
            }
            return SessionRecord{
                .hostId = hostId,
                .appId = appId,
                .appName = selected.name,
                .sessionUrl = {},
                .resumed = resume,
                .logicalSessionId = {},
                .remoteInputKey = {},
                .remoteInputIv = {},
            };
        }
        std::rethrow_exception(failure);
    }
    SessionRecord session{
        .hostId = hostId,
        .appId = appId,
        .appName = selected.name,
        .sessionUrl = launched.sessionUrl,
        .resumed = launched.resumed,
        .logicalSessionId = launched.logicalSessionId,
        .remoteInputKey = launched.remoteInputKey,
        .remoteInputIv = launched.remoteInputIv,
    };
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        if (host != hosts_.end()) {
            host->currentGameId = appId;
            host->serverState = "SUNSHINE_SERVER_BUSY";
            saveLocked();
        }
        session_ = session;
    }

    auto transport = std::make_shared<StreamSession>(
        StreamSessionConfig{
            .hostId = hostId,
            .appId = appId,
            .appName = selected.name,
            .address = GameStreamClient::streamHost(current.address),
            .appVersion = current.appVersion,
            .gfeVersion = current.gfeVersion,
            .serverCodecModeSupport = current.serverCodecModeSupport,
            .videoFormat = videoFormat,
            .settings = effectiveSettings,
            .launch = launched,
        },
        [this, hostId, appId, appName = selected.name, resumed = launched.resumed,
         generation](const StreamSessionEvent& event) {
            std::function<void(const SessionUpdate&)> listener;
            {
                std::scoped_lock lock{mutex_};
                if (sessionGeneration_ != generation || !session_) return;
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
        },
        [this, generation, hostId, appId, appName = selected.name, resumed = launched.resumed,
         quitAppAfter = settings.quitAppAfter](StreamSessionEvent event, bool hostEnded,
                                               bool userEnded) {
            requestSessionDisconnect(generation, hostId, appId, appName, resumed, std::move(event),
                                     hostEnded, userEnded && quitAppAfter);
        },
        [this, generation, hostId, appId, appName = selected.name, effectiveSettings,
         videoFormat](const StreamWindowBounds& bounds) {
            std::function<void(const StreamOverlayRequest&)> listener;
            {
                std::scoped_lock lock{mutex_};
                if (sessionGeneration_ != generation || !session_) return;
                listener = streamOverlayListener_;
            }
            if (listener) {
                listener({
                    .hostId = hostId,
                    .appId = appId,
                    .appName = appName,
                    .generation = generation,
                    .revision = bounds.revision,
                    .visible = bounds.visible,
                    .x = bounds.x,
                    .y = bounds.y,
                    .width = bounds.width,
                    .height = bounds.height,
                    .scaleFactor = bounds.scaleFactor,
                    .wayland = bounds.wayland,
                    .fullscreen = bounds.fullscreen,
                    .settings = effectiveSettings,
                    .videoFormat = videoFormat,
                    .controllerMask = connectedGamepadMask(),
                });
            }
        },
        [this, generation, hostId](const StreamStatisticsSample& sample) {
            std::function<void(const StreamStatisticsUpdate&)> listener;
            {
                std::scoped_lock lock{mutex_};
                if (sessionGeneration_ != generation || !session_) return;
                listener = streamStatisticsListener_;
            }
            if (listener) {
                listener({
                    .hostId = hostId,
                    .generation = generation,
                    .sample = sample,
                });
            }
        });
    {
        std::scoped_lock lock{mutex_};
        if (sessionGeneration_ != generation || !session_) {
            transport->requestStop();
        } else {
            transport_ = transport;
            if (sessionStopRequested_) transport->requestStop();
        }
    }
    try {
        transport->start();
    } catch (...) {
        const bool cancelled = transport->stopRequested();
        bool explicitStop = false;
        {
            std::scoped_lock lock{mutex_};
            explicitStop = sessionGeneration_ == generation && sessionStopRequested_;
        }
        if (!cancelled && !launched.resumed) {
            try {
                gameStream_->cancel(current.address, current.httpsPort, clientId,
                                    current.serverCertificate);
            } catch (...) {
            }
        }
        {
            std::scoped_lock lock{mutex_};
            if (sessionGeneration_ == generation) {
                if (transport_ == transport) transport_.reset();
                session_.reset();
                sessionStopRequested_ = false;
                if (!cancelled && !launched.resumed) {
                    const auto host =
                        std::find_if(hosts_.begin(), hosts_.end(),
                                     [&](const HostRecord& value) { return value.id == hostId; });
                    if (host != hosts_.end()) {
                        host->currentGameId = 0;
                        host->serverState = "SUNSHINE_SERVER_IDLE";
                        saveLocked();
                    }
                }
                if (!cancelled || explicitStop) ++sessionGeneration_;
            }
        }
        if (cancelled) return session;
        throw;
    }
    return session;
}

void ControlPlane::stopSession(const std::string& hostId, bool quitHost) {
    HostRecord current;
    std::string clientId;
    std::shared_ptr<StreamSession> transport;
    std::uint64_t generation = 0;
    bool ownsSession = false;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        if (host == hosts_.end()) {
            throw std::invalid_argument("Host no longer exists.");
        }
        ownsSession = session_ && session_->hostId == hostId;
        if (!ownsSession && (!quitHost || host->currentGameId == 0)) {
            return;
        }
        current = *host;
        clientId = clientId_;
        if (ownsSession) {
            generation = sessionGeneration_;
            sessionStopRequested_ = true;
            launchCancellationRequested_.store(true);
            transport = transport_;
        }
    }

    if (transport) {
        transport->requestStop();
    } else if (ownsSession) {
        LiInterruptConnection();
    }

    std::scoped_lock sessionLock{sessionMutex_};
    if (ownsSession && !transport) {
        std::scoped_lock lock{mutex_};
        if (sessionGeneration_ == generation) transport = transport_;
    }
    if (transport) transport->stop();
    {
        std::scoped_lock lock{mutex_};
        if (ownsSession && sessionGeneration_ == generation) {
            if (!transport || transport_ == transport) transport_.reset();
            session_.reset();
            sessionStopRequested_ = false;
            ++sessionGeneration_;
        }
    }

    if (quitHost) {
        gameStream_->cancel(current.address, current.httpsPort, clientId,
                            current.serverCertificate);
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == hostId; });
        if (host != hosts_.end()) {
            host->currentGameId = 0;
            host->serverState = "SUNSHINE_SERVER_IDLE";
            saveLocked();
        }
    }
}

void ControlPlane::closeStreamOverlay(const std::string& hostId, std::uint64_t generation,
                                      std::uint64_t revision) {
    std::shared_ptr<StreamSession> transport;
    {
        std::scoped_lock lock{mutex_};
        if (!session_ || session_->hostId != hostId || sessionGeneration_ != generation) return;
        transport = transport_;
    }
    if (transport) transport->closeOverlay(revision);
}

void ControlPlane::acknowledgeStreamOverlayHidden(const std::string& hostId,
                                                  std::uint64_t generation,
                                                  std::uint64_t revision) {
    std::shared_ptr<StreamSession> transport;
    {
        std::scoped_lock lock{mutex_};
        if (!session_ || session_->hostId != hostId || sessionGeneration_ != generation) return;
        transport = transport_;
    }
    if (transport) transport->acknowledgeOverlayHidden(revision);
}

void ControlPlane::requestSessionDisconnect(std::uint64_t generation, std::string hostId, int appId,
                                            std::string appName, bool resumed,
                                            StreamSessionEvent event, bool hostEnded,
                                            bool quitHost) {
    {
        std::scoped_lock lock{mutex_};
        if (sessionGeneration_ != generation) return;
    }
    LiInterruptConnection();
    {
        std::scoped_lock lock{disconnectMutex_};
        if (shuttingDown_) return;
        disconnectRequest_ = DisconnectRequest{
            .generation = generation,
            .hostEnded = hostEnded,
            .quitHost = quitHost,
            .hostId = std::move(hostId),
            .appId = appId,
            .appName = std::move(appName),
            .resumed = resumed,
            .state = std::move(event.state),
            .message = std::move(event.message),
        };
    }
    disconnectCondition_.notify_one();
}

void ControlPlane::cleanupSessions() {
    while (true) {
        DisconnectRequest request;
        {
            std::unique_lock lock{disconnectMutex_};
            disconnectCondition_.wait(
                lock, [this] { return shuttingDown_ || disconnectRequest_.has_value(); });
            if (shuttingDown_) return;
            request = *std::exchange(disconnectRequest_, std::nullopt);
        }
        try {
            finishSessionDisconnect(request);
        } catch (const std::exception& exception) {
            std::fprintf(stderr, "[terra-core] session cleanup failed: %s\n", exception.what());
        } catch (...) {
            std::fputs("[terra-core] session cleanup failed with an unknown error.\n", stderr);
        }
    }
}

void ControlPlane::finishSessionDisconnect(const DisconnectRequest& request) {
    std::scoped_lock sessionLock{sessionMutex_};
    std::shared_ptr<StreamSession> transport;
    SessionRecord endingSession;
    HostRecord current;
    std::string clientId;
    bool hasHost = false;
    {
        std::scoped_lock lock{mutex_};
        if (sessionGeneration_ != request.generation) return;
        transport = transport_;
        endingSession = session_.value_or(SessionRecord{
            .hostId = request.hostId,
            .appId = request.appId,
            .appName = request.appName,
            .sessionUrl = {},
            .resumed = request.resumed,
            .logicalSessionId = {},
            .remoteInputKey = {},
            .remoteInputIv = {},
        });
        const auto host = std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
            return value.id == endingSession.hostId;
        });
        if (host != hosts_.end()) {
            current = *host;
            hasHost = true;
        }
        clientId = clientId_;
    }
    if (transport) transport->stop();

    bool hostEnded = request.hostEnded;
    std::string state = request.state;
    std::string message = request.message;
    if (request.quitHost && hasHost) {
        try {
            gameStream_->cancel(current.address, current.httpsPort, clientId,
                                current.serverCertificate);
            hostEnded = true;
            message = "Render window closed; host application stopped.";
        } catch (const std::exception& exception) {
            state = "error";
            message = std::string{"Stream disconnected, but the host application "
                                  "could not be stopped: "} +
                      exception.what();
        }
    }

    std::function<void(const SessionUpdate&)> listener;
    {
        std::scoped_lock lock{mutex_};
        if (sessionGeneration_ != request.generation) return;
        if (!transport || transport_ == transport) transport_.reset();
        session_.reset();
        sessionStopRequested_ = false;
        ++sessionGeneration_;
        if (hostEnded) {
            const auto host =
                std::find_if(hosts_.begin(), hosts_.end(), [&](const HostRecord& value) {
                    return value.id == endingSession.hostId;
                });
            if (host != hosts_.end()) {
                host->currentGameId = 0;
                host->serverState = "SUNSHINE_SERVER_IDLE";
                saveLocked();
            }
        }
        listener = sessionListener_;
    }
    if (listener) {
        listener({
            .hostId = endingSession.hostId,
            .appId = endingSession.appId,
            .appName = endingSession.appName,
            .state = state,
            .message = message,
            .resumed = endingSession.resumed,
        });
    }
}

void ControlPlane::setSessionListener(std::function<void(const SessionUpdate&)> listener) {
    std::scoped_lock lock{mutex_};
    sessionListener_ = std::move(listener);
}

void ControlPlane::setStreamOverlayListener(
    std::function<void(const StreamOverlayRequest&)> listener) {
    std::scoped_lock lock{mutex_};
    streamOverlayListener_ = std::move(listener);
}

void ControlPlane::setStreamStatisticsListener(
    std::function<void(const StreamStatisticsUpdate&)> listener) {
    std::scoped_lock lock{mutex_};
    streamStatisticsListener_ = std::move(listener);
}

void ControlPlane::setApiResourceListener(std::function<void(const ApiResourceUpdate&)> listener) {
    std::scoped_lock lock{mutex_};
    apiResourceListener_ = std::move(listener);
}

void ControlPlane::startApiEvents(const HostRecord& host) {
    std::scoped_lock lock{mutex_};
    if (apiEventThreads_.contains(host.id)) return;
    apiEventThreads_.try_emplace(host.id, [this, host](const std::stop_token stopToken) {
        std::string lastEventId;
        while (!stopToken.stop_requested()) {
            std::atomic_bool cancelled = false;
            std::stop_callback cancel{stopToken, [&] { cancelled.store(true); }};
            SseDecoder decoder;
            try {
                gameStream_->streamEvents(
                    host.address, host.apiPort, host.serverCertificate, "/eclipse/v1/events",
                    lastEventId, cancelled, [&](const std::string& chunk) {
                        decoder.push(chunk, [&](const std::string& id, const Json& event) {
                            if (!id.empty()) lastEventId = id;
                            std::function<void(const ApiResourceUpdate&)> listener;
                            {
                                std::scoped_lock listenerLock{mutex_};
                                listener = apiResourceListener_;
                            }
                            if (listener) listener({host.id, "event", event});
                        });
                    });
            } catch (const std::exception& exception) {
                if (!stopToken.stop_requested()) {
                    std::fprintf(stderr, "[terra-core] Sol event stream failed: %s\n",
                                 exception.what());
                }
            }
            for (int delay = 0; delay < 20 && !stopToken.stop_requested(); ++delay) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }
    });
}

void ControlPlane::stopApiEvents(const std::string& hostId) {
    std::jthread thread;
    {
        std::scoped_lock lock{mutex_};
        const auto found = apiEventThreads_.find(hostId);
        if (found == apiEventThreads_.end()) return;
        found->second.request_stop();
        thread = std::move(found->second);
        apiEventThreads_.erase(found);
    }
}

HostRecord ControlPlane::pairHost(const std::string& id, const std::string& pin,
                                  PairingAccess access) {
    HostRecord current;
    std::string clientId;
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
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
                                                      current.appVersion, clientId, pin, access);
        current.paired = true;
        current.status = "online";
        current.error.clear();
        try {
            const auto info = gameStream_->probe(current.address, current.httpsPort, clientId,
                                                 current.serverCertificate);
            if (!info.macAddress.empty()) current.wakeMacAddress = info.macAddress;
            current.serverState = info.serverState;
            current.currentGameId = info.currentGameId;
            current.serverCodecModeSupport = info.serverCodecModeSupport;
            current.maxLumaPixelsHevc = info.maxLumaPixelsHevc;
            current.displayModes = info.displayModes;
            current.apiVersion = info.eclipseApiVersion;
            current.apiPort = info.eclipseApiPort;
            current.capabilities = info.eclipseCapabilities;
            if (current.apiVersion == 1) {
                const auto capabilityResponse = gameStream_->apiRequest(
                    current.address, current.apiPort, current.serverCertificate, "GET",
                    "/eclipse/v1/capabilities");
                const auto& document = capabilityResponse.body;
                current.capabilities = stringArray(document.value("capabilities", Json::array()));
                current.features = document.value("features", Json::object());
                current.limits = document.value("limits", Json::object());
                if (const auto client = document.find("client");
                    client != document.end() && client->is_object()) {
                    current.apiClientUuid = client->value("uuid", "");
                    current.apiClientName = client->value("name", "");
                    current.apiScopes = stringArray(client->value("scopes", Json::array()));
                    current.allowedApps = stringArray(client->value("allowedApps", Json::array()));
                }
            }
        } catch (...) {
        }
    } catch (const std::exception& exception) {
        current.status = current.serverName.empty() ? "offline" : "online";
        current.error = exception.what();
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
        if (host != hosts_.end()) {
            *host = current;
            saveLocked();
        }
        throw;
    }

    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
        if (host != hosts_.end()) {
            *host = current;
            saveLocked();
        }
    }
    if (current.paired && current.apiVersion == 1) startApiEvents(current);
    return current;
}

void ControlPlane::wakeHost(const std::string& id) {
    MacAddress address{};
    {
        std::scoped_lock lock{mutex_};
        const auto host = std::find_if(hosts_.begin(), hosts_.end(),
                                       [&](const HostRecord& value) { return value.id == id; });
        if (host == hosts_.end()) throw std::invalid_argument("Host no longer exists.");
        if (host->status != "offline") {
            throw std::runtime_error("Wake-on-LAN is available only while host is offline.");
        }
        const auto parsed = parseMacAddress(host->wakeMacAddress);
        if (!parsed) {
            throw std::runtime_error("Wake-on-LAN is unavailable until this paired "
                                     "host reports a valid MAC address.");
        }
        const auto now = std::chrono::steady_clock::now();
        const auto previous = lastWakeRequests_.find(id);
        if (previous != lastWakeRequests_.end() &&
            now - previous->second < std::chrono::seconds{5}) {
            throw std::runtime_error("Wait five seconds before sending another wake request.");
        }
        lastWakeRequests_[id] = now;
        address = *parsed;
    }
    sendWakeOnLan(address);
}

void ControlPlane::removeHost(const std::string& id) {
    {
        std::scoped_lock lock{mutex_};
        if (session_ && session_->hostId == id) {
            throw std::runtime_error("Stop the active session before removing this host.");
        }
        std::erase_if(hosts_, [&](const HostRecord& host) { return host.id == id; });
        apps_.erase(id);
        lastWakeRequests_.erase(id);
        saveLocked();
    }
    stopApiEvents(id);
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
            throw std::runtime_error("Cannot read native client state: " +
                                     std::string{exception.what()});
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

}  // namespace terra
