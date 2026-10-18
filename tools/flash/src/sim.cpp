// Simulated boards: each one answers like the real bootloader (optiboot,
// the Mega's wiring STK500v2 bootloader, Caterina, the Espressif ROM, the
// STM32 system bootloader), models the auto-reset circuit, and "runs" the
// firmware afterwards by printing a line about it, so flash, monitor and send
// can be exercised end to end without hardware.
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "wetwaresuite/flash/protocol.hpp"
#include "wetwaresuite/flash/serial.hpp"
#include "internal.hpp"

namespace wetwaresuite::flash {

namespace {

std::string lowerCopy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

Bytes trimFF(const Bytes& flash) {
    std::size_t n = flash.size();
    while (n > 0 && flash[n - 1] == 0xFF) --n;
    return Bytes(flash.begin(), flash.begin() + static_cast<std::ptrdiff_t>(n));
}

// Flash cells only go from 1 to 0 when written; erasing sets them back to 0xFF.
void programAnd(Bytes& flash, std::size_t at, const std::uint8_t* data, std::size_t n) {
    for (std::size_t i = 0; i < n && at + i < flash.size(); ++i) flash[at + i] = static_cast<std::uint8_t>(flash[at + i] & data[i]);
}

}  // namespace

void SimDevice::bootBanner() {
    Bytes img = flashImage();
    std::string line = "[sim " + kind() + "] ";
    if (img.empty()) {
        line += "no firmware (flash is blank)";
    } else {
        char buf[96];
        std::snprintf(buf, sizeof buf, "firmware running: %zu bytes at 0x%08x, crc32 0x%08x", img.size(), flashBase(), crc32(img.data(), img.size()));
        line += buf;
    }
    emit(line + "\r\n");
}

namespace {

// Bytes the app receives: kept, and each line is echoed back.
class AppEcho {
public:
    void feed(SimDevice& dev, std::string& received, const std::uint8_t* d, std::size_t n, Bytes& out) {
        for (std::size_t i = 0; i < n; ++i) {
            received += static_cast<char>(d[i]);
            if (d[i] == '\n' || d[i] == '\r') {
                if (!line_.empty()) {
                    std::string reply = "[sim " + dev.kind() + "] got: " + line_ + "\r\n";
                    out.insert(out.end(), reply.begin(), reply.end());
                }
                line_.clear();
            } else {
                line_ += static_cast<char>(d[i]);
            }
        }
    }

private:
    std::string line_;
};

// ---- AVR --------------------------------------------------------------------------------

class AvrSim : public SimDevice {
public:
    AvrSim(std::string kind, std::uint32_t flashSize, std::uint32_t bootSize, std::uint32_t page, std::array<std::uint8_t, 3> sig, int baud)
        : kind_(std::move(kind)), flash_(flashSize, 0xFF), appSize_(flashSize - bootSize), page_(page), sig_(sig), baud_(baud) {}
    std::string kind() const override { return kind_; }
    Bytes flashImage() const override { return trimFF(Bytes(flash_.begin(), flash_.begin() + appSize_)); }
    void enterBootloader() override {
        boot_ = true;
        idle_ = 0;
        in_.clear();
    }
    void onLines(bool dtr, bool rts) override {
        (void)rts;
        // the 100 nF cap turns DTR being asserted into a short low pulse on RESET
        if (dtr && !dtr_) reset();
        dtr_ = dtr;
    }
    void onWrite(const std::uint8_t* d, std::size_t n) override {
        if (!boot_) {
            echo_.feed(*this, received, d, n, out);
            return;
        }
        if (config_.baud != baud_) return;  // wrong baud: the bootloader sees garbage
        idle_ = 0;
        in_.insert(in_.end(), d, d + n);
        process();
    }
    void onIdle(int ms) override {
        if (pendingBanner_) {
            pendingBanner_ = false;
            bootBanner();  // a real board takes a moment to reset and print; the host reads it after
            return;
        }
        if (!boot_) return;
        idle_ += ms;
        if (idle_ > timeoutMs()) startApp();
    }

protected:
    virtual void reset() {
        enterBootloader();
    }
    virtual int timeoutMs() const { return 1000; }
    virtual void process() = 0;
    void startApp() {
        boot_ = false;
        in_.clear();
        pendingBanner_ = true;  // emitted on the next idle, after the protocol's last reply is read
    }
    void writeFlash(std::uint32_t addr, const Bytes& data, bool erasePage) {
        if (addr >= appSize_) return;  // the bootloader protects itself
        if (erasePage) {
            const std::uint32_t start = addr - addr % page_;
            for (std::uint32_t i = start; i < start + page_ && i < appSize_; ++i) flash_[i] = 0xFF;
        }
        programAnd(flash_, addr, data.data(), std::min<std::size_t>(data.size(), appSize_ - addr));
    }
    Bytes readFlash(std::uint32_t addr, std::size_t n) const {
        Bytes out(n, 0xFF);
        for (std::size_t i = 0; i < n && addr + i < flash_.size(); ++i) out[i] = flash_[addr + i];
        return out;
    }

