// Board presets, reset sequences and the three AVR bootloader protocols.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>

#include "achclwss/flash/protocol.hpp"
#include "internal.hpp"

namespace achclwss::flash {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string hexBytes(const std::uint8_t* d, std::size_t n) {
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < n; ++i) {
        std::snprintf(buf, sizeof buf, "%s%02x", i ? " " : "", d[i]);
        s += buf;
    }
    return s;
}
}  // namespace

// ---- presets ------------------------------------------------------------------------------

const std::vector<Board>& boards() {
    static const std::vector<Board> list = {
        {"uno", "Arduino Uno (optiboot)", "avr", "stk500v1", 115200, "arduino", "atmega328p", 32256, 128, 0,
         "also Uno clones with a CH340; the 512 byte optiboot sits at the top of flash"},
        {"nano", "Arduino Nano, new bootloader (optiboot)", "avr", "stk500v1", 115200, "arduino", "atmega328p", 32256, 128, 0,
         "Nanos sold since 2018; if it doesn't answer try board=nano-old"},
        {"nano-old", "Arduino Nano, old bootloader (ATmegaBOOT)", "avr", "stk500v1", 57600, "arduino", "atmega328p", 30720, 128, 0,
         "most clones and pre-2018 Nanos: 57600 baud, 2 KB bootloader"},
        {"pro-mini", "Arduino Pro Mini / Duemilanove (ATmega328P)", "avr", "stk500v1", 57600, "arduino", "atmega328p", 30720, 128, 0,
         "needs a USB-serial adapter with DTR wired to the DTR/GRN pin (else reset=none and press reset as the upload starts)"},
        {"mega2560", "Arduino Mega 2560", "avr", "stk500v2", 115200, "arduino", "atmega2560", 253952, 256, 0, "wiring STK500v2 bootloader, 8 KB"},
        {"leonardo", "Arduino Leonardo (Caterina)", "avr", "avr109", 57600, "1200bps", "atmega32u4", 28672, 128, 0,
         "native USB: opening the port at 1200 baud restarts it into the bootloader for 8 s, often as a new port"},
        {"micro", "Arduino Micro (Caterina)", "avr", "avr109", 57600, "1200bps", "atmega32u4", 28672, 128, 0, "same as leonardo"},
        {"esp32", "ESP32 (DevKitC, WROOM/WROVER boards)", "esp", "esp", 115200, "esp-classic", "esp32", 4u << 20, 0, 0x10000,
         "a raw .bin goes to 0x10000 (the app partition of the default layout); bootloader at 0x1000"},
        {"esp32-s2", "ESP32-S2", "esp", "esp", 115200, "esp-classic", "esp32s2", 4u << 20, 0, 0x10000, "bootloader at 0x1000"},
        {"esp32-s3", "ESP32-S3", "esp", "esp", 115200, "esp-classic", "esp32s3", 8u << 20, 0, 0x10000,
         "bootloader at 0x0; on the native USB port (303a:1001) the reset is esp-usb-jtag"},
        {"esp32-c3", "ESP32-C3", "esp", "esp", 115200, "esp-classic", "esp32c3", 4u << 20, 0, 0x10000,
         "bootloader at 0x0; on the native USB port (303a:1001) the reset is esp-usb-jtag"},
        {"esp32-c6", "ESP32-C6", "esp", "esp", 115200, "esp-classic", "esp32c6", 4u << 20, 0, 0x10000, "bootloader at 0x0"},
        {"esp32-h2", "ESP32-H2", "esp", "esp", 115200, "esp-classic", "esp32h2", 4u << 20, 0, 0x10000, "bootloader at 0x0"},
        {"esp8266", "ESP8266 (NodeMCU, Wemos D1 mini, ESP-01)", "esp", "esp", 115200, "esp-classic", "esp8266", 4u << 20, 0, 0,
         "a raw .bin goes to 0x0; the ROM can't MD5, so verify is skipped; ESP-01 has 1 MB"},
        {"stm32", "STM32 via the USART system bootloader (AN3155)", "stm32", "stm32", 115200, "none", "stm32", 0, 0, 0x08000000,
         "USART1 (PA9 TX, PA10 RX) at 8E1; set BOOT0=1 and press reset first (or wire DTR->NRST, RTS->BOOT0 and use reset=dtr-boot0)"},
    };
    return list;
}

