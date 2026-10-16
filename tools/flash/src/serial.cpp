// Native serial ports (termios / Win32) and port listing.
#include "achclwss/flash/serial.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <thread>

#include "internal.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cwchar>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif
#endif

namespace achclwss::flash {

namespace fs = std::filesystem;

void SerialPort::pause(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

Bytes SerialPort::readExact(std::size_t n, int timeoutMs) {
    Bytes out(n);
    std::size_t got = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (got < n) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            throw SerialError("timeout: got " + std::to_string(got) + " of " + std::to_string(n) + " bytes");
        }
        got += read(out.data() + got, n - got, static_cast<int>(left));
    }
    return out;
}

std::optional<std::uint8_t> SerialPort::readByte(int timeoutMs) {
    std::uint8_t b = 0;
    if (read(&b, 1, timeoutMs) == 1) return b;
    return std::nullopt;
}

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string winError(DWORD code) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return "no such port (unplugged? flash.ports lists the ones present)";
    case ERROR_ACCESS_DENIED:
        return "the port is busy: another program has it open (close the Arduino IDE serial monitor, PuTTY, Cura...)";
    default: return "Windows error " + std::to_string(code);
    }
}

class WinPort : public SerialPort {
public:
    WinPort(const std::string& name, const SerialConfig& config) : name_(name) {
        std::string path = name;
        if (!startsWith(path, "\\\\.\\")) path = "\\\\.\\" + path;
        handle_ = CreateFileW(widen(path).c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) throw SerialError("can't open " + name + ": " + winError(GetLastError()));
        SetupComm(handle_, 1 << 16, 1 << 16);
        configure(config);
    }
    ~WinPort() override {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    void configure(const SerialConfig& c) override {
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(handle_, &dcb)) throw SerialError("can't read the settings of " + name_ + ": " + winError(GetLastError()));
        dcb.BaudRate = static_cast<DWORD>(c.baud);
        dcb.ByteSize = static_cast<BYTE>(c.dataBits);
        dcb.Parity = c.parity == Parity::Even ? EVENPARITY : c.parity == Parity::Odd ? ODDPARITY : NOPARITY;
        dcb.fParity = c.parity == Parity::None ? FALSE : TRUE;
        dcb.StopBits = c.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
        dcb.fBinary = TRUE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        dcb.fDtrControl = dtr_ ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
        dcb.fRtsControl = rts_ ? RTS_CONTROL_ENABLE : RTS_CONTROL_DISABLE;
        if (!SetCommState(handle_, &dcb)) throw SerialError("can't set " + std::to_string(c.baud) + " baud on " + name_ + ": " + winError(GetLastError()));
        config_ = c;
        lastTimeout_ = -2;
    }
    SerialConfig config() const override { return config_; }
    void write(const std::uint8_t* data, std::size_t size) override {
        std::size_t done = 0;
        while (done < size) {
            DWORD wrote = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, 1 << 16));
            if (!WriteFile(handle_, data + done, chunk, &wrote, nullptr)) throw SerialError("write to " + name_ + " failed: " + winError(GetLastError()));
            if (wrote == 0) throw SerialError("write to " + name_ + " timed out");
            done += wrote;
        }
    }
    std::size_t read(std::uint8_t* data, std::size_t max, int timeoutMs) override {
        setTimeouts(std::max(1, timeoutMs));
        DWORD got = 0;
        if (!ReadFile(handle_, data, static_cast<DWORD>(std::min<std::size_t>(max, 1 << 16)), &got, nullptr)) {
            throw SerialError("read from " + name_ + " failed: " + winError(GetLastError()));
        }
        return got;
    }
    void setDtr(bool on) override {
        dtr_ = on;
        EscapeCommFunction(handle_, on ? SETDTR : CLRDTR);
    }
    void setRts(bool on) override {
        rts_ = on;
        EscapeCommFunction(handle_, on ? SETRTS : CLRRTS);
    }
    void flushInput() override { PurgeComm(handle_, PURGE_RXCLEAR | PURGE_RXABORT); }
    std::string name() const override { return name_; }