    std::string kind_;
    Bytes flash_;
    std::uint32_t appSize_;
    std::uint32_t page_;
    std::array<std::uint8_t, 3> sig_;
    int baud_;
    Bytes in_;
    int idle_ = 0;
    bool dtr_ = false;
    bool pendingBanner_ = false;
    std::uint32_t address_ = 0;  // byte address
    AppEcho echo_;
};

class OptibootSim : public AvrSim {
public:
    using AvrSim::AvrSim;

protected:
    void process() override {
        using namespace stk500v1;
        while (!in_.empty()) {
            const std::uint8_t cmd = in_[0];
            std::size_t need = 2;
            switch (cmd) {
            case 0x41: need = 3; break;
            case 0x42: need = 22; break;
            case 0x45: need = 7; break;
            case 0x55: need = 4; break;
            case 0x56: need = 6; break;
            case 0x74: need = 5; break;
            case 0x64:
                if (in_.size() < 4) return;
                need = 5 + static_cast<std::size_t>((in_[1] << 8) | in_[2]);
                break;
            case 0x30:
            case 0x50:
            case 0x51:
            case 0x75: need = 2; break;
            default:
                if (cmd != kEop) {
                    in_.erase(in_.begin());  // noise before a command
                    continue;
                }
                need = 1;
            }
            if (in_.size() < need) return;
            Bytes c(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(need));
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(need));
            if (c.back() != kEop) {
                emit(Bytes{kNoSync});
                continue;
            }
            Bytes reply = {kInSync};
            bool leave = false;
            switch (cmd) {
            case 0x41: reply.push_back(static_cast<std::uint8_t>(c[1] == 0x81 ? 8 : c[1] == 0x82 ? 0 : c[1] == 0x80 ? 3 : 0x03)); break;
            case 0x55: address_ = static_cast<std::uint32_t>((c[1] | (c[2] << 8)) * 2); break;
            case 0x56: reply.push_back(0x00); break;
            case 0x64: writeFlash(address_, Bytes(c.begin() + 4, c.end() - 1), true); break;
            case 0x74: {
                Bytes d = readFlash(address_, static_cast<std::size_t>((c[1] << 8) | c[2]));
                reply.insert(reply.end(), d.begin(), d.end());
                break;
            }
            case 0x75: reply.insert(reply.end(), sig_.begin(), sig_.end()); break;
            case 0x51: leave = true; break;
            default: break;
            }
            reply.push_back(kOk);
            emit(reply);
            if (leave) startApp();  // optiboot: watchdog reset into the sketch
        }
    }
};

class WiringSim : public AvrSim {
public:
    using AvrSim::AvrSim;

protected:
    void process() override {
        using namespace stk500v2;
        while (true) {
            std::uint8_t seq = 0;
            std::optional<Bytes> body;
            try {
                body = unframe(in_, &seq);
            } catch (const ProtocolError&) {
                continue;  // bad checksum: the frame was dropped, no answer
            }
            if (!body) return;
            if (body->empty()) continue;
            Bytes& b = *body;
            Bytes reply = {b[0], kStatusOk};
            bool leave = false;
            switch (b[0]) {
            case kSignOn: {
                const std::string id = "AVRISP_2";
                reply.push_back(static_cast<std::uint8_t>(id.size()));
                reply.insert(reply.end(), id.begin(), id.end());
                break;
            }
            case kGetParameter: reply.push_back(static_cast<std::uint8_t>(b.size() > 1 && b[1] == 0x91 ? 2 : b.size() > 1 && b[1] == 0x92 ? 10 : 0x0F)); break;
            case kLoadAddress:
                if (b.size() < 5) {
                    reply[1] = 0xC0;
                    break;
                }
                address_ = ((static_cast<std::uint32_t>(b[1] & 0x7F) << 24) | (static_cast<std::uint32_t>(b[2]) << 16) |
                            (static_cast<std::uint32_t>(b[3]) << 8) | b[4])
                           << 1;
                break;
            case kEnterProgmode: break;
            case kChipErase:
                for (std::uint32_t i = 0; i < appSize_; ++i) flash_[i] = 0xFF;
                break;
            case kReadSignature: reply.push_back(static_cast<std::uint8_t>(b.size() > 4 && b[4] < 3 ? sig_[b[4]] : 0)); reply.push_back(kStatusOk); break;
            case kProgramFlash: {
                if (b.size() < 10) {
                    reply[1] = 0xC0;
                    break;
                }
                const std::size_t n = static_cast<std::size_t>((b[1] << 8) | b[2]);
                if (b.size() < 10 + n) {
                    reply[1] = 0xC0;
                    break;
                }
                writeFlash(address_, Bytes(b.begin() + 10, b.begin() + 10 + static_cast<std::ptrdiff_t>(n)), true);
                address_ += static_cast<std::uint32_t>(n);
                break;
            }
            case kReadFlash: {
                const std::size_t n = b.size() >= 3 ? static_cast<std::size_t>((b[1] << 8) | b[2]) : 0;
                Bytes d = readFlash(address_, n);
                address_ += static_cast<std::uint32_t>(n);
                reply.insert(reply.end(), d.begin(), d.end());
                reply.push_back(kStatusOk);
                break;
            }
            case kLeaveProgmode: leave = true; break;
            default: reply[1] = 0xC9; break;  // STATUS_CMD_UNKNOWN
            }
            emit(frame(seq, reply));
            if (leave) startApp();
        }
    }
};

class CaterinaSim : public AvrSim {
public:
    using AvrSim::AvrSim;
    void onLines(bool dtr, bool rts) override {
        (void)rts;
        // no auto-reset cap on native USB boards; dropping DTR at 1200 baud is the "touch"
        if (!dtr && dtr_ && config_.baud == 1200) enterBootloader();
        dtr_ = dtr;
    }
    void onClose() override {
        if (config_.baud == 1200) enterBootloader();
    }
    void onWrite(const std::uint8_t* d, std::size_t n) override {
        // the Caterina bootloader is USB CDC: the baud rate is virtual, so (unlike optiboot) it
        // never sees "wrong baud" garbage.
        if (!boot_) {
            echo_.feed(*this, received, d, n, out);
            return;
        }
        idle_ = 0;
        in_.insert(in_.end(), d, d + n);
        process();
    }

protected:
    int timeoutMs() const override { return 8000; }
    void process() override {
        while (!in_.empty()) {
            const char cmd = static_cast<char>(in_[0]);
            std::size_t need = 1;
            switch (cmd) {
            case 'A': need = 3; break;
            case 'T':
            case 'x':
            case 'y': need = 2; break;
            case 'g': need = 4; break;
            case 'B':
                if (in_.size() < 4) return;
                need = 4 + static_cast<std::size_t>((in_[1] << 8) | in_[2]);
                break;
            default: break;
            }
            if (in_.size() < need) return;
            Bytes c(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(need));
            in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(need));
            switch (cmd) {
            case 'S': emit(std::string("CATERIN")); break;
            case 'V': emit(std::string("10")); break;
            case 'v': emit(std::string("?")); break;
            case 'p': emit(std::string("S")); break;
            case 'a': emit(std::string("Y")); break;
            case 'b': emit(Bytes{'Y', static_cast<std::uint8_t>(page_ >> 8), static_cast<std::uint8_t>(page_ & 0xFF)}); break;
            case 't': emit(Bytes{0x44, 0x00}); break;
            case 's': emit(Bytes{sig_[2], sig_[1], sig_[0]}); break;
            case 'F':
            case 'N':
            case 'Q':
            case 'r': emit(Bytes{0xFF}); break;
            case 'A':
                address_ = static_cast<std::uint32_t>((c[1] << 8) | c[2]) * 2;
                emit(Bytes{'\r'});
                break;
            case 'e':
                for (std::uint32_t i = 0; i < appSize_; ++i) flash_[i] = 0xFF;
                emit(Bytes{'\r'});
                break;
            case 'B':
                if (c[3] == 'F') {
                    Bytes data(c.begin() + 4, c.end());
                    writeFlash(address_, data, true);
                    address_ += static_cast<std::uint32_t>(data.size());
                }
                emit(Bytes{'\r'});
                break;
            case 'g': {
                const std::size_t n = static_cast<std::size_t>((c[1] << 8) | c[2]);
                emit(readFlash(address_, n));
                address_ += static_cast<std::uint32_t>(n);
                break;
            }
            case 'T':
            case 'P':
            case 'L':
            case 'x':
            case 'y': emit(Bytes{'\r'}); break;
            case 'E':
                emit(Bytes{'\r'});
                startApp();
                return;
            case 0x1B: break;
            default: emit(std::string("?")); break;
            }
        }
    }
};