const Board* findBoard(const std::string& name) {
    const std::string n = lower(name);
    static const std::map<std::string, std::string> aliases = {
        {"arduino", "uno"},        {"uno-r3", "uno"},     {"nano-new", "nano"},      {"duemilanove", "pro-mini"}, {"promini", "pro-mini"},
        {"mega", "mega2560"},      {"esp32s2", "esp32-s2"}, {"esp32s3", "esp32-s3"}, {"esp32c3", "esp32-c3"},     {"esp32c6", "esp32-c6"},
        {"esp32h2", "esp32-h2"},   {"nodemcu", "esp8266"}, {"d1-mini", "esp8266"},   {"bluepill", "stm32"},       {"blue-pill", "stm32"},
        {"stm32f1", "stm32"},      {"stm32f4", "stm32"},  {"nucleo", "stm32"}};
    std::string key = n;
    if (auto it = aliases.find(n); it != aliases.end()) key = it->second;
    for (const auto& b : boards()) {
        if (b.name == key) return &b;
    }
    return nullptr;
}

const std::vector<std::string>& resetMethods() {
    static const std::vector<std::string> m = {"auto", "arduino", "1200bps", "esp-classic", "esp-usb-jtag", "dtr-boot0", "none"};
    return m;
}

const AvrChip* findAvrChip(const std::uint8_t sig[3]) {
    static const AvrChip chips[] = {
        {{0x1E, 0x95, 0x0F}, "ATmega328P", "m328p", 32768, 128},  {{0x1E, 0x95, 0x14}, "ATmega328", "m328", 32768, 128},
        {{0x1E, 0x95, 0x16}, "ATmega328PB", "m328pb", 32768, 128}, {{0x1E, 0x94, 0x06}, "ATmega168", "m168", 16384, 128},
        {{0x1E, 0x94, 0x0B}, "ATmega168P", "m168p", 16384, 128},  {{0x1E, 0x93, 0x07}, "ATmega8", "m8", 8192, 64},
        {{0x1E, 0x98, 0x01}, "ATmega2560", "m2560", 262144, 256}, {{0x1E, 0x97, 0x03}, "ATmega1280", "m1280", 131072, 256},
        {{0x1E, 0x95, 0x87}, "ATmega32U4", "m32u4", 32768, 128},  {{0x1E, 0x97, 0x05}, "ATmega1284P", "m1284p", 131072, 256},
        {{0x1E, 0x96, 0x0A}, "ATmega644P", "m644p", 65536, 256},
    };
    for (const auto& c : chips) {
        if (c.sig[0] == sig[0] && c.sig[1] == sig[1] && c.sig[2] == sig[2]) return &c;
    }
    return nullptr;
}

// ---- resets ---------------------------------------------------------------------------------

void resetArduino(SerialPort& port) {
    port.setDtr(false);
    port.setRts(false);
    port.pause(250);
    port.setDtr(true);
    port.setRts(true);
    port.pause(50);
    port.flushInput();
}

void resetEspClassic(SerialPort& port) {
    port.setDtr(false);  // IO0 high
    port.setRts(true);   // EN low: chip in reset
    port.pause(100);
    port.setDtr(true);   // IO0 low
    port.setRts(false);  // EN high: boots while IO0 is low -> download mode
    port.pause(50);
    port.setDtr(false);  // IO0 high again
}

void resetEspUsbJtag(SerialPort& port) {
    port.setRts(false);
    port.setDtr(false);
    port.pause(100);
    port.setDtr(true);
    port.setRts(false);
    port.pause(100);
    port.setRts(true);
    port.setDtr(false);
    port.setRts(true);
    port.pause(100);
    port.setDtr(false);
    port.setRts(false);
}

void resetEspHard(SerialPort& port) {
    port.setDtr(false);
    port.setRts(true);
    port.pause(100);
    port.setRts(false);
}

void resetStm32(SerialPort& port, bool bootloader) {
    port.setRts(bootloader);  // BOOT0
    port.setDtr(true);        // NRST low
    port.pause(100);
    port.setDtr(false);
    port.pause(100);
}

// ---- packet builders ------------------------------------------------------------------------

