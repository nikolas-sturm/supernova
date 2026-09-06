#include "mdns_discovery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace eclipse {
namespace {

constexpr std::string_view kServiceType = "_nvstream._tcp.local";
constexpr std::uint16_t kMdnsPort = 5353;
constexpr std::uint16_t kTypeA = 1;
constexpr std::uint16_t kTypePtr = 12;
constexpr std::uint16_t kTypeAaaa = 28;
constexpr std::uint16_t kTypeSrv = 33;

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void closeSocket(NativeSocket socket) { closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void closeSocket(NativeSocket socket) { close(socket); }
#endif

std::uint16_t readU16(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset + 2 > data.size()) throw std::runtime_error("Truncated mDNS packet.");
    return static_cast<std::uint16_t>((data[offset] << 8U) | data[offset + 1]);
}

std::string normalizeName(std::string value) {
    if (!value.empty() && value.back() == '.') value.pop_back();
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::string readName(std::span<const std::uint8_t> data, std::size_t& cursor,
                     std::size_t inlineEnd = 0) {
    if (inlineEnd == 0) inlineEnd = data.size();
    std::string result;
    std::size_t position = cursor;
    std::size_t resume = cursor;
    bool jumped = false;
    std::size_t steps = 0;
    while (position < data.size() && ++steps <= data.size()) {
        if (!jumped && position >= inlineEnd) throw std::runtime_error("Truncated mDNS name.");
        const auto length = data[position++];
        if (length == 0) {
            cursor = jumped ? resume : position;
            return result;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (position >= data.size() || (!jumped && position >= inlineEnd)) {
                throw std::runtime_error("Truncated mDNS name pointer.");
            }
            const auto pointer = static_cast<std::size_t>(((length & 0x3FU) << 8U) | data[position++]);
            if (pointer >= data.size()) throw std::runtime_error("Invalid mDNS name pointer.");
            if (!jumped) {
                resume = position;
                jumped = true;
            }
            position = pointer;
            continue;
        }
        if ((length & 0xC0U) != 0 || length > 63 || position + length > data.size() ||
            (!jumped && position + length > inlineEnd) || result.size() + length + 1 > 255) {
            throw std::runtime_error("Invalid mDNS label.");
        }
        if (!result.empty()) result.push_back('.');
        result.append(reinterpret_cast<const char*>(data.data() + position), length);
        position += length;
    }
    throw std::runtime_error("Cyclic mDNS name.");
}

struct ResourceRecord {
    std::string name;
    std::uint16_t type = 0;
    std::uint16_t recordClass = 0;
    std::uint16_t port = 0;
    std::string target;
    std::string address;
};

std::vector<ResourceRecord> parseRecords(std::span<const std::uint8_t> packet) {
    if (packet.size() < 12) throw std::runtime_error("Truncated mDNS header.");
    const auto flags = readU16(packet, 2);
    if ((flags & 0x8000U) == 0 || (flags & 0x780FU) != 0) {
        throw std::runtime_error("Packet is not a successful mDNS response.");
    }
    const auto questionCount = readU16(packet, 4);
    const auto recordCount = static_cast<std::uint32_t>(readU16(packet, 6)) + readU16(packet, 8) +
                             readU16(packet, 10);
    if (questionCount > 256 || recordCount > 1024) throw std::runtime_error("Oversized mDNS packet.");

    std::size_t cursor = 12;
    for (std::uint16_t index = 0; index < questionCount; ++index) {
        static_cast<void>(readName(packet, cursor));
        if (cursor + 4 > packet.size()) throw std::runtime_error("Truncated mDNS question.");
        cursor += 4;
    }

    std::vector<ResourceRecord> records;
    records.reserve(recordCount);
    for (std::uint32_t index = 0; index < recordCount; ++index) {
        ResourceRecord record;
        record.name = normalizeName(readName(packet, cursor));
        if (cursor + 10 > packet.size()) throw std::runtime_error("Truncated mDNS record.");
        record.type = readU16(packet, cursor);
        record.recordClass = readU16(packet, cursor + 2) & 0x7FFFU;
        const auto dataLength = readU16(packet, cursor + 8);
        cursor += 10;
        const auto dataOffset = cursor;
        if (cursor + dataLength > packet.size()) throw std::runtime_error("Truncated mDNS data.");

        if (record.type == kTypePtr) {
            auto nameCursor = dataOffset;
            record.target = normalizeName(readName(packet, nameCursor, dataOffset + dataLength));
            if (nameCursor != dataOffset + dataLength) {
                throw std::runtime_error("Invalid mDNS PTR data length.");
            }
        } else if (record.type == kTypeSrv && dataLength >= 6) {
            record.port = readU16(packet, dataOffset + 4);
            auto nameCursor = dataOffset + 6;
            record.target = normalizeName(readName(packet, nameCursor, dataOffset + dataLength));
            if (nameCursor != dataOffset + dataLength) {
                throw std::runtime_error("Invalid mDNS SRV data length.");
            }
        } else if (record.type == kTypeA && dataLength == 4) {
            char buffer[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, packet.data() + dataOffset, buffer, sizeof(buffer))) {
                record.address = buffer;
            }
        } else if (record.type == kTypeAaaa && dataLength == 16) {
            char buffer[INET6_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET6, packet.data() + dataOffset, buffer, sizeof(buffer))) {
                record.address = buffer;
            }
        }
        cursor += dataLength;
        records.push_back(std::move(record));
    }
    return records;
}

