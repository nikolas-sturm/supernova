#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace eclipse {

struct MdnsService {
    std::string name;
    std::string hostname;
    std::string address;
    std::uint16_t port = 0;
};

[[nodiscard]] std::vector<MdnsService> parseMdnsResponse(std::span<const std::uint8_t> packet,
                                                         unsigned int ipv6ScopeId = 0);

class MdnsDiscovery {
public:
    using Listener = std::function<void(const MdnsService&)>;

    explicit MdnsDiscovery(Listener listener);
    ~MdnsDiscovery();

    MdnsDiscovery(const MdnsDiscovery&) = delete;
    MdnsDiscovery& operator=(const MdnsDiscovery&) = delete;

    void start();
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eclipse
