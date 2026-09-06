#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eclipse {

using MacAddress = std::array<std::uint8_t, 6>;

std::optional<MacAddress> parseMacAddress(std::string_view value) noexcept;
std::string formatMacAddress(const MacAddress& address);
std::array<std::uint8_t, 102> makeWakePacket(const MacAddress& address) noexcept;
void sendWakeOnLan(const MacAddress& address);

}  // namespace eclipse