struct ServiceTarget {
    std::string hostname;
    std::uint16_t port = 0;
};

struct ServiceAddress {
    std::string address;
    unsigned int scopeId = 0;

    bool operator==(const ServiceAddress&) const = default;
};

bool isIpv6LinkLocal(std::string_view address) {
    in6_addr parsed{};
    return inet_pton(AF_INET6, std::string{address}.c_str(), &parsed) == 1 &&
           IN6_IS_ADDR_LINKLOCAL(&parsed);
}

void ingestRecords(const std::vector<ResourceRecord>& records,
                    std::unordered_set<std::string>& instances,
                    std::unordered_map<std::string, ServiceTarget>& targets,
                    std::unordered_map<std::string, std::vector<ServiceAddress>>& addresses,
                    unsigned int ipv6ScopeId = 0) {
    for (const auto& record : records) {
        if (record.recordClass != 1) continue;
        if (record.type == kTypePtr && record.name == kServiceType && !record.target.empty()) {
            if (instances.contains(record.target) || instances.size() < 64) {
                instances.insert(record.target);
            }
        } else if (record.type == kTypeSrv && !record.target.empty() && record.port != 0 &&
                   record.name.ends_with(std::string{"."} + std::string{kServiceType})) {
            if ((instances.contains(record.name) || instances.size() < 64) &&
                (targets.contains(record.name) || targets.size() < 64)) {
                instances.insert(record.name);
                targets[record.name] = {record.target, record.port};
            }
        } else if ((record.type == kTypeA || record.type == kTypeAaaa) &&
                    !record.address.empty()) {
            if (record.type == kTypeAaaa && isIpv6LinkLocal(record.address) && ipv6ScopeId == 0) {
                continue;
            }
            if (!addresses.contains(record.name) && addresses.size() >= 128) continue;
            auto& values = addresses[record.name];
            const ServiceAddress address{
                record.address,
                record.type == kTypeAaaa && isIpv6LinkLocal(record.address) ? ipv6ScopeId : 0,
            };
            if (values.size() < 4 && std::find(values.begin(), values.end(), address) == values.end()) {
                values.push_back(address);
            }
        }
    }
}