// ---- Espressif ROM ----------------------------------------------------------------------------

class EspSim : public SimDevice {
public:
    explicit EspSim(const esp::Chip& chip) : chip_(chip), flash_(4u << 20, 0xFF) {}
    std::string kind() const override { return chip_.name; }
    Bytes flashImage() const override { return trimFF(flash_); }
    void enterBootloader() override {
        boot_ = true;
        synced_ = false;
        decoder_.reset();
        emit(std::string("rst:0x1 (POWERON_RESET),boot:0x3 (DOWNLOAD_BOOT(UART0/UART1/SDIO_REI_REO_V2))\r\nwaiting for download\r\n"));
    }
    void onLines(bool dtr, bool rts) override {
        // classic two-transistor circuit: EN is low only for RTS without DTR, IO0 only for DTR without RTS
        const bool enLow = rts && !dtr;
        const bool io0Low = dtr && !rts;
        if (pending_) {
            pending_ = false;
            if (io0Low) {
                enterBootloader();
                enLow_ = enLow;
                return;
            }
            startApp();
        }
        if (enLow_ && !enLow) pending_ = true;  // EN rises slowly through its RC: IO0 is sampled a moment later
        enLow_ = enLow;
    }
    void onIdle(int ms) override {
        (void)ms;
        settle();
    }
    void onWrite(const std::uint8_t* d, std::size_t n) override {
        settle();
        if (enLow_) return;  // held in reset
        if (!boot_) {
            echo_.feed(*this, received, d, n, out);
            return;
        }
        if (synced_ && config_.baud != baud_) return;
        for (std::size_t i = 0; i < n; ++i) {
            if (auto pkt = decoder_.feed(d[i])) handle(*pkt);
        }
    }

private:
    void settle() {
        if (pending_) {
            pending_ = false;
            startApp();
        }
    }
    void startApp() {
        boot_ = false;
        emit(std::string("ets Jun  8 2016 00:22:57\r\n\r\nrst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)\r\n"));
        const std::uint32_t appAt = chip_.name == "esp8266" ? 0 : 0x10000;
        Bytes head(flash_.begin() + appAt, flash_.begin() + appAt + 4096);
        auto info = parseEspImage(head);
        char buf[160];
        if (head[0] == 0xE9 && info) {
            std::snprintf(buf, sizeof buf, "[sim %s] booted the image at 0x%x: %u segments, entry 0x%08x\r\n", chip_.name.c_str(), appAt,
                          static_cast<unsigned>(info->segmentCount), info->entry);
        } else if (head[0] == 0xE9) {
            std::snprintf(buf, sizeof buf, "[sim %s] booted the image at 0x%x\r\n", chip_.name.c_str(), appAt);
        } else {
            std::snprintf(buf, sizeof buf, "invalid header: 0x%02x%02x%02x%02x\r\n", head[3], head[2], head[1], head[0]);
        }
        emit(std::string(buf));
    }
    void respond(std::uint8_t op, std::uint32_t value, Bytes payload = {}, std::uint8_t status = 0, std::uint8_t error = 0) {
        Bytes r = {0x01, op};
        payload.push_back(status);
        payload.push_back(error);
        if (chip_.statusBytes == 4) {
            payload.push_back(0);
            payload.push_back(0);
        }
        r.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
        r.push_back(static_cast<std::uint8_t>(payload.size() >> 8));
        put32le(r, value);
        r.insert(r.end(), payload.begin(), payload.end());
        emit(slip::encode(r));
    }
    void fail(std::uint8_t op, std::uint8_t error) { respond(op, 0, {}, 1, error); }
    void handle(const Bytes& p) {
        if (p.size() < 8 || p[0] != 0x00) return;
        const std::uint8_t op = p[1];
        const std::size_t size = static_cast<std::size_t>(p[2] | (p[3] << 8));
        const std::uint32_t chk = get32le(p, 4);
        if (p.size() != 8 + size) return fail(op, 0x05);
        Bytes d(p.begin() + 8, p.end());
        if (op == esp::kSync) {
            if (d != esp::syncPayload()) return;
            synced_ = true;
            baud_ = config_.baud;  // the ROM measures the baud from the sync pattern
            for (int i = 0; i < 8; ++i) respond(op, 0x20120707);
            return;
        }
        if (!synced_) return;
        switch (op) {
        case esp::kReadReg: {
            if (d.size() != 4) return fail(op, 0x05);
            const std::uint32_t addr = get32le(d, 0);
            respond(op, addr == esp::kChipDetectReg ? chip_.magic.front() : 0);
            return;
        }
        case esp::kWriteReg: respond(op, 0); return;
        case esp::kSpiAttach:
            if (!chip_.spiAttach) return fail(op, 0x05);
            if (d.size() != 8) return fail(op, 0x05);
            respond(op, 0);
            return;
        case esp::kSpiSetParams:
            if (d.size() != 24) return fail(op, 0x05);
            respond(op, 0);
            return;
        case esp::kChangeBaud:
            if (!chip_.changeBaud || d.size() != 8) return fail(op, 0x05);
            respond(op, 0);
            baud_ = static_cast<int>(get32le(d, 0));
            return;
        case esp::kFlashBegin: {
            if (d.size() != (chip_.encryptedArg ? 20u : 16u)) return fail(op, 0x05);
            const std::uint32_t blocks = get32le(d, 4), blockSize = get32le(d, 8), offset = get32le(d, 12);
            if (offset % esp::kSector != 0 || blockSize == 0 || static_cast<std::uint64_t>(offset) + std::uint64_t(blocks) * blockSize > flash_.size())
                return fail(op, 0x06);
            const std::uint64_t end = (static_cast<std::uint64_t>(offset) + std::uint64_t(blocks) * blockSize + esp::kSector - 1) / esp::kSector * esp::kSector;
            for (std::uint64_t i = offset; i < end && i < flash_.size(); ++i) flash_[static_cast<std::size_t>(i)] = 0xFF;
            flashOffset_ = offset;
            blockSize_ = blockSize;
            blocks_ = blocks;
            nextSeq_ = 0;
            respond(op, 0);
            return;
        }
        case esp::kFlashData: {
            if (d.size() < 16 || blockSize_ == 0) return fail(op, 0x05);
            const std::uint32_t n = get32le(d, 0), seq = get32le(d, 4);
            if (d.size() != 16 + n) return fail(op, 0x05);
            if (espChecksum(d.data() + 16, n) != (chk & 0xFF)) return fail(op, 0x07);  // checksum error
            if (seq != nextSeq_ || seq >= blocks_) return fail(op, 0x05);
            programAnd(flash_, flashOffset_ + seq * blockSize_, d.data() + 16, n);
            ++nextSeq_;
            respond(op, 0);
            return;
        }
        case esp::kFlashEnd: {
            respond(op, 0);
            if (d.size() >= 4 && get32le(d, 0) == 0) startApp();
            return;
        }
        case esp::kSpiFlashMd5: {
            if (!chip_.md5 || d.size() != 16) return fail(op, 0x05);
            const std::uint32_t addr = get32le(d, 0), n = get32le(d, 4);
            if (static_cast<std::uint64_t>(addr) + n > flash_.size()) return fail(op, 0x06);
            const std::string h = md5Hex(flash_.data() + addr, n);
            respond(op, 0, Bytes(h.begin(), h.end()));
            return;
        }
        default: fail(op, 0x05); return;
        }
    }

