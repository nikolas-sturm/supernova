#include "gamestream_client.h"

#include "input_forwarder.h"
#include "wake_on_lan.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXSocketTLSOptions.h>
#include <ixwebsocket/IXUuid.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <pugixml.hpp>

namespace terra {
namespace {
using Bytes = std::vector<unsigned char>;

constexpr std::uint16_t kDefaultHttpPort = 47989;
constexpr std::uint16_t kDefaultHttpsPort = 47984;
constexpr int kRequestTimeoutSeconds = 5;
constexpr int kLaunchTimeoutSeconds = 120;
/// Allows Sol's five-minute PIN session to return its final response.
constexpr int kPairingPinTimeoutSeconds = 310;
constexpr std::size_t kMaximumApiResponseBytes = 8 * 1024 * 1024;

struct Endpoint {
    std::string host;
    std::uint16_t port;
    std::string canonicalAddress;
};

template <typename Type, auto FreeFunction> struct OpenSslDeleter {
    void operator()(Type* value) const {
        if (value != nullptr) {
            static_cast<void>(FreeFunction(value));
        }
    }
};

template <typename Type, auto FreeFunction>
using OpenSslPointer = std::unique_ptr<Type, OpenSslDeleter<Type, FreeFunction>>;

[[noreturn]] void throwOpenSsl(const std::string& message) {
    char detail[256]{};
    const auto error = ERR_get_error();
    if (error != 0) {
        ERR_error_string_n(error, detail, sizeof(detail));
    }
    throw std::runtime_error(message + (error == 0 ? "" : ": " + std::string{detail}));
}

void requireOpenSsl(int result, const std::string& message) {
    if (result <= 0) {
        throwOpenSsl(message);
    }
}

std::string trim(std::string value) {
    const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

void collectDisplayModes(const pugi::xml_node& node, std::vector<HostDisplayMode>& result) {
    for (const auto child : node.children()) {
        if (std::string_view{child.name()} == "DisplayMode") {
            HostDisplayMode mode{
                .width = child.child("Width").text().as_int(0),
                .height = child.child("Height").text().as_int(0),
                .refreshRate = child.child("RefreshRate").text().as_int(0),
            };
            if (mode.width > 0 && mode.width <= 16384 && mode.height > 0 && mode.height <= 16384 &&
                mode.refreshRate > 0 && mode.refreshRate <= 1000) {
                result.push_back(mode);
            }
        } else {
            collectDisplayModes(child, result);
        }
    }
}

std::uint16_t parsePort(const std::string& value) {
    if (value.empty() ||
        !std::all_of(value.begin(), value.end(),
                     [](const unsigned char character) { return std::isdigit(character); })) {
        throw std::invalid_argument("Host port must be a number.");
    }
    const auto parsed = std::stoul(value);
    if (parsed == 0 || parsed > 65535) {
        throw std::invalid_argument("Host port must be between 1 and 65535.");
    }
    return static_cast<std::uint16_t>(parsed);
}

Endpoint parseEndpoint(std::string address) {
    address = trim(std::move(address));
    constexpr std::string_view httpPrefix = "http://";
    constexpr std::string_view httpsPrefix = "https://";
    if (address.starts_with(httpPrefix)) {
        address.erase(0, httpPrefix.size());
    } else if (address.starts_with(httpsPrefix)) {
        throw std::invalid_argument("Enter Sol HTTP address, not an HTTPS URL.");
    }
    if (address.empty() || address.find_first_of("/?#") != std::string::npos) {
        throw std::invalid_argument("Enter a host name or IP address without a path.");
    }

    Endpoint endpoint{{}, kDefaultHttpPort, {}};
    if (address.front() == '[') {
        const auto closingBracket = address.find(']');
        if (closingBracket == std::string::npos || closingBracket == 1) {
            throw std::invalid_argument("Invalid bracketed IPv6 address.");
        }
        endpoint.host = address.substr(1, closingBracket - 1);
        if (closingBracket + 1 < address.size()) {
            if (address[closingBracket + 1] != ':') {
                throw std::invalid_argument("Invalid IPv6 host port.");
            }
            endpoint.port = parsePort(address.substr(closingBracket + 2));
        }
    } else {
        const auto colonCount = std::count(address.begin(), address.end(), ':');
        if (colonCount == 1) {
            const auto separator = address.find(':');
            endpoint.host = address.substr(0, separator);
            endpoint.port = parsePort(address.substr(separator + 1));
        } else {
            endpoint.host = address;
        }
    }

    if (endpoint.host.empty() || endpoint.host.find_first_of(" \t\r\n") != std::string::npos) {
        throw std::invalid_argument("Host name or IP address is invalid.");
    }
    const bool ipv6 = endpoint.host.find(':') != std::string::npos;
    endpoint.canonicalAddress = ipv6 ? "[" + endpoint.host + "]" : endpoint.host;
    if (endpoint.port != kDefaultHttpPort) {
        endpoint.canonicalAddress += ":" + std::to_string(endpoint.port);
    }
    return endpoint;
}

std::string compactUuid() {
    auto value = ix::uuid4();
    std::erase(value, '-');
    return value;
}

std::string urlHost(const Endpoint& endpoint) {
    if (endpoint.host.find(':') == std::string::npos) return endpoint.host;
    auto host = endpoint.host;
    for (std::size_t offset = 0; (offset = host.find('%', offset)) != std::string::npos;
         offset += 3) {
        host.replace(offset, 1, "%25");
    }
    return "[" + host + "]";
}

std::string urlEncode(const std::string& value) {
    ix::HttpClient client;
    return client.urlEncode(value);
}

void configureTls(ix::HttpClient& client, const Identity& identity,
                  const std::string& serverCertificate) {
    if (serverCertificate.empty()) {
        throw std::runtime_error("Pinned server certificate is required for HTTPS.");
    }
    ix::SocketTLSOptions tls;
    tls.tls = true;
    tls.certFile = identity.certificatePath().string();
    tls.keyFile = identity.runtimeKeyPath().string();
    tls.caFile = serverCertificate;
    tls.disable_hostname_validation = true;
    client.setTLSOptions(tls);
}

std::jthread monitorCancellation(const ix::HttpRequestArgsPtr& arguments,
                                 const std::atomic_bool* cancellation) {
    if (cancellation == nullptr) return {};
    arguments->cancel.store(cancellation->load());
    return std::jthread([arguments, cancellation](const std::stop_token stopToken) {
        while (!stopToken.stop_requested() && !cancellation->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (cancellation->load()) arguments->cancel.store(true);
    });
}

std::string request(const Endpoint& endpoint, std::uint16_t port, bool https,
                    const std::string& command, const std::string& arguments,
                    const std::string& clientId, const Identity& identity,
                    const std::string& serverCertificate, int timeoutSeconds,
                    const std::atomic_bool* cancellation = nullptr) {
    const auto url = std::string{https ? "https://" : "http://"} + urlHost(endpoint) + ":" +
                     std::to_string(port) + "/" + command + "?uniqueid=" + clientId +
                     "&uuid=" + compactUuid() + (arguments.empty() ? "" : "&" + arguments);

    ix::HttpClient client;
    if (https) {
        configureTls(client, identity, serverCertificate);
    }

    const auto requestArguments = client.createRequest();
    requestArguments->connectTimeout = std::min(timeoutSeconds, 5);
    requestArguments->transferTimeout = timeoutSeconds;
    requestArguments->followRedirects = false;
    requestArguments->compress = false;
    requestArguments->extraHeaders["Connection"] = "close";
    auto cancellationMonitor = monitorCancellation(requestArguments, cancellation);
    const auto response = client.get(url, requestArguments);
    if (cancellationMonitor.joinable()) cancellationMonitor.request_stop();
    if (!response || response->errorCode != ix::HttpErrorCode::Ok) {
        throw std::runtime_error(response ? response->errorMsg : "No HTTP response.");
    }
    if (response->statusCode != 200) {
        throw std::runtime_error("Sol returned HTTP " + std::to_string(response->statusCode) + ".");
    }
    return response->body;
}

std::uint64_t parseUnsignedField(const pugi::xml_node& root, const char* name,
                                 std::uint64_t maximum) {
    const auto node = root.child(name);
    if (!node) return 0;
    const std::string value = node.text().as_string();
    if (value.empty() || !std::ranges::all_of(value, [](const unsigned char character) {
            return std::isdigit(character);
        })) {
        throw std::runtime_error(std::string{"Sol returned invalid "} + name + ".");
    }
    try {
        const auto parsed = std::stoull(value);
        if (parsed <= maximum) return parsed;
    } catch (const std::exception&) {
    }
    throw std::runtime_error(std::string{"Sol returned out-of-range "} + name + ".");
}

std::vector<std::string> parseCsv(const std::string& csv) {
    std::vector<std::string> result;
    for (std::size_t begin = 0; begin <= csv.size();) {
        const auto end = csv.find(',', begin);
        auto value = trim(csv.substr(begin, end - begin));
        if (!value.empty() && !std::ranges::contains(result, value)) {
            result.push_back(std::move(value));
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}

void validateApiPath(const std::string& path) {
    constexpr std::string_view prefix = "/eclipse/v1";
    if (!path.starts_with(prefix) ||
        (path.size() > prefix.size() && path[prefix.size()] != '/' && path[prefix.size()] != '?') ||
        path.find_first_of("\\\r\n#") != std::string::npos ||
        std::ranges::any_of(path, [](const unsigned char character) {
            return character < 0x20 || character == 0x7f;
        })) {
        throw std::invalid_argument("Eclipse API path must begin with /eclipse/v1.");
    }
    auto lowered = path.substr(0, path.find('?'));
    std::ranges::transform(lowered, lowered.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (lowered.find("/../") != std::string::npos || lowered.ends_with("/..") ||
        lowered.find("/./") != std::string::npos || lowered.ends_with("/.") ||
        lowered.find("%2e") != std::string::npos || lowered.find("%2f") != std::string::npos ||
        lowered.find("%5c") != std::string::npos || lowered.find("%25") != std::string::npos) {
        throw std::invalid_argument("Eclipse API path cannot contain traversal segments.");
    }
}

void validateHeaders(const std::map<std::string, std::string>& headers) {
    for (const auto& [name, value] : headers) {
        if (name.empty() ||
            !std::ranges::all_of(name,
                                 [](const unsigned char character) {
                                     return std::isalnum(character) ||
                                            std::string_view{"!#$%&'*+-.^_`|~"}.contains(character);
                                 }) ||
            value.find_first_of("\r\n") != std::string::npos) {
            throw std::invalid_argument("HTTP header contains invalid characters.");
        }
    }
}

std::map<std::string, std::string> copyHeaders(const ix::WebSocketHttpHeaders& headers) {
    return std::map<std::string, std::string>{headers.begin(), headers.end()};
}

[[noreturn]] void throwApiError(int status, const std::string& body) {
    try {
        const auto parsed = nlohmann::json::parse(body);
        const auto& error = parsed.at("error");
        throw ApiError(status, error.at("code").get<std::string>(),
                       error.at("message").get<std::string>(),
                       error.value("details", nlohmann::json{}));
    } catch (const ApiError&) {
        throw;
    } catch (const std::exception&) {
        throw ApiError(status, "http_error", "Sol returned HTTP " + std::to_string(status) + ".",
                       nlohmann::json{});
    }
}

struct RawApiResponse {
    int status;
    std::map<std::string, std::string> headers;
    std::string body;
};

RawApiResponse apiCall(const Endpoint& endpoint, std::uint16_t port,
                       const std::string& serverCertificate, const Identity& identity,
                       const std::string& method, const std::string& path,
                       const std::optional<std::string>& body,
                       const std::map<std::string, std::string>& headers) {
    validateApiPath(path);
    validateHeaders(headers);
    if (method != "GET" && method != "POST" && method != "PATCH" && method != "PUT" &&
        method != "DELETE") {
        throw std::invalid_argument("Unsupported Eclipse API HTTP method.");
    }

    const int attempts = method == "GET" ? 2 : 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        ix::HttpClient client;
        configureTls(client, identity, serverCertificate);
        if (method == "DELETE" && body) client.setForceBody(true);
        const auto arguments = client.createRequest();
        arguments->connectTimeout = 5;
        arguments->transferTimeout = 120;
        arguments->followRedirects = false;
        arguments->compress = false;
        arguments->extraHeaders = ix::WebSocketHttpHeaders{headers.begin(), headers.end()};
        if (!arguments->extraHeaders.contains("Accept")) {
            arguments->extraHeaders["Accept"] = "application/json";
        }
        arguments->extraHeaders["Connection"] = "close";
        if (body) arguments->extraHeaders["Content-Type"] = "application/json";

        std::string responseBody;
        bool tooLarge = false;
        arguments->onChunkCallback = [&](const std::string& chunk) {
            if (chunk.size() > kMaximumApiResponseBytes - responseBody.size()) {
                tooLarge = true;
                arguments->cancel.store(true);
                return;
            }
            responseBody += chunk;
        };
        const auto response =
            client.request("https://" + urlHost(endpoint) + ":" +
                               std::to_string(port == 0 ? kDefaultHttpsPort : port) + path,
                           method, body.value_or(""), arguments);
        if (tooLarge) throw std::runtime_error("Sol API response exceeds 8 MiB.");
        if (!response || response->errorCode != ix::HttpErrorCode::Ok) {
            if (attempt + 1 < attempts && response &&
                response->errorCode == ix::HttpErrorCode::CannotReadStatusLine) {
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                continue;
            }
            throw std::runtime_error(response ? response->errorMsg : "No HTTP response.");
        }
        if (response->statusCode < 200 || response->statusCode >= 300) {
            throwApiError(response->statusCode, responseBody);
        }
        return {response->statusCode, copyHeaders(response->headers), std::move(responseBody)};
    }
    throw std::runtime_error("No HTTP response.");
}

pugi::xml_node parseRoot(pugi::xml_document& document, const std::string& xml) {
    const auto parsed = document.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw std::runtime_error("Sol returned malformed XML.");
    }
    const auto root = document.child("root");
    if (!root) {
        throw std::runtime_error("Sol response is missing root element.");
    }
    const auto statusCode = root.attribute("status_code").as_int(-1);
    if (statusCode != 200) {
        const auto message = root.attribute("status_message").as_string("Unknown error");
        throw std::runtime_error("Sol status " + std::to_string(statusCode) + ": " + message);
    }
    return root;
}

std::string childText(const pugi::xml_node& root, const char* name) {
    return root.child(name).text().as_string();
}

void requireSecureRequest(const std::string& serverCertificate, int appId = 0) {
    if (serverCertificate.empty()) {
        throw std::runtime_error("Pair this host before using its application library.");
    }
    if (appId < 0) {
        throw std::invalid_argument("Application ID is invalid.");
    }
}

std::string toHex(std::span<const unsigned char> data) {
    constexpr char digits[] = "0123456789abcdef";
    std::string output(data.size() * 2, '0');
    for (std::size_t index = 0; index < data.size(); ++index) {
        output[index * 2] = digits[data[index] >> 4U];
        output[index * 2 + 1] = digits[data[index] & 0x0fU];
    }
    return output;
}

std::string toHex(const std::string& data) {
    return toHex(std::span{reinterpret_cast<const unsigned char*>(data.data()), data.size()});
}

Bytes fromHex(const std::string& value) {
    if (value.size() % 2 != 0) {
        throw std::runtime_error("Sol returned invalid hexadecimal data.");
    }
    const auto nibble = [](const unsigned char character) -> unsigned char {
        if (character >= '0' && character <= '9') return character - '0';
        if (character >= 'a' && character <= 'f') return character - 'a' + 10;
        if (character >= 'A' && character <= 'F') return character - 'A' + 10;
        throw std::runtime_error("Sol returned invalid hexadecimal data.");
    };
    Bytes output(value.size() / 2);
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<unsigned char>((nibble(value[index * 2]) << 4U) |
                                                   nibble(value[index * 2 + 1]));
    }
    return output;
}

Bytes randomBytes(std::size_t size) {
    Bytes output(size);
    requireOpenSsl(RAND_bytes(output.data(), static_cast<int>(output.size())),
                   "Cannot generate pairing randomness");
    return output;
}

Bytes digest(std::span<const unsigned char> data, const EVP_MD* algorithm) {
    Bytes output(static_cast<std::size_t>(EVP_MD_get_size(algorithm)));
    unsigned int outputSize = 0;
    requireOpenSsl(
        EVP_Digest(data.data(), data.size(), output.data(), &outputSize, algorithm, nullptr),
        "Cannot hash pairing data");
    output.resize(outputSize);
    return output;
}

Bytes cipher(std::span<const unsigned char> input, std::span<const unsigned char> key,
             bool encrypt) {
    if (key.size() != 16 || input.empty() || input.size() % 16 != 0) {
        throw std::runtime_error("Pairing cipher input has invalid size.");
    }
    OpenSslPointer<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context{EVP_CIPHER_CTX_new()};
    if (!context) {
        throwOpenSsl("Cannot create pairing cipher");
    }
    requireOpenSsl(EVP_CipherInit_ex(context.get(), EVP_aes_128_ecb(), nullptr, key.data(), nullptr,
                                     encrypt ? 1 : 0),
                   "Cannot initialize pairing cipher");
    requireOpenSsl(EVP_CIPHER_CTX_set_padding(context.get(), 0),
                   "Cannot disable pairing cipher padding");
    Bytes output(input.size() + 16);
    int written = 0;
    int finalWritten = 0;
    requireOpenSsl(EVP_CipherUpdate(context.get(), output.data(), &written, input.data(),
                                    static_cast<int>(input.size())),
                   "Cannot process pairing cipher data");
    requireOpenSsl(EVP_CipherFinal_ex(context.get(), output.data() + written, &finalWritten),
                   "Cannot finish pairing cipher data");
    output.resize(static_cast<std::size_t>(written + finalWritten));
    if (output.size() != input.size()) {
        throw std::runtime_error("Pairing cipher returned unexpected output size.");
    }
    return output;
}

OpenSslPointer<X509, X509_free> parseCertificate(const std::string& pem) {
    OpenSslPointer<BIO, BIO_free> bio{BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()))};
    if (!bio) {
        throwOpenSsl("Cannot create certificate parser");
    }
    OpenSslPointer<X509, X509_free> certificate{
        PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr)};
    if (!certificate) {
        throwOpenSsl("Sol certificate is unreadable");
    }
    return certificate;
}

Bytes certificateSignature(X509* certificate) {
    const ASN1_BIT_STRING* signature = nullptr;
    X509_get0_signature(&signature, nullptr, certificate);
    if (signature == nullptr || ASN1_STRING_length(signature) <= 0) {
        throw std::runtime_error("Certificate signature is missing.");
    }
    const auto* data = ASN1_STRING_get0_data(signature);
    return {data, data + ASN1_STRING_length(signature)};
}

bool verifySignature(std::span<const unsigned char> data, std::span<const unsigned char> signature,
                     X509* certificate) {
    OpenSslPointer<EVP_PKEY, EVP_PKEY_free> publicKey{X509_get_pubkey(certificate)};
    OpenSslPointer<EVP_MD_CTX, EVP_MD_CTX_free> context{EVP_MD_CTX_new()};
    if (!publicKey || !context) {
        throwOpenSsl("Cannot initialize server signature verification");
    }
    requireOpenSsl(
        EVP_DigestVerifyInit(context.get(), nullptr, EVP_sha256(), nullptr, publicKey.get()),
        "Cannot initialize server signature verification");
    requireOpenSsl(EVP_DigestVerifyUpdate(context.get(), data.data(), data.size()),
                   "Cannot hash server signature data");
    const auto result = EVP_DigestVerifyFinal(context.get(), signature.data(), signature.size());
    if (result < 0) {
        throwOpenSsl("Cannot verify server signature");
    }
    return result == 1;
}

Bytes sign(std::span<const unsigned char> data, EVP_PKEY* privateKey) {
    OpenSslPointer<EVP_MD_CTX, EVP_MD_CTX_free> context{EVP_MD_CTX_new()};
    if (!context) {
        throwOpenSsl("Cannot create client signature context");
    }
    requireOpenSsl(EVP_DigestSignInit(context.get(), nullptr, EVP_sha256(), nullptr, privateKey),
                   "Cannot initialize client signature");
    requireOpenSsl(EVP_DigestSignUpdate(context.get(), data.data(), data.size()),
                   "Cannot hash client signature data");
    std::size_t size = 0;
    requireOpenSsl(EVP_DigestSignFinal(context.get(), nullptr, &size),
                   "Cannot size client signature");
    Bytes signature(size);
    requireOpenSsl(EVP_DigestSignFinal(context.get(), signature.data(), &size),
                   "Cannot create client signature");
    signature.resize(size);
    return signature;
}

void requirePaired(const pugi::xml_node& root, const char* stage) {
    if (childText(root, "paired") != "1") {
        throw std::runtime_error(std::string{"Sol rejected pairing "} + stage + ".");
    }
}

int majorVersion(const std::string& value) {
    const auto separator = value.find('.');
    const auto component = value.substr(0, separator);
    if (component.empty() ||
        !std::all_of(component.begin(), component.end(),
                     [](const unsigned char character) { return std::isdigit(character); })) {
        throw std::runtime_error("Sol app version is invalid.");
    }
    return std::stoi(component);
}

void append(Bytes& destination, std::span<const unsigned char> source) {
    destination.insert(destination.end(), source.begin(), source.end());
}
}  // namespace

