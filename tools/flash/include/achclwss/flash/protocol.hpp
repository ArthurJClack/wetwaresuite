// Bootloader protocols, implemented natively so no external tools are needed:
//   STK500v1  Arduino Uno/Nano/Pro Mini (optiboot, ATmegaBOOT)
//   STK500v2  Arduino Mega 2560 (wiring bootloader)
//   AVR109    Arduino Leonardo/Micro (Caterina), entered with a 1200 baud "touch"
//   ESP ROM   ESP32 family and ESP8266 serial ROM bootloader (SLIP framed)
//   AN3155    STM32 USART system bootloader (8E1, BOOT0 high)
// Packet builders are free functions so they can be unit tested byte for byte.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "achclwss/flash/firmware.hpp"
#include "achclwss/flash/serial.hpp"

namespace achclwss::flash {

class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

using Log = std::function<void(const std::string&)>;

// ---- board presets ------------------------------------------------------------------

struct Board {
    std::string name;      // "uno"
    std::string title;     // "Arduino Uno (optiboot)"
    std::string family;    // "avr", "esp", "stm32"
    std::string protocol;  // "stk500v1", "stk500v2", "avr109", "esp", "stm32"
    int baud = 115200;     // bootloader baud
    std::string reset;     // default reset method (see resetMethods())
    std::string mcu;       // avrdude -p / esptool --chip / STM32 family
    std::uint32_t flashSize = 0;      // bytes the firmware may use
    std::uint32_t pageSize = 0;       // AVR flash page, 0 when not paged
    std::uint32_t defaultAddress = 0;  // where a raw .bin goes
    std::string notes;
};

const std::vector<Board>& boards();
const Board* findBoard(const std::string& name);
// "arduino" (DTR/RTS pulse), "1200bps" (touch), "esp-classic", "esp-usb-jtag", "dtr-boot0", "none".
const std::vector<std::string>& resetMethods();

struct AvrChip {
    std::uint8_t sig[3];
    const char* name;      // "ATmega328P"
    const char* part;      // avrdude part id, "m328p"
    std::uint32_t flash;   // total flash bytes
    std::uint32_t page;    // page bytes
};
const AvrChip* findAvrChip(const std::uint8_t sig[3]);

// ---- reset sequences ----------------------------------------------------------------

// Arduino auto-reset: DTR/RTS off 250 ms, then on (the falling edge pulses RESET through the 100 nF cap).
void resetArduino(SerialPort& port);
// esptool's classic reset: EN low via RTS, then IO0 low via DTR while EN rises (two-transistor circuit).
void resetEspClassic(SerialPort& port);
// esptool's sequence for the ESP32-S3/C3/C6 built-in USB-Serial-JTAG (303a:1001).
void resetEspUsbJtag(SerialPort& port);
// Pulses EN via RTS to run the app.
void resetEspHard(SerialPort& port);
// STM32 with DTR wired to NRST and RTS to BOOT0 (asserted = BOOT0 high): reset into the bootloader or the app.
void resetStm32(SerialPort& port, bool bootloader);

// ---- AVR ----------------------------------------------------------------------------

struct AvrInfo {
    std::string protocol;
    std::uint8_t signature[3] = {0, 0, 0};
    std::string chip;         // "ATmega328P" or "unknown (1e 95 0f)"
    std::uint32_t flashSize = 0;
    std::uint32_t pageSize = 0;
    std::string bootloader;   // "optiboot 8.0", "CATERIN 1.0", "AVRISP_2"
};

// One AVR bootloader client. Addresses are byte addresses; implementations convert to words.
class AvrProgrammer {
public:
    virtual ~AvrProgrammer() = default;
    virtual void connect(int attempts) = 0;
    virtual AvrInfo identify() = 0;
    virtual void enter() = 0;
    virtual void erase() {}
    virtual void writePage(std::uint32_t address, const Bytes& data) = 0;
    virtual Bytes readPage(std::uint32_t address, std::size_t size) = 0;
    virtual void leave() = 0;
};

namespace stk500v1 {
constexpr std::uint8_t kOk = 0x10, kInSync = 0x14, kNoSync = 0x15, kEop = 0x20;
Bytes getSync();
Bytes getParameter(std::uint8_t param);
Bytes readSignature();
Bytes enterProgMode();
Bytes leaveProgMode();
Bytes loadAddress(std::uint32_t byteAddress);  // sends the word address, little endian
Bytes programPage(const Bytes& data, char memtype = 'F');
Bytes readPage(std::uint16_t size, char memtype = 'F');
}  // namespace stk500v1

namespace stk500v2 {
constexpr std::uint8_t kStart = 0x1B, kToken = 0x0E;
constexpr std::uint8_t kSignOn = 0x01, kGetParameter = 0x03, kLoadAddress = 0x06, kEnterProgmode = 0x10, kLeaveProgmode = 0x11,
                       kChipErase = 0x12, kProgramFlash = 0x13, kReadFlash = 0x14, kReadSignature = 0x1B;
constexpr std::uint8_t kStatusOk = 0x00;
// Wraps a body: 0x1B seq size_hi size_lo 0x0E body... xor-checksum.
Bytes frame(std::uint8_t seq, const Bytes& body);
// Parses one frame from the start of `in`; returns the body and erases the bytes used. nullopt = need more.
// Throws ProtocolError on a bad checksum.
std::optional<Bytes> unframe(Bytes& in, std::uint8_t* seq = nullptr);
Bytes loadAddressBody(std::uint32_t byteAddress, bool extended);
Bytes programFlashBody(const Bytes& data);
Bytes readFlashBody(std::uint16_t size);
}  // namespace stk500v2

namespace avr109 {
Bytes setAddress(std::uint32_t byteAddress);  // 'A' hi lo (word address)
Bytes blockWrite(const Bytes& data, char memtype = 'F');
Bytes blockRead(std::uint16_t size, char memtype = 'F');
}  // namespace avr109

std::unique_ptr<AvrProgrammer> makeAvrProgrammer(const std::string& protocol, SerialPort& port, Log log);

struct AvrFlashResult {
    AvrInfo info;
    std::size_t bytes = 0;
    std::size_t pages = 0;
    bool verified = false;
};
// Enter, write every page that holds data (0xFF padded), read back, leave (starts the sketch).
AvrFlashResult flashAvr(AvrProgrammer& prog, const Firmware& fw, bool verify, std::uint32_t pageSizeHint, std::uint32_t flashLimit, Log log);

// ---- Espressif ROM bootloader ----------------------------------------------------------

namespace slip {
constexpr std::uint8_t kEnd = 0xC0, kEsc = 0xDB, kEscEnd = 0xDC, kEscEsc = 0xDD;
Bytes encode(const Bytes& packet);
class Decoder {
public:
    // Feeds one byte; returns a whole packet when its closing 0xC0 arrives.
    std::optional<Bytes> feed(std::uint8_t byte);
    void reset() { inPacket_ = false; escape_ = false; buf_.clear(); }

private:
    bool inPacket_ = false;
    bool escape_ = false;
    Bytes buf_;
};
}  // namespace slip

namespace esp {
constexpr std::uint8_t kFlashBegin = 0x02, kFlashData = 0x03, kFlashEnd = 0x04, kMemBegin = 0x05, kMemEnd = 0x06, kMemData = 0x07,
                       kSync = 0x08, kWriteReg = 0x09, kReadReg = 0x0A, kSpiSetParams = 0x0B, kSpiAttach = 0x0D, kChangeBaud = 0x0F,
                       kSpiFlashMd5 = 0x13;
constexpr std::uint32_t kChipDetectReg = 0x40001000;
constexpr std::uint32_t kFlashBlock = 0x400;  // ROM loader block size
constexpr std::uint32_t kSector = 0x1000;

// Request: 0x00 op size(le16) checksum(le32) data. (Not SLIP encoded.)
Bytes request(std::uint8_t op, const Bytes& data, std::uint32_t checksum = 0);
Bytes syncPayload();
Bytes flashBeginPayload(std::uint32_t eraseSize, std::uint32_t blocks, std::uint32_t blockSize, std::uint32_t offset, bool encryptedArg);
Bytes flashDataPayload(const Bytes& block, std::uint32_t seq);  // header + data (data already padded)
Bytes md5Payload(std::uint32_t address, std::uint32_t size);

struct Response {
    std::uint8_t op = 0;
    std::uint32_t value = 0;
    Bytes data;
};
std::optional<Response> parseResponse(const Bytes& packet);

struct Chip {
    std::string name;            // "esp32", "esp32-s3", "esp8266"
    std::vector<std::uint32_t> magic;  // values of the chip detect register
    int statusBytes = 4;         // ROM status bytes at the end of each response
    bool encryptedArg = false;   // FLASH_BEGIN takes a 5th word
    bool md5 = true;             // ROM has SPI_FLASH_MD5
    bool changeBaud = true;
    bool spiAttach = true;
    std::uint32_t bootloaderOffset = 0x1000;
    std::uint16_t imageChipId = 0;
};
const std::vector<Chip>& chips();
const Chip* findChip(const std::string& name);
const Chip* chipFromMagic(std::uint32_t magic);
// What FLASH_BEGIN should be told to erase. The ESP8266 ROM erases about twice what it is told
// (a known ROM bug), so esptool's correction is applied there.
std::uint32_t eraseSize(const Chip& chip, std::uint32_t offset, std::uint32_t size);
}  // namespace esp

class EspLoader {
public:
    EspLoader(SerialPort& port, Log log) : port_(port), log_(std::move(log)) {}
    // Resets into the bootloader with `reset` and SYNCs; throws with a hint if the chip never answers.
    void connect(const std::string& reset, int attempts = 7);
    const esp::Chip& detectChip();
    const esp::Chip* chip() const { return chip_; }
    std::uint32_t readReg(std::uint32_t address);
    void attachFlash(std::uint32_t flashSize);
    void changeBaud(int baud);
    // Writes data at offset (erased first by FLASH_BEGIN); returns the number of blocks.
    std::size_t writeFlash(std::uint32_t offset, const Bytes& data);
    std::string flashMd5(std::uint32_t offset, std::uint32_t size);
    void finish(bool reboot);
    void hardReset() { resetEspHard(port_); }