private:
    void setTimeouts(int readMs) {
        if (readMs == lastTimeout_) return;
        COMMTIMEOUTS t{};
        // return as soon as any byte is there, or after readMs with none
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = readMs > 0 ? static_cast<DWORD>(readMs) : 1;
        t.WriteTotalTimeoutConstant = 5000;
        t.WriteTotalTimeoutMultiplier = 1;
        SetCommTimeouts(handle_, &t);
        lastTimeout_ = readMs;
    }
    std::string name_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    SerialConfig config_;
    bool dtr_ = true;
    bool rts_ = true;
    int lastTimeout_ = -2;
};

#else

speed_t speedFor(int baud) {
    switch (baud) {
    case 1200: return B1200;
    case 2400: return B2400;
    case 4800: return B4800;
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
#ifdef B460800
    case 460800: return B460800;
#endif
#ifdef B500000
    case 500000: return B500000;
#endif
#ifdef B576000
    case 576000: return B576000;
#endif
#ifdef B921600
    case 921600: return B921600;
#endif
#ifdef B1000000
    case 1000000: return B1000000;
#endif
#ifdef B1500000
    case 1500000: return B1500000;
#endif
#ifdef B2000000
    case 2000000: return B2000000;
#endif
    default: return 0;
    }
}

std::string errnoText(int e) {
    if (e == EBUSY) return "the port is busy: another program has it open (close the serial monitor, screen, minicom...)";
    if (e == EACCES)
        return std::string("permission denied: on Linux add yourself to the dialout (Debian/Ubuntu) or uucp (Arch) group, then log out and in");
    if (e == ENOENT) return "no such port (unplugged? flash.ports lists the ones present)";
    return std::strerror(e);
}

class PosixPort : public SerialPort {
public:
    PosixPort(const std::string& name, const SerialConfig& config) : name_(name) {
        fd_ = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd_ < 0) throw SerialError("can't open " + name + ": " + errnoText(errno));
        if (::ioctl(fd_, TIOCEXCL) != 0) {
            // not fatal: ptys and some drivers don't support exclusive mode
        }
        configure(config);
    }
    ~PosixPort() override {
        if (fd_ >= 0) {
            ::tcdrain(fd_);
            ::close(fd_);
        }
    }
    void configure(const SerialConfig& c) override {
        termios tio{};
        if (::tcgetattr(fd_, &tio) != 0) throw SerialError(name_ + " isn't a serial port: " + errnoText(errno));
        ::cfmakeraw(&tio);
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cflag &= ~static_cast<tcflag_t>(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
        tio.c_cflag |= c.dataBits == 7 ? CS7 : CS8;
        if (c.parity != Parity::None) tio.c_cflag |= PARENB;
        if (c.parity == Parity::Odd) tio.c_cflag |= PARODD;
        if (c.stopBits == 2) tio.c_cflag |= CSTOPB;
        tio.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY | INPCK | ISTRIP);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        const speed_t sp = speedFor(c.baud);
#ifdef __APPLE__
        ::cfsetispeed(&tio, sp ? sp : B9600);
        ::cfsetospeed(&tio, sp ? sp : B9600);
        if (::tcsetattr(fd_, TCSANOW, &tio) != 0) throw SerialError("can't configure " + name_ + ": " + errnoText(errno));
        if (!sp) {
            speed_t custom = static_cast<speed_t>(c.baud);
            if (::ioctl(fd_, IOSSIOSPEED, &custom) != 0) throw SerialError(std::to_string(c.baud) + " baud isn't supported on " + name_);
        }
#else
        if (!sp) throw SerialError(std::to_string(c.baud) + " baud isn't supported on this system; use a standard rate (9600 ... 921600)");
        ::cfsetispeed(&tio, sp);
        ::cfsetospeed(&tio, sp);
        if (::tcsetattr(fd_, TCSANOW, &tio) != 0) throw SerialError("can't configure " + name_ + ": " + errnoText(errno));
#endif
        config_ = c;
    }
    SerialConfig config() const override { return config_; }
    void write(const std::uint8_t* data, std::size_t size) override {
        std::size_t done = 0;
        while (done < size) {
            ssize_t n = ::write(fd_, data + done, size - done);
            if (n > 0) {
                done += static_cast<std::size_t>(n);
                continue;
            }
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                throw SerialError("write to " + name_ + " failed: " + errnoText(errno));
            pollfd p{fd_, POLLOUT, 0};
            if (::poll(&p, 1, 5000) == 0) throw SerialError("write to " + name_ + " timed out");
        }
    }
    std::size_t read(std::uint8_t* data, std::size_t max, int timeoutMs) override {
        pollfd p{fd_, POLLIN, 0};
        int r = ::poll(&p, 1, std::max(0, timeoutMs));
        if (r < 0) {
            if (errno == EINTR) return 0;
            throw SerialError("read from " + name_ + " failed: " + errnoText(errno));
        }
        if (r == 0) return 0;
        if (p.revents & (POLLERR | POLLNVAL)) throw SerialError(name_ + " went away (unplugged?)");
        ssize_t n = ::read(fd_, data, max);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
            throw SerialError("read from " + name_ + " failed: " + errnoText(errno));
        }
        if (n == 0 && (p.revents & POLLHUP)) {
            // a pty with no other end, or a USB device that was unplugged
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMs, 20)));
        }
        return static_cast<std::size_t>(n);
    }
    void setDtr(bool on) override { line(TIOCM_DTR, on); }
    void setRts(bool on) override { line(TIOCM_RTS, on); }
    void flushInput() override { ::tcflush(fd_, TCIFLUSH); }
    std::string name() const override { return name_; }