ApiError::ApiError(int status, std::string code, std::string message, nlohmann::json details) :
    std::runtime_error(std::move(message)),
    status_(status),
    code_(std::move(code)),
    details_(std::move(details)) {}

int ApiError::status() const noexcept { return status_; }

const std::string& ApiError::code() const noexcept { return code_; }

const nlohmann::json& ApiError::details() const noexcept { return details_; }

GameStreamClient::GameStreamClient(const std::filesystem::path& dataPath) :
    identity_(dataPath) {}

std::string GameStreamClient::normalizeAddress(std::string address) {
    return parseEndpoint(std::move(address)).canonicalAddress;
}

std::string GameStreamClient::streamHost(const std::string& address) {
    return parseEndpoint(address).host;
}

ServerInfo GameStreamClient::probe(const std::string& address, std::uint16_t httpsPort,
                                   const std::string& clientId,
                                   const std::string& serverCertificate) const {
    const auto endpoint = parseEndpoint(address);
    const bool useHttps = !serverCertificate.empty();
    const auto response = request(
        endpoint, useHttps ? (httpsPort == 0 ? kDefaultHttpsPort : httpsPort) : endpoint.port,
        useHttps, "serverinfo", {}, clientId, identity_, serverCertificate, 5);
    pugi::xml_document document;
    const auto root = parseRoot(document, response);
    const auto codecModeNode = root.child("ServerCodecModeSupport");
    const auto codecModeText = codecModeNode.text().as_string();
    const auto macAddress = parseMacAddress(childText(root, "mac"));
    std::vector<HostDisplayMode> displayModes;
    collectDisplayModes(root, displayModes);
    std::ranges::sort(displayModes);
    displayModes.erase(std::unique(displayModes.begin(), displayModes.end()), displayModes.end());
    ServerInfo info{
        .serverName = childText(root, "hostname"),
        .serverUniqueId = childText(root, "uniqueid"),
        .appVersion = childText(root, "appversion"),
        .gfeVersion = childText(root, "GfeVersion"),
        .serverState = childText(root, "state"),
        .httpsPort =
            static_cast<std::uint16_t>(root.child("HttpsPort").text().as_uint(kDefaultHttpsPort)),
        .currentGameId = root.child("currentgame").text().as_int(0),
        .serverCodecModeSupport = !codecModeNode || codecModeText[0] == '\0'
                                      ? kBaselineCodecModeSupport
                                      : codecModeNode.text().as_int(0),
        .maxLumaPixelsHevc = root.child("MaxLumaPixelsHEVC").text().as_ullong(0),
        .displayModes = std::move(displayModes),
        .macAddress = macAddress ? formatMacAddress(*macAddress) : std::string{},
        .paired = childText(root, "PairStatus") == "1",
        .eclipseApiVersion = useHttps
                                 ? static_cast<int>(parseUnsignedField(
                                       root, "EclipseApiVersion",
                                       static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
                                 : 0,
        .eclipseApiPort = static_cast<std::uint16_t>(
            useHttps ? parseUnsignedField(root, "EclipseApiPort", 65535) : 0),
        .eclipseCapabilities = useHttps ? parseCsv(childText(root, "EclipseCapabilities"))
                                        : std::vector<std::string>{},
    };
    if (info.serverName.empty() || info.serverUniqueId.empty()) {
        throw std::runtime_error("Endpoint is not a compatible Sol host.");
    }
    return info;
}

std::vector<GameStreamApp> GameStreamClient::apps(const std::string& address,
                                                  std::uint16_t httpsPort,
                                                  const std::string& clientId,
                                                  const std::string& serverCertificate) const {
    requireSecureRequest(serverCertificate);
    const auto endpoint = parseEndpoint(address);
    const auto response =
        request(endpoint, httpsPort == 0 ? kDefaultHttpsPort : httpsPort, true, "applist", {},
                clientId, identity_, serverCertificate, kRequestTimeoutSeconds);
    pugi::xml_document document;
    const auto root = parseRoot(document, response);
    std::vector<GameStreamApp> result;
    for (const auto node : root.children("App")) {
        const auto title = node.child("AppTitle");
        const auto id = node.child("ID").text().as_int(0);
        if (!title || id <= 0) {
            throw std::runtime_error("Sol returned an invalid application list.");
        }
        result.push_back({
            .id = id,
            .name = title.text().as_string(),
            .hdrSupported = childText(node, "IsHdrSupported") == "1",
            .appCollectorGame = childText(node, "IsAppCollectorGame") == "1",
            .uuid = {},
            .kind = "unknown",
            .description = {},
            .source = {},
            .publisher = {},
            .tags = {},
            .inputRequirements = {},
            .launchProfiles = {},
            .installed = true,
            .updateAvailable = false,
            .assetId = {},
            .assetPath = {},
            .assetRevision = 0,
            .displayProfileId = {},
            .streamProfileId = {},
            .sandboxProfileId = {},
        });
    }
    return result;
}

std::string GameStreamClient::boxArt(const std::string& address, std::uint16_t httpsPort,
                                     const std::string& clientId,
                                     const std::string& serverCertificate, int appId) const {
    requireSecureRequest(serverCertificate, appId);
    if (appId == 0) {
        throw std::invalid_argument("Application ID is invalid.");
    }
    const auto endpoint = parseEndpoint(address);
    return request(endpoint, httpsPort == 0 ? kDefaultHttpsPort : httpsPort, true, "appasset",
                   "appid=" + std::to_string(appId) + "&AssetType=2&AssetIdx=0", clientId,
                   identity_, serverCertificate, kRequestTimeoutSeconds);
}

LaunchResult GameStreamClient::launch(const std::string& address, std::uint16_t httpsPort,
                                      const std::string& clientId,
                                      const std::string& serverCertificate, int appId, bool resume,
                                       const StreamSettings& settings,
                                       const std::atomic_bool* cancellation,
                                       const std::string& appUuid,
                                       const std::string& launchProfileId,
                                       const std::string& workspaceId,
                                       const std::string& displayId, const bool primary) const {
    requireSecureRequest(serverCertificate, appId);
    if (appId == 0) {
        throw std::invalid_argument("Application ID is invalid.");
    }

    LaunchResult result;
    result.appId = appId;
    result.resumed = resume;
    const auto key = randomBytes(result.remoteInputKey.size());
    const auto iv = randomBytes(4);
    std::copy(key.begin(), key.end(), result.remoteInputKey.begin());
    std::copy(iv.begin(), iv.end(), result.remoteInputIv.begin());
    const auto keyIdBits =
        (static_cast<std::uint32_t>(iv[0]) << 24U) | (static_cast<std::uint32_t>(iv[1]) << 16U) |
        (static_cast<std::uint32_t>(iv[2]) << 8U) | static_cast<std::uint32_t>(iv[3]);
    const auto keyId = std::bit_cast<std::int32_t>(keyIdBits);

    auto gamepadMask = connectedGamepadMask();
    if (settings.input.forceGamepad && gamepadTransportAvailable()) gamepadMask |= 1;
    if (!primary) gamepadMask = 0;
    const std::string hdrArguments = settings.enableHdr
                                         ? "&hdrMode=1&clientHdrCapVersion=0"
                                           "&clientHdrCapSupportedFlagsInUint32=0"
                                           "&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1"
                                           "&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0"
                                         : "";
    const auto arguments =
        "appid=" + std::to_string(appId) + "&mode=" + std::to_string(settings.width) + "x" +
        std::to_string(settings.height) + "x" + std::to_string(settings.fps) +
        "&additionalStates=1&sops=" + std::to_string(settings.gameOptimizations ? 1 : 0) +
        "&rikey=" + toHex(key) + "&rikeyid=" + std::to_string(keyId) + hdrArguments +
        "&localAudioPlayMode=" + std::to_string(settings.muteHostAudio ? 0 : 1) +
        "&surroundAudioInfo=" + std::to_string(surroundAudioInfo(settings.audioConfig)) +
        "&remoteControllersBitmap=" + std::to_string(gamepadMask) +
        "&gcmap=" + std::to_string(gamepadMask) +
        "&gcpersist=" + std::to_string(settings.input.forceGamepad ? 1 : 0) +
        "&corever=1&eclipseApiVersion=1" +
        (appUuid.empty() ? "" : "&eclipseAppUuid=" + urlEncode(appUuid)) +
        (launchProfileId.empty()
             ? ""
             : "&eclipseLaunchProfileId=" + urlEncode(launchProfileId)) +
        (workspaceId.empty() ? "" : "&eclipseWorkspaceId=" + urlEncode(workspaceId)) +
        (displayId.empty() ? "" : "&eclipseDisplayId=" + urlEncode(displayId));
    const auto endpoint = parseEndpoint(address);
    const auto response =
        request(endpoint, httpsPort == 0 ? kDefaultHttpsPort : httpsPort, true,
                resume ? "resume" : "launch", arguments, clientId, identity_, serverCertificate,
                resume ? 30 : kLaunchTimeoutSeconds, cancellation);
    pugi::xml_document document;
    const auto root = parseRoot(document, response);
    result.sessionUrl = childText(root, "sessionUrl0");
    result.logicalSessionId = childText(root, "EclipseSessionId");
    result.childStreamId = childText(root, "EclipseStreamId");
    if (result.sessionUrl.empty()) {
        throw std::runtime_error("Sol launch response is missing session URL.");
    }
    return result;
}

ApiResponse GameStreamClient::apiRequest(const std::string& address, std::uint16_t apiPort,
                                         const std::string& serverCertificate,
                                         const std::string& method, const std::string& path,
                                         const std::optional<nlohmann::json>& body,
                                         const std::map<std::string, std::string>& headers) const {
    const auto response =
        apiCall(parseEndpoint(address), apiPort, serverCertificate, identity_, method, path,
                body ? std::optional<std::string>{body->dump()} : std::nullopt, headers);
    nlohmann::json parsed;
    if (!response.body.empty()) {
        try {
            parsed = nlohmann::json::parse(response.body);
        } catch (const std::exception&) {
            throw std::runtime_error("Sol API returned malformed JSON.");
        }
        if (!parsed.is_object() || parsed.value("schemaVersion", 0) != 1) {
            throw std::runtime_error("Sol API returned unsupported schemaVersion.");
        }
    }
    return {response.status, response.headers, std::move(parsed)};
}

std::string GameStreamClient::apiBytes(const std::string& address, std::uint16_t apiPort,
                                       const std::string& serverCertificate,
                                       const std::string& path,
                                       const std::map<std::string, std::string>& headers) const {
    auto assetHeaders = headers;
    assetHeaders.try_emplace("Accept", "*/*");
    return apiCall(parseEndpoint(address), apiPort, serverCertificate, identity_, "GET", path,
                   std::nullopt, assetHeaders)
        .body;
}

void GameStreamClient::streamEvents(const std::string& address, std::uint16_t apiPort,
                                    const std::string& serverCertificate, const std::string& path,
                                    const std::string& lastEventId,
                                    const std::atomic_bool& cancellation,
                                    const std::function<void(const std::string&)>& onChunk) const {
    validateApiPath(path);
    if (!onChunk) throw std::invalid_argument("SSE chunk callback is required.");
    if (lastEventId.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("SSE Last-Event-ID contains invalid characters.");
    }

    ix::HttpClient client;
    configureTls(client, identity_, serverCertificate);
    const auto arguments = client.createRequest();
    arguments->connectTimeout = 5;
    arguments->transferTimeout = 24 * 60 * 60;
    arguments->followRedirects = false;
    arguments->compress = false;
    arguments->extraHeaders["Accept"] = "text/event-stream";
    arguments->extraHeaders["Connection"] = "keep-alive";
    if (!lastEventId.empty()) arguments->extraHeaders["Last-Event-ID"] = lastEventId;

    std::exception_ptr callbackFailure;
    std::string errorBody;
    arguments->onChunkCallback = [&](const std::string& chunk) {
        if (errorBody.size() < kMaximumApiResponseBytes) {
            errorBody.append(chunk, 0,
                             std::min(chunk.size(), kMaximumApiResponseBytes - errorBody.size()));
        }
        try {
            onChunk(chunk);
        } catch (...) {
            callbackFailure = std::current_exception();
            arguments->cancel.store(true);
        }
    };
    auto cancellationMonitor = monitorCancellation(arguments, &cancellation);
    const auto endpoint = parseEndpoint(address);
    const auto response =
        client.request("https://" + urlHost(endpoint) + ":" +
                           std::to_string(apiPort == 0 ? kDefaultHttpsPort : apiPort) + path,
                       "GET", "", arguments);
    if (cancellationMonitor.joinable()) cancellationMonitor.request_stop();
    if (callbackFailure) std::rethrow_exception(callbackFailure);
    if (cancellation.load()) return;
    if (!response || response->errorCode != ix::HttpErrorCode::Ok) {
        throw std::runtime_error(response ? response->errorMsg : "No HTTP response.");
    }
    if (response->statusCode < 200 || response->statusCode >= 300) {
        throwApiError(response->statusCode, errorBody);
    }
}

void GameStreamClient::cancel(const std::string& address, std::uint16_t httpsPort,
                              const std::string& clientId,
                              const std::string& serverCertificate) const {
    requireSecureRequest(serverCertificate);
    const auto endpoint = parseEndpoint(address);
    const auto response = request(endpoint, httpsPort == 0 ? kDefaultHttpsPort : httpsPort, true,
                                  "cancel", {}, clientId, identity_, serverCertificate, 30);
    pugi::xml_document document;
    static_cast<void>(parseRoot(document, response));
    if (probe(address, httpsPort, clientId, serverCertificate).currentGameId != 0) {
        throw std::runtime_error("The running application was not started by this "
                                 "client and could not be stopped.");
    }
}

std::string GameStreamClient::pair(const std::string& address, std::uint16_t httpsPort,
                                   const std::string& appVersion, const std::string& clientId,
                                   const std::string& pin, PairingAccess access) const {
    if (pin.size() != 4 || !std::all_of(pin.begin(), pin.end(), [](const unsigned char character) {
            return std::isdigit(character);
        })) {
        throw std::invalid_argument("Pairing PIN must contain four digits.");
    }
    const auto endpoint = parseEndpoint(address);
    const auto securePort = httpsPort == 0 ? kDefaultHttpsPort : httpsPort;
    const EVP_MD* hashAlgorithm = majorVersion(appVersion) >= 7 ? EVP_sha256() : EVP_sha1();
    const auto hashLength = static_cast<std::size_t>(EVP_MD_get_size(hashAlgorithm));
    const auto salt = randomBytes(16);
    Bytes saltedPin = salt;
    append(saltedPin, std::span{reinterpret_cast<const unsigned char*>(pin.data()), pin.size()});
    auto aesKey = digest(saltedPin, hashAlgorithm);
    aesKey.resize(16);

    const auto bestEffortUnpair = [&] {
        try {
            static_cast<void>(
                request(endpoint, endpoint.port, false, "unpair", {}, clientId, identity_, {}, 5));
        } catch (...) {
        }
    };

    try {
        constexpr std::string_view gamingScopes =
            "catalog.read,stream.launch,session.control,telemetry.read";
        constexpr std::string_view workstationScopes =
            "catalog.read,stream.launch,session.control,telemetry.read,display."
            "read,display.manage,"
            "virtual-display.manage,peripheral.forward,sandbox.manage,host.control";
        constexpr std::string_view inputs = "keyboard,mouse,controller,touch,pen";
        const auto scopes = access == PairingAccess::workstation ? workstationScopes : gamingScopes;
        pugi::xml_document certificateDocument;
        const auto certificateResponse =
            request(endpoint, endpoint.port, false, "pair",
                    "devicename=roth&updateState=1&phrase=getservercert&salt=" + toHex(salt) +
                        "&clientcert=" + toHex(identity_.certificatePem()) + "&eclipsePlatform=" +
                        urlEncode("Terra") + "&eclipseScopes=" + urlEncode(std::string{scopes}) +
                        "&eclipseInput=" + urlEncode(std::string{inputs}),
                    clientId, identity_, {}, kPairingPinTimeoutSeconds);
        const auto certificateRoot = parseRoot(certificateDocument, certificateResponse);
        requirePaired(certificateRoot, "certificate stage");
        const auto certificateBytes = fromHex(childText(certificateRoot, "plaincert"));
        if (certificateBytes.empty()) {
            throw std::runtime_error("Sol is already handling another pairing request.");
        }
        const std::string serverCertificate{certificateBytes.begin(), certificateBytes.end()};
        auto serverX509 = parseCertificate(serverCertificate);
        auto clientX509 = parseCertificate(identity_.certificatePem());

        const auto randomChallenge = randomBytes(16);
        const auto encryptedChallenge = cipher(randomChallenge, aesKey, true);
        pugi::xml_document challengeDocument;
        const auto challengeResponse =
            request(endpoint, endpoint.port, false, "pair",
                    "devicename=roth&updateState=1&clientchallenge=" + toHex(encryptedChallenge),
                    clientId, identity_, {}, 5);
        const auto challengeRoot = parseRoot(challengeDocument, challengeResponse);
        requirePaired(challengeRoot, "challenge stage");
        const auto challengeData =
            cipher(fromHex(childText(challengeRoot, "challengeresponse")), aesKey, false);
        if (challengeData.size() < hashLength + 16) {
            throw std::runtime_error("Sol challenge response is too short.");
        }

        const auto clientSecret = randomBytes(16);
        const Bytes serverResponse{challengeData.begin(), challengeData.begin() + hashLength};
        Bytes challengeHashInput{challengeData.begin() + hashLength,
                                 challengeData.begin() + hashLength + 16};
        append(challengeHashInput, certificateSignature(clientX509.get()));
        append(challengeHashInput, clientSecret);
        auto paddedHash = digest(challengeHashInput, hashAlgorithm);
        paddedHash.resize(32);
        const auto encryptedResponse = cipher(paddedHash, aesKey, true);

        pugi::xml_document responseDocument;
        const auto responseXml =
            request(endpoint, endpoint.port, false, "pair",
                    "devicename=roth&updateState=1&serverchallengeresp=" + toHex(encryptedResponse),
                    clientId, identity_, {}, 5);
        const auto responseRoot = parseRoot(responseDocument, responseXml);
        requirePaired(responseRoot, "verification stage");
        const auto pairingSecret = fromHex(childText(responseRoot, "pairingsecret"));
        if (pairingSecret.size() <= 16) {
            throw std::runtime_error("Sol pairing secret is invalid.");
        }
        const Bytes serverSecret{pairingSecret.begin(), pairingSecret.begin() + 16};
        const Bytes serverSignature{pairingSecret.begin() + 16, pairingSecret.end()};
        if (!verifySignature(serverSecret, serverSignature, serverX509.get())) {
            throw std::runtime_error(
                "Server identity signature failed. Possible interception detected.");
        }

        Bytes expectedResponse = randomChallenge;
        append(expectedResponse, certificateSignature(serverX509.get()));
        append(expectedResponse, serverSecret);
        const auto expectedHash = digest(expectedResponse, hashAlgorithm);
        if (expectedHash.size() != serverResponse.size() ||
            CRYPTO_memcmp(expectedHash.data(), serverResponse.data(), expectedHash.size()) != 0) {
            throw std::runtime_error("Incorrect PIN.");
        }

        Bytes clientPairingSecret = clientSecret;
        append(clientPairingSecret, sign(clientSecret, identity_.privateKey()));
        pugi::xml_document secretDocument;
        const auto secretXml = request(endpoint, endpoint.port, false, "pair",
                                       "devicename=roth&updateState=1&clientpairingsecret=" +
                                           toHex(clientPairingSecret),
                                       clientId, identity_, {}, 5);
        requirePaired(parseRoot(secretDocument, secretXml), "client secret stage");

        pugi::xml_document finalDocument;
        const auto finalXml = request(endpoint, securePort, true, "pair",
                                      "devicename=roth&updateState=1&phrase=pairchallenge",
                                      clientId, identity_, serverCertificate, 5);
        requirePaired(parseRoot(finalDocument, finalXml), "mutual TLS stage");
        return serverCertificate;
    } catch (...) {
        bestEffortUnpair();
        throw;
    }
}

}  // namespace terra