std::vector<MdnsService> resolvedServices(
    const std::unordered_set<std::string>& instances,
    const std::unordered_map<std::string, ServiceTarget>& targets,
    const std::unordered_map<std::string, std::vector<ServiceAddress>>& addresses,
    int preferredFamily = AF_INET) {
    std::vector<MdnsService> result;
    for (const auto& instance : instances) {
        const auto target = targets.find(instance);
        if (target == targets.end()) continue;
        const auto hostAddresses = addresses.find(target->second.hostname);
        if (hostAddresses == addresses.end() || hostAddresses->second.empty()) continue;
        const auto preferred = std::find_if(
            hostAddresses->second.begin(), hostAddresses->second.end(), [&](const auto& value) {
                const bool ipv6 = value.address.find(':') != std::string::npos;
                return preferredFamily == AF_INET6 ? ipv6 : !ipv6;
            });
        const auto& selected = preferred == hostAddresses->second.end()
                                   ? hostAddresses->second.front()
                                   : *preferred;
        auto address = selected.address;
        if (selected.scopeId != 0) address += "%" + std::to_string(selected.scopeId);
        auto displayName = instance;
        const auto suffix = std::string{"."} + std::string{kServiceType};
        if (displayName.ends_with(suffix)) displayName.resize(displayName.size() - suffix.size());
        result.push_back({std::move(displayName), target->second.hostname, std::move(address),
                          target->second.port});
    }
    return result;
}

std::vector<std::uint8_t> makeQuery(std::string_view name, std::uint16_t type) {
    std::vector<std::uint8_t> result(12, 0);
    result[5] = 1;
    std::size_t start = 0;
    while (start < name.size()) {
        const auto end = name.find('.', start);
        const auto length = (end == std::string_view::npos ? name.size() : end) - start;
        if (length == 0 || length > 63) throw std::runtime_error("Invalid mDNS query name.");
        result.push_back(static_cast<std::uint8_t>(length));
        result.insert(result.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                      name.begin() + static_cast<std::ptrdiff_t>(start + length));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    result.push_back(0);
    result.push_back(static_cast<std::uint8_t>(type >> 8U));
    result.push_back(static_cast<std::uint8_t>(type));
    result.push_back(0);
    result.push_back(1);
    return result;
}

std::vector<in_addr> activeIpv4Interfaces() {
    std::vector<in_addr> result;
#if defined(_WIN32)
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
    std::vector<std::uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (size != 0 && GetAdaptersAddresses(AF_INET,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, adapters, &size) == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next) {
                if (!address->Address.lpSockaddr ||
                    address->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                result.push_back(
                    reinterpret_cast<const sockaddr_in*>(address->Address.lpSockaddr)->sin_addr);
            }
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
            if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET ||
                (interface->ifa_flags & IFF_UP) == 0 ||
                (interface->ifa_flags & IFF_LOOPBACK) != 0 ||
                (interface->ifa_flags & IFF_MULTICAST) == 0) {
                continue;
            }
            result.push_back(reinterpret_cast<const sockaddr_in*>(interface->ifa_addr)->sin_addr);
        }
        freeifaddrs(interfaces);
    }
#endif
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.s_addr < right.s_addr;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
                     return left.s_addr == right.s_addr;
                 }),
                 result.end());
    return result;
}

std::vector<unsigned int> activeIpv6Interfaces() {
    std::vector<unsigned int> result;
#if defined(_WIN32)
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET6, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                       GAA_FLAG_SKIP_DNS_SERVER,
                         nullptr, nullptr, &size);
    std::vector<std::uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (size != 0 && GetAdaptersAddresses(AF_INET6,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                              GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, adapters, &size) == NO_ERROR) {
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK ||
                (adapter->Flags & IP_ADAPTER_NO_MULTICAST) != 0 || adapter->Ipv6IfIndex == 0) {
                continue;
            }
            result.push_back(adapter->Ipv6IfIndex);
        }
    }