private:
    void line(int bit, bool on) {
        int bits = bit;
        // fails on ptys; the protocols still work, only auto-reset doesn't
        (void)::ioctl(fd_, on ? TIOCMBIS : TIOCMBIC, &bits);
    }
    std::string name_;
    int fd_ = -1;
    SerialConfig config_;
};

#endif

}  // namespace

std::unique_ptr<SerialPort> openNativePort(const std::string& name, const SerialConfig& config) {
#ifdef _WIN32
    return std::make_unique<WinPort>(name, config);
#else
    return std::make_unique<PosixPort>(name, config);
#endif
}

bool isSimPort(const std::string& name) { return startsWith(lower(name), "sim:"); }

std::unique_ptr<SerialPort> openPort(const std::string& name, const SerialConfig& config) {
    if (name.empty()) throw SerialError("no port given (flash.ports lists them; sim:uno, sim:esp32 or sim:stm32 are simulated boards)");
    if (isSimPort(name)) return openSimPort(name, config);
    return openNativePort(name, config);
}

// ---- USB identification ------------------------------------------------------------------

std::optional<UsbId> parseUsbId(const std::string& text) {
    const std::string t = lower(text);
    auto hex4 = [&](std::size_t at) -> std::optional<std::uint16_t> {
        if (at + 4 > t.size()) return std::nullopt;
        unsigned v = 0;
        for (std::size_t i = at; i < at + 4; ++i) {
            const char c = t[i];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) return std::nullopt;
            v = v * 16 + static_cast<unsigned>(d);
        }
        return static_cast<std::uint16_t>(v);
    };
    std::size_t v = t.find("vid_");
    std::size_t p = t.find("pid_");
    if (v != std::string::npos && p != std::string::npos) {
        auto a = hex4(v + 4), b = hex4(p + 4);
        if (a && b) return UsbId{*a, *b};
    }
    std::size_t colon = t.find(':');
    if (colon == 4) {
        auto a = hex4(0), b = hex4(5);
        if (a && b) return UsbId{*a, *b};
    }
    return std::nullopt;
}

std::string formatUsbId(UsbId id) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04x:%04x", id.vid, id.pid);
    return buf;
}

