#include "wake_on_lan.h"

#include <algorithm>
#include <stdexcept>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    const auto address = eclipse::parseMacAddress(" 02:1A:2b:3C:4d:5E ");
    expect(address.has_value(), "Valid MAC address was rejected.");
    expect(eclipse::formatMacAddress(*address) == "02:1a:2b:3c:4d:5e",
           "MAC address was not canonicalized.");
    expect(!eclipse::parseMacAddress("00:00:00:00:00:00"), "Zero MAC address was accepted.");
    expect(!eclipse::parseMacAddress("01:00:00:00:00:01"), "Multicast MAC address was accepted.");
    expect(!eclipse::parseMacAddress("02:00-00:00:00:01"), "Mixed separators were accepted.");

    const auto packet = eclipse::makeWakePacket(*address);
    expect(std::all_of(packet.begin(), packet.begin() + 6,
                       [](std::uint8_t value) { return value == 0xFF; }),
           "Wake packet prefix is invalid.");
    for (std::size_t repetition = 0; repetition < 16; ++repetition) {
        expect(std::equal(address->begin(), address->end(),
                          packet.begin() + 6 + repetition * address->size()),
               "Wake packet MAC repetition is invalid.");
    }
}