    esp::Chip chip_;
    Bytes flash_;
    slip::Decoder decoder_;
    bool synced_ = false;
    int baud_ = 115200;
    bool enLow_ = false;
    bool pending_ = false;
    std::uint32_t flashOffset_ = 0, blockSize_ = 0, blocks_ = 0, nextSeq_ = 0;
    AppEcho echo_;
};

// ---- STM32 ----------------------------------------------------------------------------------

class Stm32Sim : public SimDevice {
public:
    Stm32Sim(std::string kind, std::uint16_t pid, std::uint8_t version, bool extErase, std::uint32_t flashSize, std::uint32_t page)
        : kind_(std::move(kind)), pid_(pid), version_(version), extErase_(extErase), flash_(flashSize, 0xFF), page_(page) {
        boot_ = true;  // as if BOOT0 were held high when it powered up
    }
    std::string kind() const override { return kind_; }
    std::uint32_t flashBase() const override { return stm32::kFlashBase; }
    Bytes flashImage() const override { return trimFF(flash_); }
    void enterBootloader() override {
        boot_ = true;
        state_ = State::WaitInit;
        in_.clear();
    }
    void onLines(bool dtr, bool rts) override {
        // dtr-boot0 wiring: DTR asserted holds NRST low, RTS asserted drives BOOT0 high
        if (nrstLow_ && !dtr) {
            if (rts) {
                enterBootloader();
            } else {
                boot_ = false;
                state_ = State::App;
                bootBanner();
            }
        }
        nrstLow_ = dtr;
    }
    void onWrite(const std::uint8_t* d, std::size_t n) override {
        if (nrstLow_) return;
        if (state_ == State::App) {
            echo_.feed(*this, received, d, n, out);
            return;
        }
        // the bootloader runs 8E1; anything else reaches it as framing/parity errors
        if (config_.parity != Parity::Even) return;
        in_.insert(in_.end(), d, d + n);
        step();
    }

private:
    enum class State { WaitInit, Cmd, Addr, ReadCount, WriteData, Erase, ExtErase, App };
    static constexpr std::uint8_t ACK = stm32::kAck, NACK = stm32::kNack;

