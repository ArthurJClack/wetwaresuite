// Shared between the flash tool's source files.
#pragma once

#include <memory>
#include <string>

#include "wetwaresuite/flash/serial.hpp"

namespace wetwaresuite::flash {

std::unique_ptr<SerialPort> openSimPort(const std::string& name, const SerialConfig& config);
// "avr", "esp", "stm32" for a sim kind; empty when unknown.
std::string simFamily(const std::string& kind);

inline void put32le(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}
inline std::uint32_t get32le(const Bytes& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

}  // namespace wetwaresuite::flash
