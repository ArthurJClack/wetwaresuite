// The flash flow: resolve the board, open the port, run the right protocol.
// Also board detection and planning the external tools.
#include "wetwaresuite/flash/flash.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <thread>

#include "wetwaresuite/core/process.hpp"
#include "internal.hpp"

namespace wetwaresuite::flash {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string simKind(const std::string& port) {
    if (!isSimPort(port)) return {};
    std::string k = lower(port.substr(4));
    return k.substr(0, k.find('#'));
}

}  // namespace

std::vector<Part> parseParts(const std::string& text) {
    std::vector<Part> parts;
    std::string cur;
    auto flush = [&] {
        std::string item = trim(cur);
        cur.clear();
        if (item.empty()) return;
        const auto eq = item.find('=');
        if (eq == std::string::npos) throw FirmwareError("each part needs an address, e.g. 0x10000=app.bin; got '" + item + "'");
        parts.push_back({parseAddress(trim(item.substr(0, eq))), trim(item.substr(eq + 1))});
    };
    for (char c : text) {
        if (c == ',' || c == ';' || c == '\n') flush();
        else cur += c;
    }
    flush();
    std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) { return a.address < b.address; });
    return parts;
}

std::string unescape(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        const char c = text[++i];
        switch (c) {
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case '0': out += '\0'; break;
        case '\\': out += '\\'; break;
        case 'x': {
            std::string hex;
            while (hex.size() < 2 && i + 1 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1]))) hex += text[++i];
            out += static_cast<char>(hex.empty() ? 0 : std::stoi(hex, nullptr, 16));
            break;
        }
        default:
            out += '\\';
            out += c;
        }
    }
    return out;
}

std::string printable(const std::string& bytes) {
    std::string out;
    char buf[8];
    for (unsigned char c : bytes) {
        if (c == '\n' || c == '\r' || c == '\t' || (c >= 0x20 && c < 0x7F)) {
            out += static_cast<char>(c);
        } else {
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        }
    }
    return out;
}

// ---- resolution --------------------------------------------------------------------------------

Resolved resolve(const FlashRequest& request, const PortInfo& port) {
    Resolved r;
    std::string boardName = lower(request.board);
    if (boardName.empty() || boardName == "auto") {
        if (!port.board.empty()) {
            boardName = port.board;
            r.why = "from the USB id " + (port.usb ? formatUsbId(*port.usb) + " (" + port.chip + ")" : port.chip);
        } else if (!simKind(port.name).empty()) {
            boardName = simKind(port.name);
            r.why = "from the simulated port";
        }
    } else {
        r.why = "you set board=" + request.board;
    }
    const Board* b = boardName.empty() ? nullptr : findBoard(boardName);
    const bool userBoard = !(request.board.empty() || lower(request.board) == "auto");
    if (!b && userBoard) {
        std::string names;
        for (const auto& x : boards()) names += (names.empty() ? "" : ", ") + x.name;
        throw ProtocolError("unknown board '" + request.board + "' (one of: " + names + ")");
    }
    if (b) {
        r.board = *b;
    } else {
        // make up a preset from the family we can tell, so the flow can still try
        r.board.name = boardName.empty() ? "unknown" : boardName;
        r.board.title = r.board.name;
        std::string fam = port.family;
        if (fam.empty() && !simKind(port.name).empty()) fam = simFamily(simKind(port.name));
        r.board.family = fam == "esp" ? "esp" : fam == "stm32" || fam == "debug-probe" ? "stm32" : fam == "avr" ? "avr" : "";
        r.board.protocol = r.board.family == "esp" ? "esp" : r.board.family == "stm32" ? "stm32" : r.board.family == "avr" ? "stk500v1" : "";
        r.board.baud = 115200;
        r.board.reset = r.board.family == "esp" ? "esp-classic" : r.board.family == "avr" ? "arduino" : "none";
        r.board.defaultAddress = r.board.family == "esp" ? 0x10000 : r.board.family == "stm32" ? 0x08000000 : 0;
        if (r.why.empty()) r.why = "guessed";
    }
    r.protocol = request.protocol.empty() ? r.board.protocol : lower(request.protocol);
    if (r.protocol.empty()) {
        throw ProtocolError("couldn't tell what kind of board is on " + port.name +
                            ". Pass board= (flash.boards lists them) or protocol= (stk500v1, stk500v2, avr109, esp, stm32)");
    }
    r.baud = request.baud ? request.baud : r.board.baud ? r.board.baud : 115200;
    r.reset = request.reset.empty() || request.reset == "auto" ? r.board.reset : lower(request.reset);
    // ESP native-USB ports use the JTAG reset by default
    if (r.protocol == "esp" && (request.reset.empty() || request.reset == "auto") && port.usb && port.usb->vid == 0x303A) r.reset = "esp-usb-jtag";
    if (r.reset.empty()) r.reset = r.protocol == "esp" ? "esp-classic" : r.protocol == "avr109" ? "1200bps" : r.protocol == "stm32" ? "none" : "arduino";
    return r;
}

