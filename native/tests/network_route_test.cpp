#include "network_route.h"

#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    using eclipse::RouteReachability;
    using eclipse::StreamingLocation;

    const auto unknown = eclipse::streamNetworkConfiguration(RouteReachability::unknown);
    expect(unknown.packetSize == 1392 && unknown.location == StreamingLocation::automatic,
           "Unknown route did not preserve automatic remote detection.");

    const auto lan = eclipse::streamNetworkConfiguration(RouteReachability::lan);
    expect(lan.packetSize == 1392 && lan.location == StreamingLocation::local,
           "LAN route did not select local packet policy.");

    const auto vpn = eclipse::streamNetworkConfiguration(RouteReachability::vpn);
    expect(vpn.packetSize == 1024 && vpn.location == StreamingLocation::remote,
           "VPN route did not select reduced remote packet policy.");

    expect(eclipse::isLikelyVpnInterface("wg0", false, 1420),
           "WireGuard interface was not identified.");
    expect(eclipse::isLikelyVpnInterface("Ethernet", true, 1500),
           "Point-to-point interface was not identified.");
    expect(eclipse::isLikelyVpnInterface("Ethernet", false, 1400),
           "Reduced-MTU interface was not identified.");
    expect(eclipse::isLikelyVpnInterface("Ethernet", false, 1500, 131),
           "Windows tunnel interface type was not identified.");
    expect(!eclipse::isLikelyVpnInterface("Ethernet", false, 1500),
           "Normal Ethernet interface was identified as VPN.");
    expect(eclipse::detectRouteReachability("127.0.0.1") == RouteReachability::lan,
           "Loopback route was not identified as local.");
    expect(eclipse::detectRouteReachability("localhost") == RouteReachability::unknown,
           "DNS route incorrectly forced local transport policy.");
}