namespace stk500v1 {
Bytes getSync() { return {0x30, kEop}; }
Bytes getParameter(std::uint8_t param) { return {0x41, param, kEop}; }
Bytes readSignature() { return {0x75, kEop}; }
Bytes enterProgMode() { return {0x50, kEop}; }
Bytes leaveProgMode() { return {0x51, kEop}; }
Bytes loadAddress(std::uint32_t byteAddress) {
    const std::uint32_t w = byteAddress / 2;
    return {0x55, static_cast<std::uint8_t>(w & 0xFF), static_cast<std::uint8_t>((w >> 8) & 0xFF), kEop};
}
Bytes programPage(const Bytes& data, char memtype) {
    Bytes b = {0x64, static_cast<std::uint8_t>(data.size() >> 8), static_cast<std::uint8_t>(data.size() & 0xFF), static_cast<std::uint8_t>(memtype)};
    b.insert(b.end(), data.begin(), data.end());
    b.push_back(kEop);
    return b;
}
Bytes readPage(std::uint16_t size, char memtype) {
    return {0x74, static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size & 0xFF), static_cast<std::uint8_t>(memtype), kEop};
}
}  // namespace stk500v1

namespace stk500v2 {
Bytes frame(std::uint8_t seq, const Bytes& body) {
    Bytes f = {kStart, seq, static_cast<std::uint8_t>(body.size() >> 8), static_cast<std::uint8_t>(body.size() & 0xFF), kToken};
    f.insert(f.end(), body.begin(), body.end());
    f.push_back(xorBytes(f.data(), f.size()));
    return f;
}

std::optional<Bytes> unframe(Bytes& in, std::uint8_t* seq) {
    while (!in.empty()) {
        if (in[0] != kStart) {
            in.erase(in.begin());
            continue;
        }
        if (in.size() < 5) return std::nullopt;
        if (in[4] != kToken) {
            in.erase(in.begin());
            continue;
        }
        const std::size_t size = static_cast<std::size_t>((in[2] << 8) | in[3]);
        if (in.size() < 6 + size) return std::nullopt;
        const bool good = xorBytes(in.data(), 5 + size) == in[5 + size];
        Bytes body(in.begin() + 5, in.begin() + 5 + static_cast<std::ptrdiff_t>(size));
        if (seq) *seq = in[1];
        in.erase(in.begin(), in.begin() + 6 + static_cast<std::ptrdiff_t>(size));
        if (!good) throw ProtocolError("STK500v2 frame with a bad checksum");
        return body;
    }
    return std::nullopt;
}

Bytes loadAddressBody(std::uint32_t byteAddress, bool extended) {
    std::uint32_t w = byteAddress / 2;
    if (extended) w |= 0x80000000u;
    return {kLoadAddress, static_cast<std::uint8_t>(w >> 24), static_cast<std::uint8_t>((w >> 16) & 0xFF), static_cast<std::uint8_t>((w >> 8) & 0xFF),
            static_cast<std::uint8_t>(w & 0xFF)};
}

Bytes programFlashBody(const Bytes& data) {
    // mode 0xC1: page mode, write page; the wiring bootloader ignores the ISP timing fields
    Bytes b = {kProgramFlash, static_cast<std::uint8_t>(data.size() >> 8), static_cast<std::uint8_t>(data.size() & 0xFF), 0xC1, 10, 0x40, 0x4C, 0x20, 0x00, 0x00};
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

Bytes readFlashBody(std::uint16_t size) {
    return {kReadFlash, static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size & 0xFF), 0x20};
}
}  // namespace stk500v2

namespace avr109 {
Bytes setAddress(std::uint32_t byteAddress) {
    const std::uint32_t w = byteAddress / 2;
    return {'A', static_cast<std::uint8_t>((w >> 8) & 0xFF), static_cast<std::uint8_t>(w & 0xFF)};
}
Bytes blockWrite(const Bytes& data, char memtype) {
    Bytes b = {'B', static_cast<std::uint8_t>(data.size() >> 8), static_cast<std::uint8_t>(data.size() & 0xFF), static_cast<std::uint8_t>(memtype)};
    b.insert(b.end(), data.begin(), data.end());
    return b;
}
Bytes blockRead(std::uint16_t size, char memtype) {
    return {'g', static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size & 0xFF), static_cast<std::uint8_t>(memtype)};
}
}  // namespace avr109

