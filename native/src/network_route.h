#pragma once

#include <string_view>

namespace eclipse {

enum class RouteReachability { unknown, lan, vpn };
enum class StreamingLocation { automatic, local, remote };

struct StreamNetworkConfiguration {
    int packetSize;
    StreamingLocation location;
};

[[nodiscard]] bool isLikelyVpnInterface(std::string_view name, bool pointToPoint,
                                        unsigned int mtu, unsigned int interfaceType = 0) noexcept;
[[nodiscard]] StreamNetworkConfiguration streamNetworkConfiguration(
    RouteReachability reachability) noexcept;
[[nodiscard]] RouteReachability detectRouteReachability(std::string_view host) noexcept;

}  // namespace eclipse