    std::vector<std::uint8_t> commands() const {
        return {0x00, 0x01, 0x02, 0x11, 0x21, 0x31, extErase_ ? std::uint8_t{0x44} : std::uint8_t{0x43}, 0x63, 0x73, 0x82, 0x92};
    }
    bool inFlash(std::uint32_t a) const { return a >= stm32::kFlashBase && a < stm32::kFlashBase + flash_.size(); }
    bool inRam(std::uint32_t a) const { return a >= 0x20000000u && a < 0x20000000u + ram_.size(); }
    void consume(std::size_t n) { in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(n)); }
    void eraseSector(std::size_t index) {
        // F1: uniform pages; F4: 16K x4, 64K, then 128K sectors
        std::size_t start = 0, size = page_;
        if (extErase_) {
            static const std::size_t kb[] = {16, 16, 16, 16, 64};
            start = 0;
            for (std::size_t i = 0; i < index; ++i) start += (i < 5 ? kb[i] : 128) * 1024;
            size = (index < 5 ? kb[index] : 128) * 1024;
        } else {
            start = index * page_;
        }
        for (std::size_t i = start; i < start + size && i < flash_.size(); ++i) flash_[i] = 0xFF;
    }
    void step() {
        while (!in_.empty()) {
            switch (state_) {
            case State::WaitInit:
                if (in_[0] == stm32::kInit) {
                    emit(Bytes{ACK});
                    state_ = State::Cmd;
                }
                consume(1);
                break;
            case State::Cmd: {
                if (in_[0] == stm32::kInit) {  // a second init byte after connecting
                    consume(1);
                    emit(Bytes{NACK});
                    break;
                }
                if (in_.size() < 2) return;
                const std::uint8_t c = in_[0], x = in_[1];
                consume(2);
                if (static_cast<std::uint8_t>(c ^ x) != 0xFF) {
                    emit(Bytes{NACK});
                    break;
                }
                cmd_ = c;
                switch (c) {
                case stm32::kGet: {
                    auto cmds = commands();
                    Bytes r = {ACK, static_cast<std::uint8_t>(cmds.size()), version_};
                    r.insert(r.end(), cmds.begin(), cmds.end());
                    r.push_back(ACK);
                    emit(r);
                    break;
                }
                case stm32::kGetVersion: emit(Bytes{ACK, version_, 0x00, 0x00, ACK}); break;
                case stm32::kGetId:
                    emit(Bytes{ACK, 0x01, static_cast<std::uint8_t>(pid_ >> 8), static_cast<std::uint8_t>(pid_ & 0xFF), ACK});
                    break;
                case stm32::kRead:
                case stm32::kGo:
                case stm32::kWrite:
                    emit(Bytes{ACK});
                    state_ = State::Addr;
                    break;
                case stm32::kErase:
                    if (extErase_) {
                        emit(Bytes{NACK});
                        break;
                    }
                    emit(Bytes{ACK});
                    state_ = State::Erase;
                    break;
                case stm32::kExtErase:
                    if (!extErase_) {
                        emit(Bytes{NACK});
                        break;
                    }
                    emit(Bytes{ACK});
                    state_ = State::ExtErase;
                    break;
                default: emit(Bytes{NACK}); break;
                }
                break;
            }
            case State::Addr: {
                if (in_.size() < 5) return;
                const std::uint32_t a = (static_cast<std::uint32_t>(in_[0]) << 24) | (static_cast<std::uint32_t>(in_[1]) << 16) |
                                        (static_cast<std::uint32_t>(in_[2]) << 8) | in_[3];
                const bool okSum = xorBytes(in_.data(), 4) == in_[4];
                consume(5);
                if (!okSum || !(inFlash(a) || inRam(a))) {
                    emit(Bytes{NACK});
                    state_ = State::Cmd;
                    break;
                }
                address_ = a;
                emit(Bytes{ACK});
                if (cmd_ == stm32::kRead) state_ = State::ReadCount;
                else if (cmd_ == stm32::kWrite) state_ = State::WriteData;
                else {
                    state_ = State::App;
                    boot_ = false;
                    bootBanner();
                    in_.clear();
                    return;
                }
                break;
            }
            case State::ReadCount: {
                if (in_.size() < 2) return;
                const std::uint8_t n = in_[0], x = in_[1];
                consume(2);
                state_ = State::Cmd;
                if (static_cast<std::uint8_t>(n ^ x) != 0xFF) {
                    emit(Bytes{NACK});
                    break;
                }
                Bytes r = {ACK};
                for (std::size_t i = 0; i <= n; ++i) r.push_back(byteAt(address_ + static_cast<std::uint32_t>(i)));
                emit(r);
                break;
            }
            case State::WriteData: {
                const std::size_t need = static_cast<std::size_t>(in_[0]) + 3;
                if (in_.size() < need) return;
                const std::uint8_t sum = xorBytes(in_.data(), need - 1);
                const bool ok = sum == in_[need - 1] && (in_[0] + 1) % 4 == 0;
                if (ok) {
                    for (std::size_t i = 0; i <= in_[0]; ++i) writeAt(address_ + static_cast<std::uint32_t>(i), in_[1 + i]);
                }
                consume(need);
                emit(Bytes{ok ? ACK : NACK});
                state_ = State::Cmd;
                break;
            }
            case State::Erase: {
                if (in_[0] == 0xFF) {
                    if (in_.size() < 2) return;
                    const bool ok = in_[1] == 0x00;
                    consume(2);
                    if (ok) std::fill(flash_.begin(), flash_.end(), std::uint8_t{0xFF});
                    emit(Bytes{ok ? ACK : NACK});
                } else {
                    const std::size_t need = static_cast<std::size_t>(in_[0]) + 3;
                    if (in_.size() < need) return;
                    const bool ok = xorBytes(in_.data(), need - 1) == in_[need - 1];
                    if (ok) {
                        for (std::size_t i = 1; i < need - 1; ++i) eraseSector(in_[i]);
                    }
                    consume(need);
                    emit(Bytes{ok ? ACK : NACK});
                }
                state_ = State::Cmd;
                break;
            }
            case State::ExtErase: {
                if (in_.size() < 2) return;
                const unsigned n = static_cast<unsigned>((in_[0] << 8) | in_[1]);
                if (n >= 0xFFF0) {
                    if (in_.size() < 3) return;
                    const bool ok = xorBytes(in_.data(), 2) == in_[2];
                    consume(3);
                    if (ok) std::fill(flash_.begin(), flash_.end(), std::uint8_t{0xFF});
                    emit(Bytes{ok ? ACK : NACK});
                } else {
                    const std::size_t need = 2 + 2 * (static_cast<std::size_t>(n) + 1) + 1;
                    if (in_.size() < need) return;
                    const bool ok = xorBytes(in_.data(), need - 1) == in_[need - 1];
                    if (ok) {
                        for (std::size_t i = 2; i + 1 < need - 1; i += 2) eraseSector(static_cast<std::size_t>((in_[i] << 8) | in_[i + 1]));
                    }
                    consume(need);
                    emit(Bytes{ok ? ACK : NACK});
                }
                state_ = State::Cmd;
                break;
            }
            case State::App: return;
            }
        }
    }
    std::uint8_t byteAt(std::uint32_t a) const {
        if (inFlash(a)) return flash_[a - stm32::kFlashBase];
        if (inRam(a)) return ram_[a - 0x20000000u];
        return 0;
    }
    void writeAt(std::uint32_t a, std::uint8_t v) {
        if (inFlash(a)) flash_[a - stm32::kFlashBase] = static_cast<std::uint8_t>(flash_[a - stm32::kFlashBase] & v);
        else if (inRam(a)) ram_[a - 0x20000000u] = v;
    }

    std::string kind_;
    std::uint16_t pid_;
    std::uint8_t version_;
    bool extErase_;
    Bytes flash_;
    Bytes ram_ = Bytes(20 * 1024, 0);
    std::uint32_t page_;
    State state_ = State::WaitInit;
    std::uint8_t cmd_ = 0;
    std::uint32_t address_ = 0;
    Bytes in_;
    bool nrstLow_ = false;
    AppEcho echo_;
};

