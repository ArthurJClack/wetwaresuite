// The Espressif ROM serial bootloader protocol (what esptool --no-stub speaks).
#include <algorithm>
#include <chrono>
#include <cstdio>

#include "wetwaresuite/flash/protocol.hpp"
#include "internal.hpp"

namespace wetwaresuite::flash {

namespace slip {
Bytes encode(const Bytes& packet) {
    Bytes out;
    out.reserve(packet.size() + 8);
    out.push_back(kEnd);
    for (auto b : packet) {
        if (b == kEnd) {
            out.push_back(kEsc);
            out.push_back(kEscEnd);
        } else if (b == kEsc) {
            out.push_back(kEsc);
            out.push_back(kEscEsc);
        } else {
            out.push_back(b);
        }
    }
    out.push_back(kEnd);
    return out;
}

std::optional<Bytes> Decoder::feed(std::uint8_t b) {
    if (!inPacket_) {
        if (b == kEnd) {
            inPacket_ = true;
            escape_ = false;
            buf_.clear();
        }
        return std::nullopt;  // text the ROM prints between packets
    }
    if (b == kEnd) {
        if (buf_.empty()) return std::nullopt;  // back-to-back delimiters
        inPacket_ = false;
        Bytes p;
        p.swap(buf_);
        return p;
    }
    if (escape_) {
        escape_ = false;
        buf_.push_back(static_cast<std::uint8_t>(b == kEscEnd ? kEnd : b == kEscEsc ? kEsc : b));
        return std::nullopt;
    }
    if (b == kEsc) {
        escape_ = true;
        return std::nullopt;
    }
    buf_.push_back(b);
    return std::nullopt;
}
}  // namespace slip

namespace esp {

Bytes request(std::uint8_t op, const Bytes& data, std::uint32_t checksum) {
    Bytes r = {0x00, op, static_cast<std::uint8_t>(data.size() & 0xFF), static_cast<std::uint8_t>((data.size() >> 8) & 0xFF)};
    put32le(r, checksum);
    r.insert(r.end(), data.begin(), data.end());
    return r;
}

Bytes syncPayload() {
    Bytes b = {0x07, 0x07, 0x12, 0x20};
    b.insert(b.end(), 32, 0x55);
    return b;
}

Bytes flashBeginPayload(std::uint32_t eraseSize, std::uint32_t blocks, std::uint32_t blockSize, std::uint32_t offset, bool encryptedArg) {
    Bytes b;
    put32le(b, eraseSize);
    put32le(b, blocks);
    put32le(b, blockSize);
    put32le(b, offset);
    if (encryptedArg) put32le(b, 0);
    return b;
}

Bytes flashDataPayload(const Bytes& block, std::uint32_t seq) {
    Bytes b;
    put32le(b, static_cast<std::uint32_t>(block.size()));
    put32le(b, seq);
    put32le(b, 0);
    put32le(b, 0);
    b.insert(b.end(), block.begin(), block.end());
    return b;
}

Bytes md5Payload(std::uint32_t address, std::uint32_t size) {
    Bytes b;
    put32le(b, address);
    put32le(b, size);
    put32le(b, 0);
    put32le(b, 0);
    return b;
}

std::optional<Response> parseResponse(const Bytes& p) {
    if (p.size() < 8 || p[0] != 0x01) return std::nullopt;
    const std::size_t size = static_cast<std::size_t>(p[2] | (p[3] << 8));
    if (p.size() != 8 + size) return std::nullopt;
    Response r;
    r.op = p[1];
    r.value = get32le(p, 4);
    r.data.assign(p.begin() + 8, p.end());
    return r;
}

const std::vector<Chip>& chips() {
    static const std::vector<Chip> list = {
        {"esp8266", {0xfff0c101u}, 2, false, false, false, false, 0x0, 0},
        {"esp32", {0x00f01d83u}, 4, false, true, true, true, 0x1000, 0},
        {"esp32-s2", {0x000007c6u}, 4, true, true, true, true, 0x1000, 2},
        {"esp32-s3", {0x00000009u}, 4, true, true, true, true, 0x0, 9},
        {"esp32-c3", {0x6921506fu, 0x1b31506fu, 0x4881606fu, 0x4361606fu}, 4, true, true, true, true, 0x0, 5},
        {"esp32-c2", {0x6f51306fu, 0x7c41a06fu}, 4, true, true, true, true, 0x0, 12},
        {"esp32-c6", {0x2ce0806fu}, 4, true, true, true, true, 0x0, 13},
        {"esp32-h2", {0xd7b73e80u}, 4, true, true, true, true, 0x0, 16},
    };
    return list;
}

const Chip* findChip(const std::string& name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.size() > 5 && n.rfind("esp32", 0) == 0 && n[5] != '-') n.insert(5, "-");  // esp32s3 -> esp32-s3
    for (const auto& c : chips()) {
        if (c.name == n) return &c;
    }
    return nullptr;
}

const Chip* chipFromMagic(std::uint32_t magic) {
    for (const auto& c : chips()) {
        if (std::find(c.magic.begin(), c.magic.end(), magic) != c.magic.end()) return &c;
    }
    return nullptr;
}

std::uint32_t eraseSize(const Chip& chip, std::uint32_t offset, std::uint32_t size) {
    if (chip.name != "esp8266") return size;
    // esptool's ESP8266ROM.get_erase_size: the ROM erases the head sectors twice
    const std::uint32_t sectorsPerBlock = 16, sector = kSector;
    const std::uint32_t numSectors = (size + sector - 1) / sector;
    const std::uint32_t startSector = offset / sector;
    std::uint32_t head = sectorsPerBlock - (startSector % sectorsPerBlock);
    if (numSectors < head) head = numSectors;
    if (numSectors < 2 * head) return (numSectors + 1) / 2 * sector;
    return (numSectors - head) * sector;
}

}  // namespace esp