std::optional<KnownUsb> identifyUsb(UsbId id) {
    struct Row {
        std::uint16_t vid, pid;
        const char* chip;
        const char* family;
        const char* board;
        const char* note;
    };
    static const Row rows[] = {
        {0x1A86, 0x7523, "CH340", "usb-serial", "", "cheap USB-serial: Nano clones, many ESP32/ESP8266 boards"},
        {0x1A86, 0x5523, "CH341", "usb-serial", "", ""},
        {0x1A86, 0x55D4, "CH9102", "usb-serial", "", "used on newer ESP32 boards"},
        {0x1A86, 0x55D3, "CH343", "usb-serial", "", ""},
        {0x10C4, 0xEA60, "CP210x", "usb-serial", "", "Silicon Labs USB-serial: most ESP32 DevKits, NodeMCU"},
        {0x10C4, 0xEA70, "CP2105", "usb-serial", "", ""},
        {0x0403, 0x6001, "FTDI FT232R", "usb-serial", "", "FTDI adapters, older Arduinos (Duemilanove, Nano v3 originals)"},
        {0x0403, 0x6010, "FTDI FT2232", "usb-serial", "", ""},
        {0x0403, 0x6011, "FTDI FT4232", "usb-serial", "", ""},
        {0x0403, 0x6014, "FTDI FT232H", "usb-serial", "", ""},
        {0x0403, 0x6015, "FTDI FT-X", "usb-serial", "", ""},
        {0x067B, 0x2303, "Prolific PL2303", "usb-serial", "", ""},
        {0x2341, 0x0043, "Arduino Uno R3", "avr", "uno", ""},
        {0x2341, 0x0001, "Arduino Uno", "avr", "uno", ""},
        {0x2A03, 0x0043, "Arduino Uno R3 (arduino.org)", "avr", "uno", ""},
        {0x2341, 0x0010, "Arduino Mega 2560", "avr", "mega2560", ""},
        {0x2341, 0x0042, "Arduino Mega 2560 R3", "avr", "mega2560", ""},
        {0x2341, 0x8036, "Arduino Leonardo", "avr", "leonardo", "sketch running; flash does the 1200 baud reset"},
        {0x2341, 0x0036, "Arduino Leonardo (bootloader)", "avr", "leonardo", ""},
        {0x2341, 0x8037, "Arduino Micro", "avr", "micro", "sketch running; flash does the 1200 baud reset"},
        {0x2341, 0x0037, "Arduino Micro (bootloader)", "avr", "micro", ""},
        {0x303A, 0x1001, "Espressif USB-Serial-JTAG", "esp", "esp32-s3", "built-in USB of ESP32-S3/C3/C6/H2 (could be any of them)"},
        {0x303A, 0x0002, "Espressif ESP32-S2 native USB", "esp", "esp32-s2", ""},
        {0x0483, 0x3748, "ST-Link V2", "debug-probe", "stm32", "SWD probe, not a serial bootloader: use backend=st-flash or stm32prog"},
        {0x0483, 0x374B, "ST-Link V2-1 (Nucleo/Discovery)", "debug-probe", "stm32",
         "SWD probe with a virtual COM port; flash over SWD with backend=st-flash or stm32prog"},
        {0x0483, 0x3752, "ST-Link V2-1", "debug-probe", "stm32", "SWD probe: use backend=st-flash or stm32prog"},
        {0x0483, 0x374E, "ST-Link V3", "debug-probe", "stm32", "SWD probe: use backend=stm32prog"},
        {0x0483, 0x374F, "ST-Link V3", "debug-probe", "stm32", "SWD probe: use backend=stm32prog"},
        {0x0483, 0x3753, "ST-Link V3", "debug-probe", "stm32", "SWD probe: use backend=stm32prog"},
        {0x0483, 0xDF11, "STM32 DFU bootloader", "stm32", "stm32", "USB DFU, not serial: use backend=dfu-util or stm32prog"},
        {0x0483, 0x5740, "STM32 virtual COM port", "stm32", "", "an STM32 app's USB serial (not the bootloader)"},
        {0x1366, 0x0101, "SEGGER J-Link", "debug-probe", "", "SWD/JTAG probe: not supported here"},
        {0x1366, 0x1015, "SEGGER J-Link", "debug-probe", "", "SWD/JTAG probe: not supported here"},
    };
    for (const auto& r : rows) {
        if (r.vid == id.vid && r.pid == id.pid) return KnownUsb{r.chip, r.family, r.board, r.note};
    }
    if (id.vid == 0x2341 || id.vid == 0x2A03) return KnownUsb{"Arduino (unknown model)", "avr", "", ""};
    if (id.vid == 0x303A) return KnownUsb{"Espressif USB", "esp", "", ""};
    if (id.vid == 0x0483) return KnownUsb{"STMicroelectronics USB", "stm32", "", ""};
    return std::nullopt;
}