// ---- native flashing --------------------------------------------------------------------------

namespace {

Firmware loadOne(const std::string& file, const Resolved& r, const FlashRequest& req) {
    const std::uint32_t addr = req.address ? *req.address : r.board.defaultAddress;
    Firmware fw = loadFirmware(file, addr);
    // ELF sanity: an AVR ELF on an ESP, etc.
    if (fw.format == "elf") {
        if (r.board.family == "avr" && fw.elfMachineId != 83)
            throw FirmwareError("this ELF is for " + fw.elfMachine + ", not AVR; wrong file for board " + r.board.name);
        if (r.board.family == "esp" && fw.elfMachineId != 94 && fw.elfMachineId != 243)
            throw FirmwareError("this ELF is for " + fw.elfMachine + ", not an ESP (Xtensa/RISC-V). Flash the .bin esptool/idf builds, not the .elf");
        if (r.board.family == "stm32" && fw.elfMachineId != 40)
            throw FirmwareError("this ELF is for " + fw.elfMachine + ", not ARM; wrong file for an STM32");
    }
    return fw;
}

FlashReport flashAvrFlow(SerialPort& port, const FlashRequest& req, const Resolved& r, Log log) {
    FlashReport rep;
    rep.board = r.board.name;
    rep.protocol = r.protocol;
    Firmware fw = loadOne(req.file, r, req);
    // Enter the bootloader. Real avr109 (Caterina) hardware is already touched and re-opened by
    // flashPort; a simulated avr109 board is touched here. STK500 boards get the DTR/RTS auto-reset.
    if (r.protocol == "avr109") {
        if (isSimPort(port.name())) {
            SerialConfig cfg = port.config();
            SerialConfig touch = cfg;
            touch.baud = 1200;
            port.configure(touch);
            port.setDtr(true);
            port.pause(5);
            port.setDtr(false);  // the 1->0 edge at 1200 baud is the reset "touch"
            port.pause(20);
            port.configure(cfg);
        }
    } else if (r.reset == "arduino") {
        resetArduino(port);
    }
    auto prog = makeAvrProgrammer(r.protocol, port, log);
    log("connecting (" + r.protocol + ") at " + std::to_string(r.baud) + " baud");
    try {
        prog->connect(10);
    } catch (const ProtocolError& e) {
        std::string hint = " at " + std::to_string(r.baud) + " baud.";
        if (r.protocol == "stk500v1")
            hint += r.baud == 115200 ? " Old-bootloader Nanos and Pro Minis run at 57600: try board=nano-old (or baud=57600)."
                                     : " Newer Unos/Nanos (optiboot) run at 115200: try board=uno (or baud=115200).";
        hint += " Check the port, and that nothing else (an IDE serial monitor) has it open; flash.detect probes for you.";
        throw ProtocolError(e.what() + hint);
    }
    AvrFlashResult res = flashAvr(*prog, fw, req.verify, r.board.pageSize, r.board.flashSize, log);
    rep.chip = res.info.chip;
    rep.bytes = res.bytes;
    rep.blocks = res.pages;
    rep.verified = res.verified;
    rep.verifyMethod = res.verified ? "read-back" : "";
    rep.regions.push_back(hex32(fw.lowAddress()) + " +" + std::to_string(fw.byteCount()));
    return rep;
}

FlashReport flashEspFlow(SerialPort& port, const FlashRequest& req, const Resolved& r, Log log) {
    FlashReport rep;
    rep.board = r.board.name;
    rep.protocol = "esp";
    // gather the regions to write
    std::vector<Part> parts = req.parts;
    if (parts.empty()) parts.push_back({req.address ? *req.address : r.board.defaultAddress, req.file});
    std::vector<std::pair<std::uint32_t, Bytes>> regions;
    for (const auto& p : parts) {
        Firmware fw = loadFirmware(p.path, p.address);
        if (fw.format == "elf")
            throw FirmwareError("ESP boards flash .bin images, not ELF. Point at the app.bin (and the bootloader/partition .bin) the build made");
        Bytes bytes = fw.flatten();
        if (fw.esp && p.address == r.board.defaultAddress && fw.espMerged)
            regions.emplace_back(fw.espMergedBootOffset, bytes);  // a merged image starts at the bootloader offset
        else
            regions.emplace_back(p.address, bytes);
        rep.regions.push_back(hex32(regions.back().first) + " +" + std::to_string(bytes.size()));
    }
    EspLoader loader(port, log);
    loader.connect(r.reset);
    const esp::Chip& chip = loader.detectChip();
    rep.chip = chip.name;
    if (r.board.family == "esp" && findBoard(r.board.name) && chip.name != r.board.mcu && lower(r.board.name).find(chip.name) == std::string::npos) {
        log("note: you said " + r.board.name + " but the chip reports " + chip.name);
    }
    loader.attachFlash(req.espFlashSize);
    if (req.flashBaud && req.flashBaud != r.baud) loader.changeBaud(req.flashBaud);
    std::size_t blocks = 0;
    for (const auto& [addr, bytes] : regions) {
        blocks += loader.writeFlash(addr, bytes);
        rep.bytes += bytes.size();
        if (req.verify && chip.md5) {
            std::string want = md5Hex(bytes.data(), bytes.size());
            std::string got = loader.flashMd5(addr, static_cast<std::uint32_t>(bytes.size()));
            if (want != got) throw ProtocolError("MD5 mismatch at " + hex32(addr) + ": flash has " + got + ", file is " + want);
            rep.verified = true;
            rep.verifyMethod = "md5";
            log("verified " + hex32(addr) + " by MD5");
        }
    }
    rep.blocks = blocks;
    if (req.verify && !chip.md5) log(chip.name + " ROM can't MD5, so the write isn't verified");
    loader.finish(req.run);
    if (req.run) {
        loader.hardReset();
        log("reset to run the new firmware");
    }
    return rep;
}

FlashReport flashStm32Flow(SerialPort& port, const FlashRequest& req, const Resolved& r, Log log) {
    FlashReport rep;
    rep.board = r.board.name;
    rep.protocol = "stm32";
    Firmware fw = loadOne(req.file, r, req);
    std::uint32_t base = req.address ? *req.address : fw.lowAddress();
    if (base == 0) base = stm32::kFlashBase;  // a raw .bin with no address goes to the start of flash
    Bytes bytes = fw.format == "bin" ? fw.segments.front().data : fw.flatten();
    if (fw.format != "bin") base = fw.lowAddress();
    if (base < stm32::kFlashBase || base >= stm32::kFlashBase + (64u << 20))
        throw FirmwareError("STM32 flash starts at 0x08000000; address " + hex32(base) + " is outside it (did you mean a raw .bin at 0x08000000?)");
    Stm32Loader loader(port, log);
    SerialConfig cfg = port.config();
    cfg.parity = Parity::Even;  // AN3155: 8E1
    port.configure(cfg);
    if (r.reset == "dtr-boot0") {
        log("resetting into the bootloader (DTR->NRST, RTS->BOOT0)");
        resetStm32(port, true);
    }
    loader.connect();
    Stm32Info info = loader.identify();
    rep.chip = info.part;
    log("bootloader v" + std::to_string(info.version >> 4) + "." + std::to_string(info.version & 0xF) + ", " + info.part);
    if (req.erase) loader.massErase();
    loader.write(base, bytes);
    rep.bytes = bytes.size();
    rep.blocks = (bytes.size() + 255) / 256;
    rep.regions.push_back(hex32(base) + " +" + std::to_string(bytes.size()));
    if (req.verify) {
        Bytes back = loader.read(base, bytes.size());
        std::size_t i = 0;
        while (i < bytes.size() && back[i] == bytes[i]) ++i;
        if (i < bytes.size()) throw ProtocolError("verify failed at " + hex32(base + static_cast<std::uint32_t>(i)));
        rep.verified = true;
        rep.verifyMethod = "read-back";
        log("verified by reading " + std::to_string(bytes.size()) + " bytes back");
    }
    if (req.run) {
        if (r.reset == "dtr-boot0") {
            resetStm32(port, false);
            log("reset with BOOT0 low to run the firmware");
        } else {
            loader.go(base);
            log("told the bootloader to jump to " + hex32(base));
        }
    }
    return rep;
}

}  // namespace

