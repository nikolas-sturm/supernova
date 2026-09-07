#include "network_route.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace terra {
namespace {
constexpr auto kStreamPort = "47989";

bool containsVpnName(std::string_view name) noexcept {
    std::string lowercase{name};
    std::ranges::transform(lowercase, lowercase.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    for (const auto marker : {"vpn", "wireguard", "wintun", "openvpn", "zerotier",
                              "tailscale", "nebula"}) {
        if (lowercase.find(marker) != std::string::npos) return true;
    }
    const auto isInterfacePrefix = [&](std::string_view prefix) {
        const auto position = lowercase.find(prefix);
        if (position == std::string::npos ||
            (position != 0 && std::isalnum(static_cast<unsigned char>(lowercase[position - 1])))) {
            return false;
        }
        const auto suffix = position + prefix.size();
        return suffix == lowercase.size() ||
               !std::isalpha(static_cast<unsigned char>(lowercase[suffix]));
    };
    if (isInterfacePrefix("tun") || isInterfacePrefix("tap") || isInterfacePrefix("utun") ||
        isInterfacePrefix("ppp") || isInterfacePrefix("wg")) {
        return true;
    }
    return false;
}

std::pair<const unsigned char*, std::size_t> addressBytes(const sockaddr* address) noexcept {
    if (!address) return {nullptr, 0};
    if (address->sa_family == AF_INET) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        return {reinterpret_cast<const unsigned char*>(&ipv4->sin_addr), sizeof(ipv4->sin_addr)};
    }
    if (address->sa_family == AF_INET6) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        return {reinterpret_cast<const unsigned char*>(&ipv6->sin6_addr), sizeof(ipv6->sin6_addr)};
    }
    return {nullptr, 0};
}

bool sameAddress(const sockaddr* left, const sockaddr* right) noexcept {
    if (!left || !right || left->sa_family != right->sa_family) return false;
    const auto [leftBytes, leftSize] = addressBytes(left);
    const auto [rightBytes, rightSize] = addressBytes(right);
    return leftSize != 0 && leftSize == rightSize &&
           std::memcmp(leftBytes, rightBytes, leftSize) == 0;
}

#ifdef _WIN32
bool inSubnet(const sockaddr* address, const sockaddr* local, unsigned int prefixLength) noexcept {
    if (!address || !local || address->sa_family != local->sa_family) return false;
    const auto [addressData, addressSize] = addressBytes(address);
    const auto [localData, localSize] = addressBytes(local);
    if (addressSize == 0 || addressSize != localSize || prefixLength > addressSize * 8) return false;

    const auto wholeBytes = prefixLength / 8;
    const auto remainingBits = prefixLength % 8;
    if (wholeBytes != 0 && std::memcmp(addressData, localData, wholeBytes) != 0) return false;
    if (remainingBits == 0) return true;
    const auto mask = static_cast<unsigned char>(0xffU << (8U - remainingBits));
    return (addressData[wholeBytes] & mask) == (localData[wholeBytes] & mask);
}
#else
bool inSubnet(const sockaddr* address, const sockaddr* local, const sockaddr* mask) noexcept {
    if (!address || !local || !mask || address->sa_family != local->sa_family ||
        address->sa_family != mask->sa_family) {
        return false;
    }
    const auto [addressData, addressSize] = addressBytes(address);
    const auto [localData, localSize] = addressBytes(local);
    const auto [maskData, maskSize] = addressBytes(mask);
    if (addressSize == 0 || addressSize != localSize || addressSize != maskSize) return false;
    for (std::size_t index = 0; index < addressSize; ++index) {
        if ((addressData[index] & maskData[index]) != (localData[index] & maskData[index])) {
            return false;
        }
    }
    return true;
}
#endif

#ifdef _WIN32
class SocketRuntime {
public:
    SocketRuntime() noexcept : ready_(WSAStartup(MAKEWORD(2, 2), &data_) == 0) {}
    ~SocketRuntime() {
        if (ready_) WSACleanup();
    }
    explicit operator bool() const noexcept { return ready_; }

private:
    WSADATA data_{};
    bool ready_ = false;
};

std::string narrow(const wchar_t* value) {
    if (!value) return {};
    std::string result;
    while (*value != L'\0') {
        result.push_back(*value <= 0x7f ? static_cast<char>(*value) : ' ');
        ++value;
    }
    return result;
}