namespace {

void applyIdentity(PortInfo& p) {
    if (p.usb) {
        if (auto k = identifyUsb(*p.usb)) {
            p.chip = k->chip;
            p.family = k->family;
            p.board = k->board;
            p.note = k->note;
        }
    }
    if (p.chip.empty()) {
        // macOS (and USB info missing): guess from the driver's device name
        const std::string n = lower(p.name);
        if (n.find("wchusbserial") != std::string::npos || n.find("usbserial-1") != std::string::npos) {
            p.chip = "CH340 (guessed from the name)";
            p.family = "usb-serial";
        } else if (n.find("slab_usbtouart") != std::string::npos || n.find("usbserial-0001") != std::string::npos) {
            p.chip = "CP210x (guessed from the name)";
            p.family = "usb-serial";
        } else if (n.find("usbserial-a") != std::string::npos) {
            p.chip = "FTDI (guessed from the name)";
            p.family = "usb-serial";
        } else if (n.find("usbmodem") != std::string::npos) {
            p.chip = "USB CDC device (Arduino Uno/Mega/Leonardo, ESP32-S3/C3 native USB, ...)";
        }
    }
    if (p.description.empty()) p.description = p.chip;
}

#if !defined(_WIN32)
std::string readLine(const fs::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// Walks up from /sys/class/tty/<name>/device to the USB device with idVendor/idProduct.
void fillFromSysfs(PortInfo& p, const std::string& base) {
    std::error_code ec;
    fs::path dev = fs::canonical(fs::path("/sys/class/tty") / base / "device", ec);
    if (ec) return;
    for (fs::path at = dev; !at.empty() && at != at.root_path() && at.string().size() > 4; at = at.parent_path()) {
        if (fs::exists(at / "idVendor", ec)) {
            p.usb = parseUsbId(readLine(at / "idVendor") + ":" + readLine(at / "idProduct"));
            p.manufacturer = readLine(at / "manufacturer");
            p.description = readLine(at / "product");
            p.serial = readLine(at / "serial");
            return;
        }
    }
}
#endif

#ifdef _WIN32
std::wstring regString(HKEY key, const wchar_t* value) {
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
    std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
    DWORD got = static_cast<DWORD>(s.size() * sizeof(wchar_t));
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(s.data()), &got) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

std::vector<std::wstring> subkeys(HKEY key) {
    std::vector<std::wstring> out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(key, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.emplace_back(name, len);
    }
    return out;
}

struct WinUsbPort {
    UsbId id;
    std::string description, manufacturer, serial;
};

// HKLM\SYSTEM\CurrentControlSet\Enum\{USB,FTDIBUS}\<VID_xxxx&PID_xxxx>\<instance>\Device Parameters\PortName
void scanEnum(const wchar_t* bus, std::map<std::string, WinUsbPort>& out) {
    std::wstring root = std::wstring(L"SYSTEM\\CurrentControlSet\\Enum\\") + bus;
    HKEY rootKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root.c_str(), 0, KEY_READ, &rootKey) != ERROR_SUCCESS) return;
    for (const auto& dev : subkeys(rootKey)) {
        auto id = parseUsbId(narrow(dev));
        if (!id) continue;
        HKEY devKey;
        if (RegOpenKeyExW(rootKey, dev.c_str(), 0, KEY_READ, &devKey) != ERROR_SUCCESS) continue;
        for (const auto& inst : subkeys(devKey)) {
            HKEY instKey;
            if (RegOpenKeyExW(devKey, inst.c_str(), 0, KEY_READ, &instKey) != ERROR_SUCCESS) continue;
            HKEY params;
            if (RegOpenKeyExW(instKey, L"Device Parameters", 0, KEY_READ, &params) == ERROR_SUCCESS) {
                std::string port = narrow(regString(params, L"PortName"));
                if (!port.empty()) {
                    WinUsbPort w;
                    w.id = *id;
                    w.description = narrow(regString(instKey, L"FriendlyName"));
                    if (w.description.empty()) w.description = narrow(regString(instKey, L"DeviceDesc"));
                    const auto semi = w.description.rfind(';');  // "@oem12.inf,%desc%;USB-SERIAL CH340"
                    if (semi != std::string::npos) w.description = w.description.substr(semi + 1);
                    w.manufacturer = narrow(regString(instKey, L"Mfg"));
                    const auto msemi = w.manufacturer.rfind(';');
                    if (msemi != std::string::npos) w.manufacturer = w.manufacturer.substr(msemi + 1);
                    if (std::wstring(bus) == L"USB" && inst.find(L'&') == std::wstring::npos) w.serial = narrow(inst);
                    out[lower(port)] = w;
                }
                RegCloseKey(params);
            }
            RegCloseKey(instKey);
        }
        RegCloseKey(devKey);
    }
    RegCloseKey(rootKey);
}