// ---- the port --------------------------------------------------------------------------------

class SimPort : public SerialPort {
public:
    SimPort(std::string name, std::shared_ptr<SimDevice> dev, const SerialConfig& c) : name_(std::move(name)), dev_(std::move(dev)) {
        configure(c);
    }
    ~SimPort() override { dev_->onClose(); }
    void configure(const SerialConfig& c) override {
        config_ = c;
        dev_->onConfig(c);
    }
    SerialConfig config() const override { return config_; }
    void write(const std::uint8_t* data, std::size_t size) override { dev_->onWrite(data, size); }
    std::size_t read(std::uint8_t* data, std::size_t max, int timeoutMs) override {
        int waited = 0;
        while (dev_->out.empty()) {
            if (waited >= timeoutMs) {
                if (timeoutMs <= 0) dev_->onIdle(0);
                return 0;
            }
            const int step = std::min(10, timeoutMs - waited);
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
            waited += step;
            dev_->onIdle(step);
        }
        const std::size_t n = std::min(max, dev_->out.size());
        std::copy(dev_->out.begin(), dev_->out.begin() + static_cast<std::ptrdiff_t>(n), data);
        dev_->out.erase(dev_->out.begin(), dev_->out.begin() + static_cast<std::ptrdiff_t>(n));
        return n;
    }
    void setDtr(bool on) override {
        dtr_ = on;
        dev_->onLines(dtr_, rts_);
    }
    void setRts(bool on) override {
        rts_ = on;
        dev_->onLines(dtr_, rts_);
    }
    void flushInput() override { dev_->out.clear(); }
    void pause(int ms) override { dev_->onIdle(ms); }
    std::string name() const override { return name_; }

private:
    std::string name_;
    std::shared_ptr<SimDevice> dev_;
    SerialConfig config_;
    bool dtr_ = false;
    bool rts_ = false;
};