// ---- clients ----------------------------------------------------------------------------------

namespace {

void fillChip(AvrInfo& info) {
    if (const AvrChip* c = findAvrChip(info.signature)) {
        info.chip = c->name;
        info.flashSize = c->flash;
        if (!info.pageSize) info.pageSize = c->page;
    } else {
        info.chip = "unknown (signature " + hexBytes(info.signature, 3) + ")";
    }
}

class Stk500v1Client : public AvrProgrammer {
public:
    Stk500v1Client(SerialPort& port, Log log) : port_(port), log_(std::move(log)) {}
    void connect(int attempts) override {
        for (int i = 0; i < attempts; ++i) {
            port_.write(stk500v1::getSync());
            auto a = port_.readByte(200);
            if (a && *a == stk500v1::kInSync) {
                auto b = port_.readByte(100);
                if (b && *b == stk500v1::kOk) {
                    port_.pause(10);
                    port_.flushInput();
                    // one more, cleanly, to be sure we're in step
                    txn(stk500v1::getSync(), 0);
                    return;
                }
            }
            port_.flushInput();
        }
        throw ProtocolError("no answer from an STK500v1 (optiboot) bootloader");
    }
    AvrInfo identify() override {
        AvrInfo info;
        info.protocol = "stk500v1";
        Bytes sig = txn(stk500v1::readSignature(), 3);
        std::copy(sig.begin(), sig.end(), info.signature);
        const int major = txn(stk500v1::getParameter(0x81), 1)[0];
        const int minor = txn(stk500v1::getParameter(0x82), 1)[0];
        info.bootloader = (major >= 4 ? "optiboot " : "STK500v1 bootloader ") + std::to_string(major) + "." + std::to_string(minor);
        fillChip(info);
        return info;
    }
    void enter() override { txn(stk500v1::enterProgMode(), 0); }
    void writePage(std::uint32_t address, const Bytes& data) override {
        if (address >= 0x20000) throw ProtocolError("STK500v1 can't address flash beyond 128 KB; use backend=avrdude");
        txn(stk500v1::loadAddress(address), 0);
        txn(stk500v1::programPage(data), 0, 2000);
    }
    Bytes readPage(std::uint32_t address, std::size_t size) override {
        txn(stk500v1::loadAddress(address), 0);
        return txn(stk500v1::readPage(static_cast<std::uint16_t>(size)), size, 2000);
    }
    void leave() override { txn(stk500v1::leaveProgMode(), 0); }

private:
    Bytes txn(const Bytes& cmd, std::size_t replyLen, int timeoutMs = 500) {
        port_.write(cmd);
        auto first = port_.readByte(timeoutMs);
        if (!first) throw ProtocolError("STK500v1: no answer to command 0x" + hexBytes(cmd.data(), 1));
        if (*first == stk500v1::kNoSync) throw ProtocolError("STK500v1: bootloader out of sync (wrong baud or board?)");
        if (*first != stk500v1::kInSync) throw ProtocolError("STK500v1: unexpected answer 0x" + hexBytes(&*first, 1));
        Bytes data = replyLen ? port_.readExact(replyLen, timeoutMs) : Bytes{};
        auto ok = port_.readByte(timeoutMs);
        if (!ok || *ok != stk500v1::kOk) throw ProtocolError("STK500v1: command 0x" + hexBytes(cmd.data(), 1) + " failed");
        return data;
    }
    SerialPort& port_;
    Log log_;
};

class Stk500v2Client : public AvrProgrammer {
public:
    Stk500v2Client(SerialPort& port, Log log) : port_(port), log_(std::move(log)) {}
    void connect(int attempts) override {
        for (int i = 0; i < attempts; ++i) {
            try {
                Bytes r = txn({stk500v2::kSignOn}, 300);
                std::string id(r.begin() + std::min<std::ptrdiff_t>(3, static_cast<std::ptrdiff_t>(r.size())), r.end());
                signOn_ = id;
                return;
            } catch (const std::exception&) {
                port_.flushInput();
            }
        }
        throw ProtocolError("no answer from an STK500v2 bootloader");
    }
    AvrInfo identify() override {
        AvrInfo info;
        info.protocol = "stk500v2";
        for (std::uint8_t i = 0; i < 3; ++i) {
            Bytes r = txn({stk500v2::kReadSignature, 0x04, 0x30, 0x00, i, 0x00});
            if (r.size() < 3) throw ProtocolError("STK500v2: short signature answer");
            info.signature[i] = r[2];
        }
        int major = 0, minor = 0;
        try {
            major = txn({stk500v2::kGetParameter, 0x91})[2];
            minor = txn({stk500v2::kGetParameter, 0x92})[2];
        } catch (const ProtocolError&) {
        }
        info.bootloader = (signOn_.empty() ? std::string("STK500v2") : signOn_) + " " + std::to_string(major) + "." + std::to_string(minor);
        fillChip(info);
        extended_ = info.flashSize > 65536;
        return info;
    }
    void enter() override { txn({stk500v2::kEnterProgmode, 200, 100, 25, 32, 0, 0x53, 3, 0xAC, 0x53, 0x00, 0x00}); }
    void writePage(std::uint32_t address, const Bytes& data) override {
        txn(stk500v2::loadAddressBody(address, extended_));
        txn(stk500v2::programFlashBody(data), 2000);
    }
    Bytes readPage(std::uint32_t address, std::size_t size) override {
        txn(stk500v2::loadAddressBody(address, extended_));
        Bytes r = txn(stk500v2::readFlashBody(static_cast<std::uint16_t>(size)), 2000);
        if (r.size() < 2 + size) throw ProtocolError("STK500v2: short read");
        return Bytes(r.begin() + 2, r.begin() + 2 + static_cast<std::ptrdiff_t>(size));
    }
    void leave() override { txn({stk500v2::kLeaveProgmode, 1, 1}); }

private:
    Bytes txn(const Bytes& body, int timeoutMs = 1000) {
        const std::uint8_t seq = seq_++;
        port_.write(stk500v2::frame(seq, body));
        Bytes buf;
        std::uint8_t got = 0;
        const int step = 50;
        for (int waited = 0; waited <= timeoutMs;) {
            std::uint8_t tmp[512];
            std::size_t n = port_.read(tmp, sizeof tmp, step);
            if (n == 0) {
                waited += step;
                continue;
            }
            buf.insert(buf.end(), tmp, tmp + n);
            while (auto r = stk500v2::unframe(buf, &got)) {
                if (got != seq || r->empty() || (*r)[0] != body[0]) continue;
                if (r->size() < 2 || (*r)[1] != stk500v2::kStatusOk)
                    throw ProtocolError("STK500v2: command 0x" + hexBytes(body.data(), 1) + " failed (status 0x" +
                                        (r->size() > 1 ? hexBytes(r->data() + 1, 1) : std::string("??")) + ")");
                return *r;
            }
        }
        throw ProtocolError("STK500v2: no answer to command 0x" + hexBytes(body.data(), 1));
    }
    SerialPort& port_;
    Log log_;
    std::uint8_t seq_ = 1;
    bool extended_ = false;
    std::string signOn_;
};

class Avr109Client : public AvrProgrammer {
public:
    Avr109Client(SerialPort& port, Log log) : port_(port), log_(std::move(log)) {}
    void connect(int attempts) override {
        for (int i = 0; i < attempts; ++i) {
            port_.write(Bytes{0x1B});
            port_.pause(20);
            port_.flushInput();
            port_.write(Bytes{'S'});
            try {
                Bytes id = port_.readExact(7, 400);
                id_.assign(id.begin(), id.end());
                return;
            } catch (const SerialError&) {
                port_.flushInput();
            }
        }
        throw ProtocolError("no answer from an AVR109 (Caterina) bootloader");
    }
    AvrInfo identify() override {
        AvrInfo info;
        info.protocol = "avr109";
        port_.write(Bytes{'V'});
        Bytes v = port_.readExact(2, 500);
        info.bootloader = id_ + " " + std::string(1, static_cast<char>(v[0])) + "." + std::string(1, static_cast<char>(v[1]));
        port_.write(Bytes{'s'});
        Bytes s = port_.readExact(3, 500);
        info.signature[0] = s[2];
        info.signature[1] = s[1];
        info.signature[2] = s[0];
        port_.write(Bytes{'b'});
        Bytes b = port_.readExact(3, 500);
        if (b[0] == 'Y') info.pageSize = static_cast<std::uint32_t>((b[1] << 8) | b[2]);
        fillChip(info);
        return info;
    }
    void enter() override { cr({'P'}); }
    void erase() override { cr({'e'}, 5000); }
    void writePage(std::uint32_t address, const Bytes& data) override {
        cr(avr109::setAddress(address));
        cr(avr109::blockWrite(data), 2000);
    }
    Bytes readPage(std::uint32_t address, std::size_t size) override {
        cr(avr109::setAddress(address));
        port_.write(avr109::blockRead(static_cast<std::uint16_t>(size)));
        return port_.readExact(size, 2000);
    }
    void leave() override {
        cr({'L'});
        cr({'E'});
    }

private:
    void cr(const Bytes& cmd, int timeoutMs = 1000) {
        port_.write(cmd);
        auto r = port_.readByte(timeoutMs);
        if (!r) throw ProtocolError(std::string("AVR109: no answer to '") + static_cast<char>(cmd[0]) + "'");
        if (*r != '\r') throw ProtocolError(std::string("AVR109: '") + static_cast<char>(cmd[0]) + "' failed");
    }
    SerialPort& port_;
    Log log_;
    std::string id_;
};

}  // namespace