    esp::Response command(std::uint8_t op, const Bytes& data, std::uint32_t checksum = 0, int timeoutMs = 3000, std::size_t payloadLen = 0);

private:
    bool syncOnce();
    std::optional<Bytes> readPacket(int timeoutMs);
    SerialPort& port_;
    Log log_;
    slip::Decoder decoder_;
    const esp::Chip* chip_ = nullptr;
};

// ---- STM32 USART bootloader (AN3155) --------------------------------------------------------

namespace stm32 {
constexpr std::uint8_t kInit = 0x7F, kAck = 0x79, kNack = 0x1F;
constexpr std::uint8_t kGet = 0x00, kGetVersion = 0x01, kGetId = 0x02, kRead = 0x11, kGo = 0x21, kWrite = 0x31, kErase = 0x43,
                       kExtErase = 0x44;
constexpr std::uint32_t kFlashBase = 0x08000000;
Bytes command(std::uint8_t cmd);        // cmd, ~cmd
Bytes address(std::uint32_t address);   // 4 bytes big endian + xor
Bytes writeBlock(const Bytes& data);    // N-1, data, xor(N-1, data)
Bytes readCount(std::size_t n);         // N-1, ~(N-1)
Bytes globalErase();                    // 0xFF 0x00
Bytes extendedMassErase();              // 0xFF 0xFF 0x00
struct Part {
    std::uint16_t pid;
    const char* name;
    std::uint32_t flashKb;  // largest variant
};
const Part* findPart(std::uint16_t pid);
}  // namespace stm32

struct Stm32Info {
    std::uint8_t version = 0;
    std::vector<std::uint8_t> commands;
    std::uint16_t pid = 0;
    std::string part;
    bool extendedErase = false;
};

class Stm32Loader {
public:
    Stm32Loader(SerialPort& port, Log log) : port_(port), log_(std::move(log)) {}
    void connect(int attempts = 4);
    Stm32Info identify();
    void massErase();
    void write(std::uint32_t address, const Bytes& data);  // in 256 byte blocks
    Bytes read(std::uint32_t address, std::size_t size);
    void go(std::uint32_t address);

private:
    void expectAck(const char* what, int timeoutMs = 1000);
    void send(const Bytes& b) { port_.write(b); }
    SerialPort& port_;
    Log log_;
    Stm32Info info_;
};

}  // namespace achclwss::flash
