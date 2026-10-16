// Firmware files: Intel HEX, raw .bin, ELF (program headers and sections) and
// the Espressif app/bootloader image header. Everything is turned into a list
// of address-tagged segments that the protocol code writes.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "achclwss/flash/serial.hpp"

namespace achclwss::flash {

class FirmwareError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Segment {
    std::uint32_t address = 0;
    Bytes data;
    std::uint32_t end() const { return address + static_cast<std::uint32_t>(data.size()); }
};

struct ElfSection {
    std::string name;
    std::uint32_t address = 0;  // VMA
    std::uint32_t loadAddress = 0;  // LMA, where it lives in flash
    std::uint32_t size = 0;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    bool alloc() const { return (flags & 2u) != 0; }
};

struct EspSegment {
    std::uint32_t loadAddress = 0;
    std::uint32_t size = 0;
};

struct EspImageInfo {
    std::uint8_t segmentCount = 0;
    std::uint8_t flashMode = 0;     // 0 QIO, 1 QOUT, 2 DIO, 3 DOUT
    std::uint8_t flashSizeFreq = 0;  // high nibble size code, low nibble frequency code
    std::uint32_t entry = 0;
    bool extended = false;           // ESP32-family 16-byte extended header present
    std::uint16_t chipId = 0;        // from the extended header
    bool hashAppended = false;
    std::vector<EspSegment> segments;
    bool checksumOk = false;
    std::uint8_t checksum = 0;
    std::string chipName() const;    // "esp32", "esp32-s3", ... or "esp8266" when no extended header
    std::string flashModeName() const;
    std::string flashSizeName() const;
    std::string flashFreqName() const;
};

struct Firmware {
    std::string path;
    std::string format;  // "ihex", "bin", "elf", "esp-image" (a .bin with an ESP header)
    std::vector<Segment> segments;  // sorted, non-overlapping, adjacent ones merged
    std::optional<std::uint32_t> entry;
    // ELF extras
    std::string elfMachine;  // "AVR", "ARM", "Xtensa", "RISC-V", ...
    std::uint16_t elfMachineId = 0;
    std::vector<ElfSection> sections;
    // ESP extras (for .bin files starting with 0xE9)
    std::optional<EspImageInfo> esp;
    bool espMerged = false;  // bootloader + partition table + app in one file (flash at the bootloader offset)
    std::uint32_t espMergedBootOffset = 0;  // where that merged file's data starts relative to flash 0 (0x1000 or 0)
    std::uint32_t espBootloaderAt = 0;      // flash address of the merged file's bootloader (must match the chip)

    std::size_t byteCount() const;
    std::uint32_t lowAddress() const;
    std::uint32_t highAddress() const;  // one past the last byte
    // One contiguous block from lowAddress() to highAddress(), gaps filled with `fill`.
    Bytes flatten(std::uint8_t fill = 0xFF) const;
};

// Parses by content (":" = HEX, "\x7fELF" = ELF, else raw). A raw .bin gets `binAddress`.
Firmware loadFirmware(const std::filesystem::path& path, std::uint32_t binAddress = 0);
Firmware parseFirmware(const Bytes& bytes, const std::string& nameHint, std::uint32_t binAddress = 0);

Firmware parseIntelHex(const std::string& text);
Firmware parseElf(const Bytes& bytes);
std::optional<EspImageInfo> parseEspImage(const Bytes& bytes);
// Detects a merged ESP32 image (bootloader at 0 of the file, partition table at 0x8000 - offset).
bool looksLikeMergedEsp(const Bytes& bytes, std::uint32_t& bootOffset);

// Writes segments back out as Intel HEX (used by tests and to hand avrdude a file).
std::string toIntelHex(const std::vector<Segment>& segments, std::size_t perLine = 16);

// Sorts and merges touching segments; throws on overlaps.
std::vector<Segment> normalizeSegments(std::vector<Segment> segments);

// Checksums and digests used by the protocols.
std::uint8_t hexChecksum(const std::uint8_t* data, std::size_t size);  // two's complement of the sum
std::uint8_t espChecksum(const std::uint8_t* data, std::size_t size, std::uint8_t seed = 0xEF);  // XOR
std::uint8_t xorBytes(const std::uint8_t* data, std::size_t size, std::uint8_t seed = 0);
std::uint32_t crc32(const std::uint8_t* data, std::size_t size);
std::string md5Hex(const std::uint8_t* data, std::size_t size);
Bytes md5(const std::uint8_t* data, std::size_t size);

std::string hex32(std::uint32_t v);  // "0x08000000"
std::uint32_t parseAddress(const std::string& text);  // "0x1000", "4096", "64k"

}  // namespace achclwss::flash