std::vector<std::string> windowsComPorts() {
    std::vector<std::string> names;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        for (DWORD i = 0;; ++i) {
            wchar_t valueName[256];
            DWORD nameLen = 256, type = 0;
            BYTE data[512];
            DWORD dataLen = sizeof(data);
            if (RegEnumValueW(key, i, valueName, &nameLen, nullptr, &type, data, &dataLen) != ERROR_SUCCESS) break;
            if (type != REG_SZ) continue;
            std::wstring w(reinterpret_cast<const wchar_t*>(data), dataLen / sizeof(wchar_t));
            w.resize(wcsnlen(w.c_str(), w.size()));
            if (!w.empty()) names.push_back(narrow(w));
        }
        RegCloseKey(key);
    }
    if (names.empty()) {
        // fallback: every DOS device name, keeping COMn
        std::vector<wchar_t> buf(1 << 16);
        DWORD n = QueryDosDeviceW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        for (DWORD i = 0; i < n && buf[i];) {
            std::wstring dev(&buf[i]);
            i += static_cast<DWORD>(dev.size()) + 1;
            if (dev.size() > 3 && dev.compare(0, 3, L"COM") == 0 && std::all_of(dev.begin() + 3, dev.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                names.push_back(narrow(dev));
        }
    }
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return a.size() < b.size();
        return a < b;
    });
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}
#endif

}  // namespace

std::vector<PortInfo> listPorts() {
    std::vector<PortInfo> out;
#ifdef _WIN32
    std::map<std::string, WinUsbPort> usb;
    scanEnum(L"USB", usb);
    scanEnum(L"FTDIBUS", usb);
    for (const auto& name : windowsComPorts()) {
        PortInfo p;
        p.name = name;
        auto it = usb.find(lower(name));
        if (it != usb.end()) {
            p.usb = it->second.id;
            p.description = it->second.description;
            p.manufacturer = it->second.manufacturer;
            p.serial = it->second.serial;
        }
        applyIdentity(p);
        out.push_back(std::move(p));
    }
#elif defined(__APPLE__)
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/dev", ec)) {
        const std::string n = e.path().filename().string();
        if (!startsWith(n, "cu.")) continue;
        if (n == "cu.Bluetooth-Incoming-Port" || n.find("debug-console") != std::string::npos) continue;
        PortInfo p;
        p.name = e.path().string();
        applyIdentity(p);
        out.push_back(std::move(p));
    }
#else
    std::error_code ec;
    std::vector<std::string> names;
    if (fs::exists("/sys/class/tty", ec)) {
        for (const auto& e : fs::directory_iterator("/sys/class/tty", ec)) {
            const std::string n = e.path().filename().string();
            if (startsWith(n, "ttyUSB") || startsWith(n, "ttyACM") || startsWith(n, "ttyAMA") || startsWith(n, "rfcomm")) {
                names.push_back(n);
                continue;
            }
            // other USB ttys (unusual drivers) still count when they hang off a USB device
            if (startsWith(n, "tty") && !startsWith(n, "ttyS") && fs::exists(e.path() / "device", ec)) {
                PortInfo probe;
                fillFromSysfs(probe, n);
                if (probe.usb) names.push_back(n);
            }
        }
    }
    if (names.empty()) {
        for (const auto& e : fs::directory_iterator("/dev", ec)) {
            const std::string n = e.path().filename().string();
            if (startsWith(n, "ttyUSB") || startsWith(n, "ttyACM")) names.push_back(n);
        }
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) {
        PortInfo p;
        p.name = "/dev/" + n;
        if (!fs::exists(p.name, ec)) continue;
        fillFromSysfs(p, n);
        applyIdentity(p);
        out.push_back(std::move(p));
    }
#endif
    return out;
}

PortInfo describePort(const std::string& name) {
    if (isSimPort(name)) {
        PortInfo p;
        p.name = name;
        p.description = "simulated board";
        p.chip = "simulator";
        std::string kind = lower(name.substr(4));
        kind = kind.substr(0, kind.find('#'));
        p.board = kind;
        if (const auto dev = simDevice(name)) p.family = simFamily(kind);
        return p;
    }
    for (auto& p : listPorts()) {
        if (lower(p.name) == lower(name)) return p;
#ifndef _WIN32
        std::error_code ec;
        if (fs::equivalent(p.name, name, ec)) return p;
#endif
    }
    PortInfo p;
    p.name = name;
    applyIdentity(p);
    return p;
}

}  // namespace achclwss::flash