#else
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
            if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET6 ||
                (interface->ifa_flags & IFF_UP) == 0 ||
                (interface->ifa_flags & IFF_LOOPBACK) != 0 ||
                (interface->ifa_flags & IFF_MULTICAST) == 0) {
                continue;
            }
            const auto index = if_nametoindex(interface->ifa_name);
            if (index != 0) result.push_back(index);
        }
        freeifaddrs(interfaces);
    }
#endif
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

bool hasAddressFamily(const std::vector<ServiceAddress>& addresses, int family) {
    return std::ranges::any_of(addresses, [family](const auto& address) {
        const bool ipv6 = address.address.find(':') != std::string::npos;
        return family == AF_INET6 ? ipv6 : !ipv6;
    });
}

}  // namespace

std::vector<MdnsService> parseMdnsResponse(std::span<const std::uint8_t> packet,
                                           unsigned int ipv6ScopeId) {
    std::unordered_set<std::string> instances;
    std::unordered_map<std::string, ServiceTarget> targets;
    std::unordered_map<std::string, std::vector<ServiceAddress>> addresses;
    ingestRecords(parseRecords(packet), instances, targets, addresses, ipv6ScopeId);
    return resolvedServices(instances, targets, addresses);
}

class MdnsDiscovery::Impl {
public:
    explicit Impl(Listener listener) : listener_(std::move(listener)) {}
    ~Impl() { stop(); }

    void start() {
        std::scoped_lock lock{mutex_};
        if (thread_.joinable()) return;
        instances_.clear();
        targets_.clear();
        addresses_.clear();
        reported_.clear();
        thread_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
    }