namespace {
std::string errorText(std::uint8_t e) {
    switch (e) {
    case 0x05: return "received message is invalid";
    case 0x06: return "failed to act on received message";
    case 0x07: return "invalid CRC in message";
    case 0x08: return "flash write error";
    case 0x09: return "flash read error";
    case 0x0A: return "flash read length error";
    case 0x0B: return "deflate error";
    default: return "error";
    }
}

std::string opName(std::uint8_t op) {
    switch (op) {
    case esp::kFlashBegin: return "FLASH_BEGIN";
    case esp::kFlashData: return "FLASH_DATA";
    case esp::kFlashEnd: return "FLASH_END";
    case esp::kSync: return "SYNC";
    case esp::kReadReg: return "READ_REG";
    case esp::kSpiAttach: return "SPI_ATTACH";
    case esp::kSpiSetParams: return "SPI_SET_PARAMS";
    case esp::kChangeBaud: return "CHANGE_BAUDRATE";
    case esp::kSpiFlashMd5: return "SPI_FLASH_MD5";
    default: {
        char buf[8];
        std::snprintf(buf, sizeof buf, "0x%02x", op);
        return buf;
    }
    }
}

int timeoutFor(std::uint64_t bytes, int msPerMb) {
    return static_cast<int>(std::max<std::uint64_t>(3000, bytes * static_cast<std::uint64_t>(msPerMb) / (1024 * 1024)));
}
}  // namespace

std::optional<Bytes> EspLoader::readPacket(int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return std::nullopt;
        auto b = port_.readByte(static_cast<int>(left));
        if (!b) return std::nullopt;
        if (auto p = decoder_.feed(*b)) return p;
    }
}

esp::Response EspLoader::command(std::uint8_t op, const Bytes& data, std::uint32_t checksum, int timeoutMs, std::size_t payloadLen) {
    port_.write(slip::encode(esp::request(op, data, checksum)));
    for (int i = 0; i < 100; ++i) {
        auto pkt = readPacket(timeoutMs);
        if (!pkt) break;
        auto r = esp::parseResponse(*pkt);
        if (!r || r->op != op) continue;  // a late answer to something earlier
        if (r->data.size() < payloadLen + 2) throw ProtocolError("ESP ROM: short answer to " + opName(op));
        const std::uint8_t status = r->data[payloadLen], error = r->data[payloadLen + 1];
        if (status != 0) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "0x%02x", error);
            throw ProtocolError("ESP ROM refused " + opName(op) + ": " + errorText(error) + " (" + buf + ")");
        }
        return *r;
    }
    throw ProtocolError("ESP ROM: no answer to " + opName(op));
}

bool EspLoader::syncOnce() {
    port_.write(slip::encode(esp::request(esp::kSync, esp::syncPayload())));
    for (int i = 0; i < 4; ++i) {
        auto pkt = readPacket(100);
        if (!pkt) return false;
        auto r = esp::parseResponse(*pkt);
        if (r && r->op == esp::kSync) return true;
    }
    return false;
}

void EspLoader::connect(const std::string& reset, int attempts) {
    for (int a = 0; a < attempts; ++a) {
        if (reset == "esp-classic" || reset == "auto") {
            // alternate the two sequences on "auto", like esptool does
            if (reset == "auto" && a % 2 == 1) resetEspUsbJtag(port_);
            else resetEspClassic(port_);
        } else if (reset == "esp-usb-jtag") {
            resetEspUsbJtag(port_);
        }
        port_.pause(50);
        port_.flushInput();
        decoder_.reset();
        for (int s = 0; s < 5; ++s) {
            if (syncOnce()) {
                // the ROM answers SYNC several times; drain the rest
                while (readPacket(60)) {
                }
                if (log_) log_("synced with the ESP ROM bootloader");
                return;
            }
        }
    }
    throw ProtocolError(
        "no answer from the ESP ROM bootloader. Hold the BOOT (IO0) button, tap EN/RESET, release BOOT, then retry with reset=none; "
        "check the baud and that the cable carries data; ESP32-S3/C3 boards on their native USB port need reset=esp-usb-jtag");
}

