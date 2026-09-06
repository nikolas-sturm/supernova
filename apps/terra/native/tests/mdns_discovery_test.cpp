#include "mdns_discovery.h"

#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void appendU16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 8U));
    data.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value >> 24U));
    data.push_back(static_cast<std::uint8_t>(value >> 16U));
    data.push_back(static_cast<std::uint8_t>(value >> 8U));
    data.push_back(static_cast<std::uint8_t>(value));
}

void appendName(std::vector<std::uint8_t>& data, std::string_view name) {
    std::size_t start = 0;
    while (start < name.size()) {
        const auto end = name.find('.', start);
        const auto length = (end == std::string_view::npos ? name.size() : end) - start;
        data.push_back(static_cast<std::uint8_t>(length));
        data.insert(data.end(), name.begin() + static_cast<std::ptrdiff_t>(start),
                    name.begin() + static_cast<std::ptrdiff_t>(start + length));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    data.push_back(0);
}

void appendRecord(std::vector<std::uint8_t>& packet, std::string_view name, std::uint16_t type,
                  const std::vector<std::uint8_t>& value) {
    appendName(packet, name);
    appendU16(packet, type);
    appendU16(packet, 1);
    appendU32(packet, 120);
    appendU16(packet, static_cast<std::uint16_t>(value.size()));
    packet.insert(packet.end(), value.begin(), value.end());
}

}  // namespace

int main() {
    std::vector<std::uint8_t> packet(12, 0);
    packet[2] = 0x84;
    packet[3] = 0x00;
    packet[7] = 3;

    std::vector<std::uint8_t> pointer;
    appendName(pointer, "Studio PC._nvstream._tcp.local");
    appendRecord(packet, "_nvstream._tcp.local", 12, pointer);

    std::vector<std::uint8_t> service(6, 0);
    service[4] = static_cast<std::uint8_t>(47989 >> 8U);
    service[5] = static_cast<std::uint8_t>(47989);
    appendName(service, "studio.local");
    appendRecord(packet, "Studio PC._nvstream._tcp.local", 33, service);
    appendRecord(packet, "studio.local", 1, {192, 168, 1, 40});

    const auto services = eclipse::parseMdnsResponse(packet);
    expect(services.size() == 1, "Complete Sunshine DNS-SD response was not resolved.");
    expect(services.front().name == "studio pc", "Service instance name was not parsed.");
    expect(services.front().hostname == "studio.local", "SRV hostname was not parsed.");
    expect(services.front().address == "192.168.1.40", "IPv4 address was not parsed.");
    expect(services.front().port == 47989, "SRV port was not parsed.");

    std::vector<std::uint8_t> ipv6Packet(12, 0);
    ipv6Packet[2] = 0x84;
    ipv6Packet[7] = 3;
    std::vector<std::uint8_t> ipv6Pointer;
    appendName(ipv6Pointer, "IPv6 PC._nvstream._tcp.local");
    appendRecord(ipv6Packet, "_nvstream._tcp.local", 12, ipv6Pointer);
    std::vector<std::uint8_t> ipv6Service(6, 0);
    ipv6Service[4] = static_cast<std::uint8_t>(47989 >> 8U);
    ipv6Service[5] = static_cast<std::uint8_t>(47989);
    appendName(ipv6Service, "ipv6.local");
    appendRecord(ipv6Packet, "IPv6 PC._nvstream._tcp.local", 33, ipv6Service);
    appendRecord(ipv6Packet, "ipv6.local", 28,
                 {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40});
    const auto ipv6Services = eclipse::parseMdnsResponse(ipv6Packet);
    expect(ipv6Services.size() == 1, "AAAA-only Sunshine response was not resolved.");
    expect(ipv6Services.front().address == "2001:db8::40", "IPv6 address was not parsed.");

    auto linkLocalPacket = ipv6Packet;
    linkLocalPacket.resize(linkLocalPacket.size() - 16);
    linkLocalPacket.insert(linkLocalPacket.end(),
                           {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1});
    const auto linkLocalServices = eclipse::parseMdnsResponse(linkLocalPacket, 7);
    expect(linkLocalServices.size() == 1 && linkLocalServices.front().address == "fe80::1%7",
           "Link-local AAAA record lost its interface scope.");

    bool rejected = false;
    try {
        static_cast<void>(eclipse::parseMdnsResponse(std::vector<std::uint8_t>{0, 1, 2}));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "Truncated mDNS packet was accepted.");

    std::vector<std::uint8_t> invalidRecord(12, 0);
    invalidRecord[2] = 0x84;
    invalidRecord[7] = 1;
    appendName(invalidRecord, "_nvstream._tcp.local");
    appendU16(invalidRecord, 12);
    appendU16(invalidRecord, 1);
    appendU32(invalidRecord, 120);
    appendU16(invalidRecord, 0);
    rejected = false;
    try {
        static_cast<void>(eclipse::parseMdnsResponse(invalidRecord));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    expect(rejected, "PTR name outside its RDATA boundary was accepted.");
}