RouteReachability classifyWindowsRoute(const sockaddr* target) noexcept {
    DWORD interfaceIndex = 0;
    if (GetBestInterfaceEx(const_cast<sockaddr*>(target), &interfaceIndex) != NO_ERROR) {
        return RouteReachability::unknown;
    }

    ULONG size = 16 * 1024;
    std::vector<unsigned char> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    auto result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &size);
    }
    if (result != NO_ERROR) return RouteReachability::unknown;

    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
        if (adapter->IfIndex != interfaceIndex && adapter->Ipv6IfIndex != interfaceIndex) continue;
        const auto name = std::string{adapter->AdapterName ? adapter->AdapterName : ""} + " " +
                          narrow(adapter->FriendlyName) + " " + narrow(adapter->Description);
        if (isLikelyVpnInterface(name, false, adapter->Mtu, adapter->IfType)) {
            return RouteReachability::vpn;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            if (inSubnet(target, unicast->Address.lpSockaddr, unicast->OnLinkPrefixLength)) {
                return RouteReachability::lan;
            }
        }
        return RouteReachability::unknown;
    }
    return RouteReachability::unknown;
}
#else
unsigned int interfaceMtu(int socketHandle, const char* name) noexcept {
    ifreq request{};
    std::strncpy(request.ifr_name, name, IFNAMSIZ - 1);
    return ioctl(socketHandle, SIOCGIFMTU, &request) == 0
               ? static_cast<unsigned int>(request.ifr_mtu)
               : 0;
}

RouteReachability classifyPosixRoute(int socketHandle, const sockaddr* local,
                                     const sockaddr* target) noexcept {
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) return RouteReachability::unknown;

    auto result = RouteReachability::unknown;
    for (auto* interface = interfaces; interface; interface = interface->ifa_next) {
        if (!interface->ifa_addr || !sameAddress(interface->ifa_addr, local)) continue;
        const auto flags = interface->ifa_flags;
        if ((flags & IFF_UP) == 0) break;
        if (isLikelyVpnInterface(interface->ifa_name, (flags & IFF_POINTOPOINT) != 0,
                                 interfaceMtu(socketHandle, interface->ifa_name))) {
            result = RouteReachability::vpn;
        } else if (inSubnet(target, interface->ifa_addr, interface->ifa_netmask)) {
            result = RouteReachability::lan;
        }
        break;
    }
    freeifaddrs(interfaces);
    return result;
}
#endif
}  // namespace

bool isLikelyVpnInterface(std::string_view name, bool pointToPoint, unsigned int mtu,
                          unsigned int interfaceType) noexcept {
    constexpr unsigned int kIfTypePpp = 23;
    constexpr unsigned int kIfTypePropVirtual = 53;
    constexpr unsigned int kIfTypeTunnel = 131;
    return pointToPoint || (mtu != 0 && mtu < 1500) || interfaceType == kIfTypePpp ||
           interfaceType == kIfTypePropVirtual || interfaceType == kIfTypeTunnel ||
           containsVpnName(name);
}

StreamNetworkConfiguration streamNetworkConfiguration(RouteReachability reachability) noexcept {
    switch (reachability) {
        case RouteReachability::lan:
            return {1392, StreamingLocation::local};
        case RouteReachability::vpn:
            return {1024, StreamingLocation::remote};
        case RouteReachability::unknown:
            return {1392, StreamingLocation::automatic};
    }
    return {1392, StreamingLocation::automatic};
}

RouteReachability detectRouteReachability(std::string_view host) noexcept {
#ifdef _WIN32
    const SocketRuntime socketRuntime;
    if (!socketRuntime) return RouteReachability::unknown;
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* addresses = nullptr;
    const std::string hostString{host};
    const bool numericHost = getaddrinfo(hostString.c_str(), kStreamPort, &hints, &addresses) == 0;
    if (!numericHost) {
        hints.ai_flags = 0;
        if (getaddrinfo(hostString.c_str(), kStreamPort, &hints, &addresses) != 0) {
            return RouteReachability::unknown;
        }
    }

    auto result = RouteReachability::unknown;
    bool allRoutesAreLan = true;
    bool routeFound = false;
    for (auto* address = addresses; address; address = address->ai_next) {
        auto route = RouteReachability::unknown;
#ifdef _WIN32
        const auto socketHandle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketHandle == INVALID_SOCKET) continue;
        if (connect(socketHandle, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            route = classifyWindowsRoute(address->ai_addr);
        }
        closesocket(socketHandle);
#else
        const auto socketHandle = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socketHandle < 0) continue;
        sockaddr_storage local{};
        socklen_t localSize = sizeof(local);
        if (connect(socketHandle, address->ai_addr, address->ai_addrlen) == 0 &&
            getsockname(socketHandle, reinterpret_cast<sockaddr*>(&local), &localSize) == 0) {
            route = classifyPosixRoute(socketHandle, reinterpret_cast<sockaddr*>(&local),
                                       address->ai_addr);
        }
        close(socketHandle);
#endif
        routeFound = true;
        if (route == RouteReachability::vpn) {
            result = RouteReachability::vpn;
            break;
        }
        if (route != RouteReachability::lan) allRoutesAreLan = false;
    }
    freeaddrinfo(addresses);
    if (result != RouteReachability::vpn && numericHost && routeFound && allRoutesAreLan) {
        result = RouteReachability::lan;
    }
    return result;
}

}  // namespace terra