std::mutex& simMutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<SimDevice>>& simTable() {
    static std::map<std::string, std::shared_ptr<SimDevice>> t;
    return t;
}

}  // namespace

std::vector<std::string> simKinds() {
    return {"uno", "nano", "nano-old", "pro-mini", "mega2560", "leonardo", "micro", "esp32", "esp32-s2", "esp32-s3", "esp32-c3", "esp32-c6",
            "esp8266", "stm32", "stm32f4"};
}

std::string simFamily(const std::string& kind) {
    if (kind.rfind("esp", 0) == 0) return "esp";
    if (kind.rfind("stm32", 0) == 0) return "stm32";
    for (const auto& k : simKinds()) {
        if (k == kind) return "avr";
    }
    return {};
}

std::shared_ptr<SimDevice> makeSim(const std::string& kindIn) {
    const std::string kind = lowerCopy(kindIn);
    if (kind == "uno" || kind == "nano") return std::make_shared<OptibootSim>(kind, 32768, 512, 128, std::array<std::uint8_t, 3>{0x1E, 0x95, 0x0F}, 115200);
    if (kind == "nano-old" || kind == "pro-mini")
        return std::make_shared<OptibootSim>(kind, 32768, 2048, 128, std::array<std::uint8_t, 3>{0x1E, 0x95, 0x0F}, 57600);
    if (kind == "mega2560") return std::make_shared<WiringSim>(kind, 262144, 8192, 256, std::array<std::uint8_t, 3>{0x1E, 0x98, 0x01}, 115200);
    if (kind == "leonardo" || kind == "micro")
        return std::make_shared<CaterinaSim>(kind, 32768, 4096, 128, std::array<std::uint8_t, 3>{0x1E, 0x95, 0x87}, 57600);
    if (kind == "stm32") return std::make_shared<Stm32Sim>(kind, 0x410, 0x22, false, 64 * 1024, 1024);
    if (kind == "stm32f4") return std::make_shared<Stm32Sim>(kind, 0x431, 0x31, true, 512 * 1024, 0);
    if (const esp::Chip* chip = esp::findChip(kind)) return std::make_shared<EspSim>(*chip);
    std::string all;
    for (const auto& k : simKinds()) all += (all.empty() ? "" : ", ") + k;
    throw SerialError("unknown simulated board '" + kindIn + "' (one of: " + all + ")");
}

std::shared_ptr<SimDevice> simDevice(const std::string& portName) {
    std::string key = lowerCopy(portName);
    if (key.rfind("sim:", 0) != 0) return nullptr;
    std::lock_guard<std::mutex> lock(simMutex());
    auto& t = simTable();
    auto it = t.find(key);
    if (it != t.end()) return it->second;
    std::string kind = key.substr(4);
    kind = kind.substr(0, kind.find('#'));
    auto dev = makeSim(kind);
    t[key] = dev;
    return dev;
}

void resetSims() {
    std::lock_guard<std::mutex> lock(simMutex());
    simTable().clear();
}

std::unique_ptr<SerialPort> openSimPort(const std::string& name, const SerialConfig& config) {
    return std::make_unique<SimPort>(name, simDevice(name), config);
}

}  // namespace wetwaresuite::flash