FlashReport flashNative(SerialPort& port, const FlashRequest& req, const Resolved& r) {
    FlashReport rep;
    std::vector<std::string>* logPtr = &rep.log;
    Log log = [logPtr](const std::string& m) { logPtr->push_back(m); };
    const auto start = std::chrono::steady_clock::now();
    FlashReport inner;
    if (r.board.family == "avr" || r.protocol == "stk500v1" || r.protocol == "stk500v2" || r.protocol == "avr109")
        inner = flashAvrFlow(port, req, r, log);
    else if (r.board.family == "esp" || r.protocol == "esp")
        inner = flashEspFlow(port, req, r, log);
    else if (r.board.family == "stm32" || r.protocol == "stm32")
        inner = flashStm32Flow(port, req, r, log);
    else
        throw ProtocolError("don't know how to flash protocol '" + r.protocol + "'");
    inner.log = std::move(rep.log);
    inner.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return inner;
}

FlashReport flashPort(const FlashRequest& req) {
    PortInfo info = describePort(req.port);
    Resolved r = resolve(req, info);
    SerialConfig cfg;
    cfg.baud = r.board.family == "avr" && r.protocol == "avr109" ? 115200 : r.baud;
    // Leonardo/Micro: open at 1200 baud to touch, which restarts into the bootloader (often a new port)
    if (r.reset == "1200bps" && !isSimPort(req.port)) {
        {
            SerialConfig touch;
            touch.baud = 1200;
            auto p = openPort(req.port, touch);
            p->setDtr(false);
            p->pause(20);
        }
        // the bootloader port may be the same name or a new one; wait for it to appear
        std::string bootPort = req.port;
        for (int i = 0; i < 40; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::error_code ec;
            if (fs::exists(req.port, ec)) {
                bootPort = req.port;
                break;
            }
            for (const auto& pi : listPorts()) {
                if (pi.board == "leonardo" || pi.board == "micro" || (pi.usb && pi.usb->vid == 0x2341)) bootPort = pi.name;
            }
        }
        cfg.baud = r.baud;
        auto port = openPort(bootPort, cfg);
        return flashNative(*port, req, r);
    }
    auto port = openPort(req.port, cfg);
    return flashNative(*port, req, r);
}