std::uint32_t EspLoader::readReg(std::uint32_t address) {
    Bytes d;
    put32le(d, address);
    return command(esp::kReadReg, d).value;
}

const esp::Chip& EspLoader::detectChip() {
    const std::uint32_t magic = readReg(esp::kChipDetectReg);
    chip_ = esp::chipFromMagic(magic);
    if (!chip_) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%08x", magic);
        throw ProtocolError(std::string("unknown Espressif chip (detect register ") + buf + "); pass board= to say which it is");
    }
    if (log_) log_("chip is " + chip_->name);
    return *chip_;
}

void EspLoader::attachFlash(std::uint32_t flashSize) {
    if (!chip_) detectChip();
    if (chip_->spiAttach) {
        command(esp::kSpiAttach, Bytes(8, 0));
        Bytes p;
        for (std::uint32_t v : {0u, flashSize, 64u * 1024u, 4u * 1024u, 256u, 0xFFFFu}) put32le(p, v);
        command(esp::kSpiSetParams, p);
    } else {
        // the ESP8266 ROM has no SPI_ATTACH; an empty FLASH_BEGIN attaches the flash
        command(esp::kFlashBegin, esp::flashBeginPayload(0, 0, esp::kFlashBlock, 0, false));
    }
}

void EspLoader::changeBaud(int baud) {
    if (!chip_) detectChip();
    if (!chip_->changeBaud) {
        if (log_) log_(chip_->name + " ROM can't change baud; staying at " + std::to_string(port_.config().baud));
        return;
    }
    Bytes p;
    put32le(p, static_cast<std::uint32_t>(baud));
    put32le(p, 0);
    command(esp::kChangeBaud, p);
    SerialConfig c = port_.config();
    c.baud = baud;
    port_.configure(c);
    port_.pause(50);
    port_.flushInput();
    if (log_) log_("switched to " + std::to_string(baud) + " baud");
}

std::size_t EspLoader::writeFlash(std::uint32_t offset, const Bytes& data) {
    if (!chip_) detectChip();
    if (offset % esp::kSector != 0) throw ProtocolError("ESP flash offsets must be multiples of 0x1000 (4 KB sectors), got " + hex32(offset));
    const std::uint32_t size = static_cast<std::uint32_t>(data.size());
    const std::uint32_t blocks = (size + esp::kFlashBlock - 1) / esp::kFlashBlock;
    const std::uint32_t erase = esp::eraseSize(*chip_, offset, size);
    if (log_) log_("erasing and writing " + std::to_string(size) + " bytes at " + hex32(offset));
    command(esp::kFlashBegin, esp::flashBeginPayload(erase, blocks, esp::kFlashBlock, offset, chip_->encryptedArg), 0, timeoutFor(size, 30000));
    std::uint32_t nextLog = blocks / 4;
    for (std::uint32_t seq = 0; seq < blocks; ++seq) {
        const std::size_t from = static_cast<std::size_t>(seq) * esp::kFlashBlock;
        Bytes block(data.begin() + static_cast<std::ptrdiff_t>(from),
                    data.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(from + esp::kFlashBlock, data.size())));
        block.resize(esp::kFlashBlock, 0xFF);
        const std::uint32_t sum = espChecksum(block.data(), block.size());
        for (int attempt = 0;; ++attempt) {
            try {
                command(esp::kFlashData, esp::flashDataPayload(block, seq), sum, 5000);
                break;
            } catch (const ProtocolError& e) {
                if (attempt >= 2 || std::string(e.what()).find("no answer") == std::string::npos) throw;
                if (log_) log_("block " + std::to_string(seq) + " got no answer, retrying");
            }
        }
        if (log_ && blocks > 16 && seq + 1 >= nextLog && seq + 1 < blocks) {
            log_("wrote " + std::to_string(seq + 1) + "/" + std::to_string(blocks) + " blocks");
            nextLog += blocks / 4;
        }
    }
    return blocks;
}

std::string EspLoader::flashMd5(std::uint32_t offset, std::uint32_t size) {
    if (!chip_) detectChip();
    if (!chip_->md5) throw ProtocolError(chip_->name + " ROM has no MD5 command");
    esp::Response r = command(esp::kSpiFlashMd5, esp::md5Payload(offset, size), 0, timeoutFor(size, 8000), 32);
    return std::string(r.data.begin(), r.data.begin() + 32);
}

void EspLoader::finish(bool reboot) {
    // like esptool: an empty FLASH_BEGIN, then FLASH_END telling the ROM to stay (we reset with RTS after)
    command(esp::kFlashBegin, esp::flashBeginPayload(0, 0, esp::kFlashBlock, 0, chip_ && chip_->encryptedArg));
    Bytes p;
    put32le(p, reboot ? 0u : 1u);
    command(esp::kFlashEnd, p);
}

}  // namespace wetwaresuite::flash
