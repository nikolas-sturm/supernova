#include "wake_on_lan.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
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

int hexDigit(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

#if defined(_WIN32)
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Cannot initialize Windows sockets for Wake-on-LAN.");
        }
    }
    ~SocketRuntime() { WSACleanup(); }
};
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
void closeSocket(NativeSocket socket) { closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
void closeSocket(NativeSocket socket) { close(socket); }
#endif

}  // namespace

std::optional<MacAddress> parseMacAddress(std::string_view value) noexcept {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    if (value.size() != 17 || (value[2] != ':' && value[2] != '-')) return std::nullopt;
    const char separator = value[2];
    MacAddress result{};
    bool nonzero = false;
    for (std::size_t index = 0; index < result.size(); ++index) {
        const auto offset = index * 3;
        if (index != 0 && value[offset - 1] != separator) return std::nullopt;
        const int high = hexDigit(value[offset]);
        const int low = hexDigit(value[offset + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        result[index] = static_cast<std::uint8_t>((high << 4) | low);
        nonzero = nonzero || result[index] != 0;
    }
    if (!nonzero || (result[0] & 1U) != 0) return std::nullopt;
    return result;
}

std::string formatMacAddress(const MacAddress& address) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(17, ':');
    for (std::size_t index = 0; index < address.size(); ++index) {
        result[index * 3] = digits[address[index] >> 4U];
        result[index * 3 + 1] = digits[address[index] & 0x0FU];
    }
    return result;
}

std::array<std::uint8_t, 102> makeWakePacket(const MacAddress& address) noexcept {
    std::array<std::uint8_t, 102> packet{};
    std::fill_n(packet.begin(), 6, 0xFF);
    for (std::size_t repetition = 0; repetition < 16; ++repetition) {
        std::copy(address.begin(), address.end(), packet.begin() + 6 + repetition * address.size());
    }
    return packet;
}

void sendWakeOnLan(const MacAddress& address) {
#if defined(_WIN32)
    SocketRuntime runtime;
#endif
    const NativeSocket socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socketHandle == kInvalidSocket) {
        throw std::runtime_error("Cannot create Wake-on-LAN socket.");
    }
    const int enabled = 1;
    if (setsockopt(socketHandle, SOL_SOCKET, SO_BROADCAST,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0) {
        closeSocket(socketHandle);
        throw std::runtime_error("Cannot enable Wake-on-LAN broadcast.");
    }

    std::unordered_set<std::uint32_t> broadcastAddresses{htonl(INADDR_BROADCAST)};
#if !defined(_WIN32)
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0) {
        for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
            if (!interface->ifa_addr || !interface->ifa_netmask ||
                interface->ifa_addr->sa_family != AF_INET ||
                (interface->ifa_flags & IFF_UP) == 0 ||
                (interface->ifa_flags & IFF_LOOPBACK) != 0 ||
                (interface->ifa_flags & IFF_BROADCAST) == 0) {
                continue;
            }
            const auto address = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr)
                                     ->sin_addr.s_addr;
            const auto mask = reinterpret_cast<const sockaddr_in*>(interface->ifa_netmask)
                                  ->sin_addr.s_addr;
            broadcastAddresses.insert(address | ~mask);
        }
        freeifaddrs(interfaces);
    }
#endif
    const auto packet = makeWakePacket(address);
    bool sent = false;
    for (const auto broadcastAddress : broadcastAddresses) {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(9);
        destination.sin_addr.s_addr = broadcastAddress;
        for (int attempt = 0; attempt < 3; ++attempt) {
#if defined(_WIN32)
            const auto result = sendto(socketHandle, reinterpret_cast<const char*>(packet.data()),
                                       static_cast<int>(packet.size()), 0,
                                       reinterpret_cast<const sockaddr*>(&destination),
                                       sizeof(destination));
#else
            const auto result = sendto(socketHandle, packet.data(), packet.size(), 0,
                                       reinterpret_cast<const sockaddr*>(&destination),
                                       sizeof(destination));
#endif
            sent = sent || (result >= 0 && static_cast<std::size_t>(result) == packet.size());
        }
    }
    closeSocket(socketHandle);
    if (!sent) throw std::runtime_error("Wake-on-LAN broadcast failed.");
}

}  // namespace eclipse
