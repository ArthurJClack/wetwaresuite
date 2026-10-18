// A small cross-platform serial port layer (termios on Linux/macOS, Win32
// CreateFile + DCB on Windows), port listing with USB VID:PID identification,
// and in-memory simulated boards ("sim:uno", "sim:esp32", ...) that speak the
// real bootloader protocols, so the whole flash flow runs without hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace achclwss::flash {

using Bytes = std::vector<std::uint8_t>;

enum class Parity { None, Even, Odd };

struct SerialConfig {
    int baud = 115200;
    Parity parity = Parity::None;
    int dataBits = 8;
    int stopBits = 1;
};

class SerialError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SerialPort {
public:
    virtual ~SerialPort() = default;
    virtual void configure(const SerialConfig& config) = 0;
    virtual SerialConfig config() const = 0;
    virtual void write(const std::uint8_t* data, std::size_t size) = 0;
    // Reads up to `max` bytes, waiting at most timeoutMs for the first one. Returns 0 on timeout.
    virtual std::size_t read(std::uint8_t* data, std::size_t max, int timeoutMs) = 0;
    // true = asserted (the pin goes low on a USB-serial adapter, which is what pulls RESET/EN/IO0 low).
    virtual void setDtr(bool on) = 0;
    virtual void setRts(bool on) = 0;
    virtual void flushInput() = 0;
    // Waits ms milliseconds. Simulated ports return at once so tests stay fast.
    virtual void pause(int ms);
    virtual std::string name() const = 0;

    void write(const Bytes& data) { write(data.data(), data.size()); }
    void write(const std::string& text) { write(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); }
    // Reads exactly n bytes or throws SerialError("timeout ...") after timeoutMs in total.
    Bytes readExact(std::size_t n, int timeoutMs);
    // Reads one byte; nullopt on timeout.
    std::optional<std::uint8_t> readByte(int timeoutMs);
};

// Opens a real port ("/dev/ttyUSB0", "COM3", "\\\\.\\COM12") or a simulated one ("sim:esp32").
// Throws SerialError with a readable reason (busy, permission, missing).
std::unique_ptr<SerialPort> openPort(const std::string& name, const SerialConfig& config = {});
std::unique_ptr<SerialPort> openNativePort(const std::string& name, const SerialConfig& config);
bool isSimPort(const std::string& name);

// A USB vendor/product pair and what it usually is.
struct UsbId {
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
};

struct KnownUsb {
    std::string chip;    // "CH340", "CP210x", "FTDI FT232R", "Arduino Uno R3", "ST-Link V2-1", ...
    std::string family;  // "usb-serial", "avr", "esp", "stm32", "debug-probe"
    std::string board;   // a board preset name when it implies one ("uno", "mega2560", "esp32-s3"), else ""
    std::string note;    // e.g. "SWD probe: not a serial bootloader"
};

// Identifies common boards and adapters by VID:PID; nullopt when unknown.
std::optional<KnownUsb> identifyUsb(UsbId id);

struct PortInfo {
    std::string name;          // what to pass as port=
    std::string description;   // OS product string or a guess from the name
    std::optional<UsbId> usb;  // when the OS tells us
    std::string manufacturer;
    std::string serial;
    std::string chip;    // from identifyUsb, or a guess from the name on macOS
    std::string family;
    std::string board;
    std::string note;
};

// Every serial port the OS lists: Linux /dev/ttyUSB*, ttyACM* (and other USB ttys) with VID:PID from
// sysfs; macOS /dev/cu.* (identified from the name only); Windows COM ports from the registry
// (VID:PID from the USB and FTDIBUS enum keys) with a QueryDosDevice fallback.
std::vector<PortInfo> listPorts();
// The same identification for one port name, used by detect.
PortInfo describePort(const std::string& name);

// Parses "1a86:7523" or "VID_1A86&PID_7523" (any case) from a string.
std::optional<UsbId> parseUsbId(const std::string& text);
std::string formatUsbId(UsbId id);

// ---- simulated boards ---------------------------------------------------------------

// A simulated board behind a "sim:<kind>" port. It sees every write, reset-line change and
// line setting, and answers like the real bootloader would. Devices are kept per name for the
// life of the process, so flashing sim:esp32 and then reading sim:esp32 with monitor works.
class SimDevice {
public:
    virtual ~SimDevice() = default;
    virtual void onConfig(const SerialConfig& config) { config_ = config; }
    virtual void onLines(bool dtr, bool rts) = 0;
    virtual void onWrite(const std::uint8_t* data, std::size_t size) = 0;
    virtual void onClose() {}
    // The host waited `ms` with nothing to read (bootloaders time out into the app).
    virtual void onIdle(int ms) { (void)ms; }
    virtual std::string kind() const = 0;
    // The firmware currently in flash, from its base address, trimmed of trailing 0xFF.
    virtual Bytes flashImage() const = 0;
    virtual std::uint32_t flashBase() const { return 0; }
    // Put the board straight into its bootloader (what pressing reset with BOOT0/IO0 held does).
    virtual void enterBootloader() = 0;
    bool inBootloader() const { return boot_; }

    Bytes out;  // bytes waiting for the host to read
    std::string received;  // bytes the running "app" got (for the send command)

protected:
    void emit(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    void emit(const std::string& s) { out.insert(out.end(), s.begin(), s.end()); }
    // What a sim app prints when it boots, so monitor shows it running.
    void bootBanner();
    SerialConfig config_;
    bool boot_ = false;
};

// Kinds: uno, nano, nano-old, pro-mini (STK500v1/optiboot), mega2560 (STK500v2), leonardo, micro
// (AVR109), esp32, esp32-s2, esp32-s3, esp32-c3, esp32-c6, esp8266 (ESP ROM), stm32 (AN3155).
std::shared_ptr<SimDevice> makeSim(const std::string& kind);
// The device behind "sim:<kind>[#tag]", created on first use and kept for the process.
std::shared_ptr<SimDevice> simDevice(const std::string& portName);
// Forgets every simulated device (tests).
void resetSims();
std::vector<std::string> simKinds();

}  // namespace achclwss::flash
