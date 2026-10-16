// flash: put firmware on Arduino (AVR), ESP32/ESP8266 and STM32 boards from
// the command line, batch scripts or Claude over MCP. The bootloader protocols
// are implemented natively (protocol.hpp) on a small serial layer
// (serial.hpp); avrdude, esptool, STM32_Programmer_CLI, st-flash and
// dfu-util are used when picked with backend= (or for ST-Link/DFU, which are
// not serial bootloaders).
#pragma once

#include <string>
#include <vector>

#include "achclwss/core/command.hpp"
#include "achclwss/flash/firmware.hpp"
#include "achclwss/flash/protocol.hpp"
#include "achclwss/flash/serial.hpp"

namespace achclwss::flash {

// One region of a multi-part ESP upload: "0x1000=bootloader.bin".
struct Part {
    std::uint32_t address = 0;
    std::string path;
};
// Parses "0x1000=boot.bin,0x8000=part.bin,0x10000=app.bin" (also ';' or newlines).
std::vector<Part> parseParts(const std::string& text);

struct FlashRequest {
    std::string port;
    std::string file;           // one firmware file, or
    std::vector<Part> parts;    // several (ESP)
    std::string board = "auto";  // a preset name, or auto (from the port's USB id, else detect)
    std::string protocol;       // override the preset's
    int baud = 0;               // 0 = the preset's
    int flashBaud = 0;          // ESP: switch to this after connecting (0 = stay)
    std::string reset;          // empty = the preset's
    std::optional<std::uint32_t> address;  // for raw .bin
    bool verify = true;
    bool erase = true;          // STM32 mass erase before writing (needed: flash only goes 1 -> 0)
    bool run = true;            // start the new firmware afterwards
    std::uint32_t espFlashSize = 4u << 20;  // what SPI_SET_PARAMS is told
};

struct FlashReport {
    std::string board;
    std::string protocol;
    std::string chip;
    std::string backend = "native";
    std::size_t bytes = 0;
    std::size_t blocks = 0;  // pages/blocks written
    bool verified = false;
    std::string verifyMethod;  // "read-back", "md5", ""
    double seconds = 0;
    std::vector<std::string> log;
    std::vector<std::string> regions;  // "0x10000 +12345"
};

// Resolves board/protocol/baud/reset from the request, the preset table and the port's USB id.
struct Resolved {
    Board board;
    std::string protocol;
    int baud = 115200;
    std::string reset;
    std::string why;  // how the board was chosen
};
Resolved resolve(const FlashRequest& request, const PortInfo& port);

// Runs the native flash flow on an open port. Throws ProtocolError/SerialError/FirmwareError.
FlashReport flashNative(SerialPort& port, const FlashRequest& request, const Resolved& resolved);
// Opens the port (handling the Leonardo 1200 baud touch and port re-enumeration) and flashes.
FlashReport flashPort(const FlashRequest& request);

struct DetectReport {
    bool found = false;
    std::string protocol;
    std::string chip;
    std::string board;  // best preset guess
    int baud = 0;
    std::string details;
    std::vector<std::string> tried;
};
// Probes a port: the USB id first, then (unless probe=false) each bootloader in turn.
// `only` restricts it to one family ("avr", "esp", "stm32").
DetectReport detectBoard(const std::string& port, const std::string& only, bool probe);

// ---- external tools --------------------------------------------------------------------

struct ExternalPlan {
    std::string program;  // what findProgram looks for
    std::vector<std::string> args;  // args[0] is the program name
    std::string error;
};
// backend: avrdude, esptool, stm32prog (STM32_Programmer_CLI), st-flash, dfu-util.
ExternalPlan planExternal(const std::string& backend, const FlashRequest& request, const Resolved& resolved);
// Which external tools are on PATH ("esptool" also matches esptool.py).
std::vector<std::pair<std::string, std::string>> externalTools();
std::optional<std::string> findBackendProgram(const std::string& backend);

// ---- monitor / send ---------------------------------------------------------------------

// Turns "hello\\n" style escapes into bytes (\n \r \t \\ \xNN \0).
std::string unescape(const std::string& text);
// Makes received bytes printable: keeps text, writes other bytes as \xNN.
std::string printable(const std::string& bytes);

void registerCommands(Session& session);

}  // namespace achclwss::flash