std::unique_ptr<AvrProgrammer> makeAvrProgrammer(const std::string& protocol, SerialPort& port, Log log) {
    if (protocol == "stk500v1") return std::make_unique<Stk500v1Client>(port, std::move(log));
    if (protocol == "stk500v2") return std::make_unique<Stk500v2Client>(port, std::move(log));
    if (protocol == "avr109") return std::make_unique<Avr109Client>(port, std::move(log));
    throw ProtocolError("not an AVR protocol: " + protocol);
}

AvrFlashResult flashAvr(AvrProgrammer& prog, const Firmware& fw, bool verify, std::uint32_t pageSizeHint, std::uint32_t flashLimit, Log log) {
    AvrFlashResult res;
    res.info = prog.identify();
    if (log) log("found " + res.info.chip + " (" + res.info.bootloader + ")");
    const std::uint32_t page = res.info.pageSize ? res.info.pageSize : pageSizeHint ? pageSizeHint : 128;
    const std::uint32_t limit = flashLimit ? flashLimit : res.info.flashSize;
    if (fw.segments.empty()) throw FirmwareError("the firmware has no data");
    if (limit && fw.highAddress() > limit) {
        throw FirmwareError("the firmware reaches " + hex32(fw.highAddress()) + " but only " + std::to_string(limit) +
                            " bytes are free for a sketch (the bootloader has the rest). A \"with_bootloader\" .hex can't be "
                            "uploaded through the bootloader: use the plain .hex");
    }
    std::map<std::uint32_t, Bytes> pages;
    for (const auto& s : fw.segments) {
        for (std::size_t i = 0; i < s.data.size(); ++i) {
            const std::uint32_t a = s.address + static_cast<std::uint32_t>(i);
            auto& p = pages[a - a % page];
            if (p.empty()) p.assign(page, 0xFF);
            p[a % page] = s.data[i];
        }
    }
    prog.enter();
    prog.erase();
    std::size_t done = 0, nextLog = pages.size() / 4;
    for (const auto& [addr, data] : pages) {
        prog.writePage(addr, data);
        ++done;
        if (log && pages.size() > 8 && done >= nextLog && done < pages.size()) {
            log("wrote " + std::to_string(done) + "/" + std::to_string(pages.size()) + " pages");
            nextLog += pages.size() / 4;
        }
    }
    res.pages = pages.size();
    res.bytes = fw.byteCount();
    if (verify) {
        for (const auto& [addr, data] : pages) {
            Bytes back = prog.readPage(addr, data.size());
            if (back != data) {
                std::size_t i = 0;
                while (i < data.size() && back[i] == data[i]) ++i;
                prog.leave();
                throw ProtocolError("verify failed at " + hex32(addr + static_cast<std::uint32_t>(i)) + ": wrote 0x" + hexBytes(&data[i], 1) +
                                    ", read 0x" + hexBytes(&back[i], 1));
            }
        }
        res.verified = true;
        if (log) log("verified " + std::to_string(pages.size()) + " pages by reading them back");
    }
    prog.leave();
    return res;
}

}  // namespace achclwss::flash