    void stop() {
        std::scoped_lock lock{mutex_};
        if (!thread_.joinable()) return;
        thread_.request_stop();
        thread_.join();
    }

private:
    void sendIpv4Query(NativeSocket socketHandle, const std::vector<in_addr>& interfaces,
                       std::string_view name, std::uint16_t type) {
        const auto query = makeQuery(name, type);
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kMdnsPort);
        inet_pton(AF_INET, "224.0.0.251", &destination.sin_addr);
        for (const auto& interfaceAddress : interfaces) {
            setsockopt(socketHandle, IPPROTO_IP, IP_MULTICAST_IF,
                       reinterpret_cast<const char*>(&interfaceAddress), sizeof(interfaceAddress));
#if defined(_WIN32)
            sendto(socketHandle, reinterpret_cast<const char*>(query.data()),
                   static_cast<int>(query.size()), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#else
            sendto(socketHandle, query.data(), query.size(), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#endif
        }
    }

    void sendIpv6Query(NativeSocket socketHandle, const std::vector<unsigned int>& interfaces,
                       std::string_view name, std::uint16_t type) {
        const auto query = makeQuery(name, type);
        sockaddr_in6 destination{};
        destination.sin6_family = AF_INET6;
        destination.sin6_port = htons(kMdnsPort);
        inet_pton(AF_INET6, "ff02::fb", &destination.sin6_addr);
        for (const auto interfaceIndex : interfaces) {
            setsockopt(socketHandle, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                       reinterpret_cast<const char*>(&interfaceIndex), sizeof(interfaceIndex));
            destination.sin6_scope_id = interfaceIndex;
#if defined(_WIN32)
            sendto(socketHandle, reinterpret_cast<const char*>(query.data()),
                   static_cast<int>(query.size()), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#else
            sendto(socketHandle, query.data(), query.size(), 0,
                   reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
#endif
        }
    }

    void sendQuery(NativeSocket ipv4Socket, const std::vector<in_addr>& ipv4Interfaces,
                   NativeSocket ipv6Socket, const std::vector<unsigned int>& ipv6Interfaces,
                   std::string_view name, std::uint16_t type) {
        if (ipv4Socket != kInvalidSocket) sendIpv4Query(ipv4Socket, ipv4Interfaces, name, type);
        if (ipv6Socket != kInvalidSocket) sendIpv6Query(ipv6Socket, ipv6Interfaces, name, type);
    }

    void processDatagram(NativeSocket socketHandle, int family,
                         const std::vector<in_addr>& ipv4Interfaces,
                         const std::vector<unsigned int>& ipv6Interfaces,
                         NativeSocket ipv4Socket, NativeSocket ipv6Socket) {
        std::array<std::uint8_t, 9000> buffer{};
        sockaddr_storage source{};
        unsigned int scopeId = 0;
        std::ptrdiff_t received = -1;
#if defined(_WIN32)
        if (family == AF_INET6) {
            GUID extensionId = WSAID_WSARECVMSG;
            LPFN_WSARECVMSG receiveMessage = nullptr;
            DWORD transferred = 0;
            if (WSAIoctl(socketHandle, SIO_GET_EXTENSION_FUNCTION_POINTER, &extensionId,
                         sizeof(extensionId), &receiveMessage, sizeof(receiveMessage),
                         &transferred, nullptr, nullptr) == 0 && receiveMessage) {
                WSABUF data{static_cast<ULONG>(buffer.size()),
                            reinterpret_cast<char*>(buffer.data())};
                std::array<char, WSA_CMSG_SPACE(sizeof(IN6_PKTINFO))> control{};
                WSAMSG message{};
                message.name = reinterpret_cast<sockaddr*>(&source);
                message.namelen = sizeof(source);
                message.lpBuffers = &data;
                message.dwBufferCount = 1;
                message.Control = {static_cast<ULONG>(control.size()), control.data()};
                if (receiveMessage(socketHandle, &message, &transferred, nullptr, nullptr) == 0) {
                    received = static_cast<std::ptrdiff_t>(transferred);
                    for (auto* header = WSA_CMSG_FIRSTHDR(&message); header;
                         header = WSA_CMSG_NXTHDR(&message, header)) {
                        if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO) {
                            scopeId = reinterpret_cast<const IN6_PKTINFO*>(
                                          WSA_CMSG_DATA(header))->ipi6_ifindex;
                        }
                    }
                }
            }
        }
        if (received < 0) {
            int sourceLength = sizeof(source);
            received = recvfrom(socketHandle, reinterpret_cast<char*>(buffer.data()),
                                static_cast<int>(buffer.size()), 0,
                                reinterpret_cast<sockaddr*>(&source), &sourceLength);
        }
#else
        if (family == AF_INET6) {
            iovec data{buffer.data(), buffer.size()};
            std::array<char, CMSG_SPACE(sizeof(in6_pktinfo))> control{};
            msghdr message{};
            message.msg_name = &source;
            message.msg_namelen = sizeof(source);
            message.msg_iov = &data;
            message.msg_iovlen = 1;
            message.msg_control = control.data();
            message.msg_controllen = control.size();
            received = recvmsg(socketHandle, &message, 0);
            for (auto* header = CMSG_FIRSTHDR(&message); header;
                 header = CMSG_NXTHDR(&message, header)) {
                if (header->cmsg_level == IPPROTO_IPV6 && header->cmsg_type == IPV6_PKTINFO) {
                    scopeId = reinterpret_cast<const in6_pktinfo*>(CMSG_DATA(header))->ipi6_ifindex;
                }
            }
        } else {
            socklen_t sourceLength = sizeof(source);
            received = recvfrom(socketHandle, buffer.data(), buffer.size(), 0,
                                reinterpret_cast<sockaddr*>(&source), &sourceLength);
        }
#endif
        if (received <= 0 || source.ss_family != family) return;
        if (family == AF_INET) {
            const auto address = ntohl(reinterpret_cast<const sockaddr_in*>(&source)->sin_addr.s_addr);
            if ((address & 0xff000000U) == 0x7f000000U) return;
        } else {
            const auto* address = reinterpret_cast<const sockaddr_in6*>(&source);
            if (IN6_IS_ADDR_LOOPBACK(&address->sin6_addr)) return;
            if (scopeId == 0) scopeId = address->sin6_scope_id;
        }

        const auto records = parseRecords(
            std::span<const std::uint8_t>{buffer.data(), static_cast<std::size_t>(received)});
        ingestRecords(records, instances_, targets_, addresses_, scopeId);
        for (const auto& instance : instances_) {
            if (!targets_.contains(instance)) {
                sendQuery(ipv4Socket, ipv4Interfaces, ipv6Socket, ipv6Interfaces, instance,
                          kTypeSrv);
            }
        }
        for (const auto& [instance, target] : targets_) {
            static_cast<void>(instance);
            const auto addresses = addresses_.find(target.hostname);
            if (addresses == addresses_.end() ||
                !hasAddressFamily(addresses->second, AF_INET)) {
                if (ipv4Socket != kInvalidSocket) {
                    sendIpv4Query(ipv4Socket, ipv4Interfaces, target.hostname, kTypeA);
                }
            }
            if (addresses == addresses_.end() ||
                !hasAddressFamily(addresses->second, AF_INET6)) {
                if (ipv6Socket != kInvalidSocket) {
                    sendIpv6Query(ipv6Socket, ipv6Interfaces, target.hostname, kTypeAaaa);
                }
            }
        }
        for (auto service : resolvedServices(instances_, targets_, addresses_, family)) {
            if (isIpv6LinkLocal(service.address) && service.address.find('%') == std::string::npos) {
                continue;
            }
            const auto key = "[" + service.address + "]:" + std::to_string(service.port);
            if (reported_.insert(key).second && listener_) listener_(service);
        }
    }

    void run(std::stop_token stopToken) noexcept {
#if defined(_WIN32)
        WSADATA socketData{};
        if (WSAStartup(MAKEWORD(2, 2), &socketData) != 0) return;
#endif
        const int enabled = 1;
        auto ipv4Interfaces = activeIpv4Interfaces();
        if (ipv4Interfaces.empty()) {
            in_addr anyInterface{};
            anyInterface.s_addr = htonl(INADDR_ANY);
            ipv4Interfaces.push_back(anyInterface);
        }
        const auto ipv6Interfaces = activeIpv6Interfaces();

        auto ipv4Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (ipv4Socket != kInvalidSocket) {
            setsockopt(ipv4Socket, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(kMdnsPort);
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            const bool bound = bind(ipv4Socket, reinterpret_cast<const sockaddr*>(&local),
                                    sizeof(local)) == 0;
            bool joined = false;
            for (const auto& interfaceAddress : ipv4Interfaces) {
                if (!bound) break;
                ip_mreq membership{};
                inet_pton(AF_INET, "224.0.0.251", &membership.imr_multiaddr);
                membership.imr_interface = interfaceAddress;
                joined = setsockopt(ipv4Socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                    reinterpret_cast<const char*>(&membership),
                                    sizeof(membership)) == 0 ||
                         joined;
            }
            if (!bound || !joined) {
                closeSocket(ipv4Socket);
                ipv4Socket = kInvalidSocket;
            }
        }

        auto ipv6Socket = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (ipv6Socket != kInvalidSocket && !ipv6Interfaces.empty()) {
            setsockopt(ipv6Socket, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
            setsockopt(ipv6Socket, IPPROTO_IPV6, IPV6_V6ONLY,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#if defined(_WIN32)
            setsockopt(ipv6Socket, IPPROTO_IPV6, IPV6_PKTINFO,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#else
            setsockopt(ipv6Socket, IPPROTO_IPV6, IPV6_RECVPKTINFO,
                       reinterpret_cast<const char*>(&enabled), sizeof(enabled));
#endif
            sockaddr_in6 local{};
            local.sin6_family = AF_INET6;
            local.sin6_port = htons(kMdnsPort);
            local.sin6_addr = in6addr_any;
            const bool bound = bind(ipv6Socket, reinterpret_cast<const sockaddr*>(&local),
                                    sizeof(local)) == 0;
            bool joined = false;
            for (const auto interfaceIndex : ipv6Interfaces) {
                if (!bound) break;
                ipv6_mreq membership{};
                inet_pton(AF_INET6, "ff02::fb", &membership.ipv6mr_multiaddr);
                membership.ipv6mr_interface = interfaceIndex;
                joined = setsockopt(ipv6Socket, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                                    reinterpret_cast<const char*>(&membership),
                                    sizeof(membership)) == 0 ||
                         joined;
            }
            if (!bound || !joined) {
                closeSocket(ipv6Socket);
                ipv6Socket = kInvalidSocket;
            }
        } else if (ipv6Socket != kInvalidSocket) {
            closeSocket(ipv6Socket);
            ipv6Socket = kInvalidSocket;
        }

        if (ipv4Socket == kInvalidSocket && ipv6Socket == kInvalidSocket) {
#if defined(_WIN32)
            WSACleanup();
#endif
            return;
        }

        auto nextQuery = std::chrono::steady_clock::time_point{};
        auto nextCacheReset = std::chrono::steady_clock::now() + std::chrono::minutes{5};
        while (!stopToken.stop_requested()) {
            if (std::chrono::steady_clock::now() >= nextQuery) {
                sendQuery(ipv4Socket, ipv4Interfaces, ipv6Socket, ipv6Interfaces, kServiceType,
                          kTypePtr);
                nextQuery = std::chrono::steady_clock::now() + std::chrono::seconds{10};
            }
            if (std::chrono::steady_clock::now() >= nextCacheReset) {
                instances_.clear();
                targets_.clear();
                addresses_.clear();
                reported_.clear();
                nextCacheReset = std::chrono::steady_clock::now() + std::chrono::minutes{5};
            }
            fd_set readable;
            FD_ZERO(&readable);
            int descriptorCount = 0;
            if (ipv4Socket != kInvalidSocket) {
                FD_SET(ipv4Socket, &readable);
                descriptorCount = std::max(descriptorCount, static_cast<int>(ipv4Socket) + 1);
            }
            if (ipv6Socket != kInvalidSocket) {
                FD_SET(ipv6Socket, &readable);
                descriptorCount = std::max(descriptorCount, static_cast<int>(ipv6Socket) + 1);
            }
            timeval timeout{0, 500'000};
            if (select(descriptorCount, &readable, nullptr, nullptr, &timeout) <= 0) continue;
            try {
                if (ipv4Socket != kInvalidSocket && FD_ISSET(ipv4Socket, &readable)) {
                    processDatagram(ipv4Socket, AF_INET, ipv4Interfaces, ipv6Interfaces,
                                    ipv4Socket, ipv6Socket);
                }
                if (ipv6Socket != kInvalidSocket && FD_ISSET(ipv6Socket, &readable)) {
                    processDatagram(ipv6Socket, AF_INET6, ipv4Interfaces, ipv6Interfaces,
                                    ipv4Socket, ipv6Socket);
                }
            } catch (const std::exception&) {
            }
        }
        if (ipv4Socket != kInvalidSocket) closeSocket(ipv4Socket);
        if (ipv6Socket != kInvalidSocket) closeSocket(ipv6Socket);
#if defined(_WIN32)
        WSACleanup();
#endif
    }

    Listener listener_;
    std::mutex mutex_;
    std::jthread thread_;
    std::unordered_set<std::string> instances_;
    std::unordered_map<std::string, ServiceTarget> targets_;
    std::unordered_map<std::string, std::vector<ServiceAddress>> addresses_;
    std::unordered_set<std::string> reported_;
};

MdnsDiscovery::MdnsDiscovery(Listener listener) : impl_(std::make_unique<Impl>(std::move(listener))) {}
MdnsDiscovery::~MdnsDiscovery() = default;
void MdnsDiscovery::start() { impl_->start(); }
void MdnsDiscovery::stop() { impl_->stop(); }

}  // namespace eclipse