// ---- detection ---------------------------------------------------------------------------------

DetectReport detectBoard(const std::string& portName, const std::string& only, bool probe) {
    DetectReport rep;
    PortInfo info = describePort(portName);
    std::string fam = info.family;
    if (fam.empty() && !simKind(portName).empty()) fam = simFamily(simKind(portName));
    if (!info.board.empty()) {
        if (const Board* b = findBoard(info.board)) {
            rep.found = true;
            rep.board = b->name;
            rep.protocol = b->protocol;
            rep.baud = b->baud;
            rep.details = "from the USB id " + (info.usb ? formatUsbId(*info.usb) + " (" + info.chip + ")" : info.chip);
            rep.chip = b->mcu;
        }
    }
    if (fam == "debug-probe") {
        rep.details = info.chip + ": " + info.note;
        return rep;  // not a serial bootloader; don't poke it
    }
    if (!probe) {
        if (!rep.found && !fam.empty()) {
            rep.details = "USB chip is " + info.chip + " (" + fam + "); couldn't narrow the board without probing";
            rep.found = !fam.empty();
            rep.protocol = fam == "esp" ? "esp" : fam == "stm32" ? "stm32" : fam == "avr" ? "stk500v1" : "";
        }
        return rep;
    }
    // Probe each family's bootloader. `only` restricts which we try. We stop at the first that answers.
    auto tryAvr = [&](const std::string& proto, int baud) -> bool {
        rep.tried.push_back(proto + "@" + std::to_string(baud));
        try {
            SerialConfig cfg;
            cfg.baud = baud;
            auto port = openPort(portName, cfg);
            if (proto != "avr109") resetArduino(*port);
            auto prog = makeAvrProgrammer(proto, *port, nullptr);
            prog->connect(proto == "stk500v2" ? 3 : 5);
            AvrInfo ai = prog->identify();
            prog->leave();
            rep.found = true;
            rep.protocol = proto;
            rep.chip = ai.chip;
            rep.baud = baud;
            rep.details = ai.chip + " via " + ai.bootloader;
            if (rep.board.empty()) {
                if (ai.chip.find("328") != std::string::npos) rep.board = baud == 57600 ? "nano-old" : "uno";
                else if (ai.chip.find("2560") != std::string::npos) rep.board = "mega2560";
                else if (ai.chip.find("32U4") != std::string::npos) rep.board = "leonardo";
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    auto wants = [&](const std::string& f) { return only.empty() || only == "auto" || only == f; };
    if (wants("esp")) {
        rep.tried.push_back("esp-rom");
        try {
            SerialConfig cfg;
            cfg.baud = 115200;
            auto port = openPort(portName, cfg);
            EspLoader loader(*port, nullptr);
            loader.connect(info.usb && info.usb->vid == 0x303A ? "esp-usb-jtag" : "esp-classic", 2);
            const esp::Chip& chip = loader.detectChip();
            rep.found = true;
            rep.protocol = "esp";
            rep.chip = chip.name;
            rep.board = chip.name == "esp8266" ? "esp8266" : chip.name;
            rep.baud = 115200;
            rep.details = "ESP ROM bootloader, chip " + chip.name;
            loader.finish(true);
            loader.hardReset();
            return rep;
        } catch (const std::exception&) {
        }
    }
    if (wants("avr")) {
        if (tryAvr("stk500v1", 115200)) return rep;
        if (tryAvr("stk500v1", 57600)) return rep;
        if (tryAvr("stk500v2", 115200)) return rep;
    }
    if (wants("stm32")) {
        rep.tried.push_back("stm32-usart");
        try {
            SerialConfig cfg;
            cfg.baud = 115200;
            cfg.parity = Parity::Even;
            auto port = openPort(portName, cfg);
            Stm32Loader loader(*port, nullptr);
            loader.connect(2);
            Stm32Info si = loader.identify();
            rep.found = true;
            rep.protocol = "stm32";
            rep.chip = si.part;
            rep.board = "stm32";
            rep.baud = 115200;
            rep.details = si.part + ", bootloader v" + std::to_string(si.version >> 4) + "." + std::to_string(si.version & 0xF);
            return rep;
        } catch (const std::exception&) {
        }
    }
    if (!rep.found && rep.details.empty())
        rep.details = "nothing answered. The board may be running a sketch (press reset just before detecting), need its BOOT button held, "
                      "or use a reset method flash can't drive on this adapter";
    return rep;
}

// ---- external tools -----------------------------------------------------------------------------

std::optional<std::string> findBackendProgram(const std::string& backend) {
    if (backend == "esptool") {
        for (const char* n : {"esptool", "esptool.py"}) {
            if (auto p = findProgram(n)) return n;
        }
        return std::nullopt;
    }
    const std::string name = backend == "stm32prog" ? "STM32_Programmer_CLI" : backend;
    return findProgram(name) ? std::optional<std::string>(name) : std::nullopt;
}

std::vector<std::pair<std::string, std::string>> externalTools() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const char* b : {"avrdude", "esptool", "stm32prog", "st-flash", "dfu-util"}) {
        if (auto p = findBackendProgram(b)) out.emplace_back(b, *p);
    }
    return out;
}

ExternalPlan planExternal(const std::string& backend, const FlashRequest& req, const Resolved& r) {
    ExternalPlan plan;
    auto found = findBackendProgram(backend);
    plan.program = backend == "stm32prog" ? "STM32_Programmer_CLI" : backend == "esptool" ? "esptool" : backend;
    // Build the command line even when the tool isn't installed, so flash.command can show it; the
    // caller checks `error` (and findBackendProgram) before actually running it.
    std::optional<std::string> prog = found ? found : std::optional<std::string>(plan.program);
    if (!found) plan.error = backend + " isn't installed (not on PATH)";
    auto& a = plan.args;
    if (backend == "avrdude") {
        a = {*prog, "-p", r.board.mcu.empty() ? "m328p" : r.board.mcu, "-c", r.protocol == "stk500v2" ? "wiring" : r.protocol == "avr109" ? "avr109" : "arduino",
             "-P", req.port, "-b", std::to_string(r.baud), "-D", "-U", "flash:w:" + req.file + ":a"};
        if (!req.verify) a.push_back("-V");
    } else if (backend == "esptool") {
        a = {*prog, "--chip", r.board.mcu.empty() ? "auto" : r.board.mcu, "--port", req.port, "--baud", std::to_string(req.flashBaud ? req.flashBaud : r.baud),
             "write_flash"};
        if (!req.verify) {
        } else {
            a.push_back("--verify");
        }
        std::vector<Part> parts = req.parts;
        if (parts.empty()) parts.push_back({req.address ? *req.address : r.board.defaultAddress, req.file});
        for (const auto& p : parts) {
            a.push_back(hex32(p.address));
            a.push_back(p.path);
        }
    } else if (backend == "stm32prog") {
        a = {*prog, "-c", "port=" + req.port, "br=" + std::to_string(r.baud), "-w", req.file};
        if (req.address) a.push_back(hex32(*req.address));
        else a.push_back(hex32(stm32::kFlashBase));
        if (req.verify) a.push_back("-v");
        if (req.run) {
            a.push_back("-g");
            a.push_back(hex32(req.address ? *req.address : stm32::kFlashBase));
        }
    } else if (backend == "st-flash") {
        a = {*prog, "--reset", "write", req.file, hex32(req.address ? *req.address : stm32::kFlashBase)};
    } else if (backend == "dfu-util") {
        a = {*prog, "-a", "0", "-s", hex32(req.address ? *req.address : stm32::kFlashBase) + ":leave", "-D", req.file};
    } else {
        plan.error = "unknown backend '" + backend + "' (native, avrdude, esptool, stm32prog, st-flash, dfu-util)";
    }
    return plan;
}

}  // namespace wetwaresuite::flash
