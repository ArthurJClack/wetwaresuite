// flash.* commands. Registration is split into one function per section so no
// single function gets large enough to trip MSVC's limits.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <thread>

#include "achclwss/core/process.hpp"
#include "achclwss/flash/flash.hpp"
#include "achclwss/raster/codec.hpp"
#include "achclwss/raster/image.hpp"
#include "internal.hpp"

namespace achclwss::flash {

namespace {

bool has(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (std::size_t i = 0; i < v.size(); ++i) s += (i ? sep : "") + v[i];
    return s;
}

std::string boardList() {
    std::vector<std::string> names;
    for (const auto& b : boards()) names.push_back(b.name);
    return join(names);
}

// A simple memory-map strip so Claude can see where firmware lands over MCP.
Attachment memoryMap(const std::vector<Segment>& segs, std::uint32_t base, const std::string& caption) {
    const int w = 640, h = 90;
    raster::Image img(w, h, raster::Color{24, 26, 32, 255});
    if (!segs.empty()) {
        const std::uint32_t lo = base;
        std::uint32_t hi = base;
        for (const auto& s : segs) hi = std::max(hi, s.end());
        const double span = std::max<std::uint32_t>(1, hi - lo);
        const int top = 20, bot = 60;
        for (int x = 0; x < w; ++x) {
            img.set(x, top, {60, 64, 72, 255});
            img.set(x, bot, {60, 64, 72, 255});
        }
        for (const auto& s : segs) {
            const int x0 = static_cast<int>((s.address - lo) / span * (w - 1));
            const int x1 = static_cast<int>((s.end() - lo) / span * (w - 1));
            for (int x = std::min(x0, w - 1); x <= std::min(std::max(x0, x1), w - 1); ++x) {
                for (int y = top; y <= bot; ++y) img.set(x, y, {90, 170, 240, 255});
            }
        }
    }
    (void)caption;
    return {"image/png", raster::encodePng(img), "memory-map.png"};
}

ParamSpec portParam() {
    return {"port", ParamType::String,
            "serial port: /dev/ttyUSB0 or /dev/ttyACM0 (Linux), /dev/cu.usbserial-XXXX (macOS), COM3 (Windows); or a "
            "simulated board sim:uno, sim:nano, sim:mega2560, sim:leonardo, sim:esp32, sim:esp8266, sim:stm32 for testing without hardware",
            true};
}
ParamSpec boardParam() {
    // not a fixed choice list, so aliases (mega, nodemcu, bluepill, esp32s3...) work too
    return {"board", ParamType::String,
            "board preset: auto (from the USB id), " + boardList() + " (aliases such as mega, nodemcu, d1-mini, bluepill also work)", false, {},
            std::string("auto")};
}
ParamSpec backendParam() {
    return {"backend", ParamType::String,
            "native (built in, the default) or an external tool: avrdude, esptool, stm32prog (STM32_Programmer_CLI), st-flash or dfu-util "
            "(st-flash/dfu-util are for ST-Link/DFU, not the serial bootloader)",
            false, {"native", "avrdude", "esptool", "stm32prog", "st-flash", "dfu-util"}, std::string("native")};
}

FlashRequest requestFrom(const Params& p) {
    FlashRequest req;
    req.port = stringParam(p, "port");
    req.file = stringParam(p, "file");
    if (has(p, "parts")) req.parts = parseParts(stringParam(p, "parts"));
    req.board = stringParam(p, "board", "auto");
    req.protocol = stringParam(p, "protocol");
    req.baud = static_cast<int>(intParam(p, "baud", 0));
    req.flashBaud = static_cast<int>(intParam(p, "flash_baud", 0));
    req.reset = stringParam(p, "reset");
    if (has(p, "address")) req.address = parseAddress(stringParam(p, "address"));
    req.verify = boolParam(p, "verify", true);
    req.erase = boolParam(p, "erase", true);
    req.run = boolParam(p, "run", true);
    if (has(p, "flash_size")) req.espFlashSize = parseAddress(stringParam(p, "flash_size"));
    return req;
}

// SWD/DFU tools only ever talk to STM32s, so they don't need the board spelled out.
ExternalPlan externalPlanFor(const std::string& backend, FlashRequest req) {
    if ((backend == "st-flash" || backend == "dfu-util" || backend == "stm32prog") && (req.board.empty() || req.board == "auto") &&
        describePort(req.port).family.empty())
        req.board = "stm32";
    return planExternal(backend, req, resolve(req, describePort(req.port)));
}

Result reportResult(const FlashReport& rep, const FlashRequest& req) {
    std::string msg = "flashed " + std::to_string(rep.bytes) + " bytes to " + (rep.chip.empty() ? rep.board : rep.chip) + " on " + req.port;
    if (rep.verified) msg += ", verified (" + rep.verifyMethod + ")";
    Result r = Result::success(msg);
    r.with("board", rep.board).with("protocol", rep.protocol).with("backend", rep.backend);
    if (!rep.chip.empty()) r.with("chip", rep.chip);
    r.with("bytes", static_cast<std::int64_t>(rep.bytes)).with("blocks", static_cast<std::int64_t>(rep.blocks));
    r.with("verified", rep.verified);
    if (!rep.verifyMethod.empty()) r.with("verify", rep.verifyMethod);
    r.with("regions", join(rep.regions));
    char sec[32];
    std::snprintf(sec, sizeof sec, "%.2f", rep.seconds);
    r.with("seconds", std::string(sec));
    if (!rep.log.empty()) r.with("log", join(rep.log, "\n"));
    return r;
}

}  // namespace

static void registerPorts(Session& session);
static void registerInspect(Session& session);
static void registerFlashCmd(Session& session);
static void registerMonitor(Session& session);

// ---- ports / detect / boards -------------------------------------------------------------------

static void registerPorts(Session& session) {
    auto& reg = session.registry();

    reg.add({"flash.ports", "List Serial Ports",
             "List the serial ports on this machine, each with the USB chip or board it looks like (CH340, CP210x, FTDI, "
             "an Arduino, an ST-Link, an Espressif native USB...) when the OS tells us the VID:PID. Pass one of the names "
             "as port= to detect or flash. The simulated boards (sim:uno, sim:esp32, sim:stm32, ...) are always available "
             "for trying the whole flow without hardware.",
             {{"usb", ParamType::Bool, "only ports that hang off USB (skip built-in RS-232 and the like)", false, {}, false},
              {"include_sim", ParamType::Bool, "also list the simulated boards", false, {}, true}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    auto ports = listPorts();
                    const bool usbOnly = boolParam(p, "usb", false);
                    Result r = Result::success("");
                    int n = 0;
                    std::vector<std::string> lines;
                    for (const auto& pi : ports) {
                        if (usbOnly && !pi.usb) continue;
                        ++n;
                        std::string line = pi.name;
                        if (pi.usb) line += "  [" + formatUsbId(*pi.usb) + "]";
                        if (!pi.chip.empty()) line += "  " + pi.chip;
                        if (!pi.board.empty()) line += "  -> board " + pi.board;
                        if (!pi.description.empty() && pi.description != pi.chip) line += "  (" + pi.description + ")";
                        if (!pi.note.empty()) line += "  note: " + pi.note;
                        lines.push_back(line);
                        r.with(pi.name, pi.chip.empty() ? pi.description : pi.chip);
                    }
                    if (boolParam(p, "include_sim", true)) {
                        for (const auto& k : simKinds()) lines.push_back("sim:" + k + "  (simulated " + simFamily(k) + " board)");
                    }
                    r.message = n == 0 ? "no USB serial ports found" : std::to_string(n) + " serial port" + (n == 1 ? "" : "s");
                    r.message += "\n" + join(lines, "\n");
                    return r;
                });
            });

    reg.add({"flash.detect", "Detect Board",
             "Work out what is on a port: first from its USB id, then (unless probe=false) by trying each bootloader in "
             "turn (STK500v1 at 115200 and 57600, STK500v2, the ESP ROM, the STM32 USART bootloader). Reports the "
             "protocol, chip and a board preset to use. Probing resets the board; a board running a sketch may need its "
             "reset pressed first, and an ESP may need its BOOT button held.",
             {portParam(),
              {"probe", ParamType::Bool, "actually talk to the bootloaders (off: USB id only)", false, {}, true},
              {"only", ParamType::String, "limit probing to one family", false, {"auto", "avr", "esp", "stm32"}, std::string("auto")}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    DetectReport d = detectBoard(stringParam(p, "port"), stringParam(p, "only", "auto"), boolParam(p, "probe", true));
                    Result r = Result::success(d.found ? "detected " + (d.chip.empty() ? d.protocol : d.chip) : "couldn't identify the board");
                    r.with("found", d.found);
                    if (!d.protocol.empty()) r.with("protocol", d.protocol);
                    if (!d.chip.empty()) r.with("chip", d.chip);
                    if (!d.board.empty()) r.with("board", d.board);
                    if (d.baud) r.with("baud", static_cast<std::int64_t>(d.baud));
                    if (!d.details.empty()) r.with("details", d.details);
                    if (!d.tried.empty()) r.with("tried", join(d.tried));
                    if (d.found && !d.board.empty()) r.message += " - flash with board=" + d.board;
                    return r;
                });
            });

    reg.add({"flash.boards", "List Board Presets",
             "List the built-in board presets with their protocol, default bootloader baud, default reset method and "
             "notes, so you know what board= to pass and which settings it uses. Be explicit: the defaults matter.",
             {{"board", ParamType::String, "show just this one in full", false}}, false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    Result r = Result::success("");
                    std::vector<std::string> lines;
                    const std::string only = stringParam(p, "board");
                    for (const auto& b : boards()) {
                        if (!only.empty() && b.name != only && findBoard(only) != &b) continue;
                        std::string line = b.name + ": " + b.title + " | " + b.protocol + " | " + std::to_string(b.baud) + " baud | reset=" + b.reset;
                        if (b.defaultAddress) line += " | raw .bin -> " + hex32(b.defaultAddress);
                        if (!b.notes.empty()) line += "\n    " + b.notes;
                        lines.push_back(line);
                        r.with(b.name, b.protocol + " @" + std::to_string(b.baud) + " reset=" + b.reset);
                    }
                    if (lines.empty()) return Result::failure("no such board '" + only + "' (one of: " + boardList() + ")");
                    r.message = (only.empty() ? std::to_string(lines.size()) + " board presets" : std::string("board preset")) + "\n" + join(lines, "\n");
                    return r;
                });
            });

    reg.add({"flash.tools", "External Tools",
             "Show which external flashing tools are on PATH (avrdude, esptool, STM32_Programmer_CLI, st-flash, "
             "dfu-util). The native protocols need none of these; they are only used when you pass backend=.",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    auto tools = externalTools();
                    Result r = Result::success(tools.empty() ? "no external flashing tools found (the native backend needs none)"
                                                             : std::to_string(tools.size()) + " external tool(s) available");
                    for (const auto& [name, prog] : tools) r.with(name, prog);
                    return r;
                });
            });
}

// ---- inspect -----------------------------------------------------------------------------------

static void registerInspect(Session& session) {
    auto& reg = session.registry();
    reg.add({"flash.inspect", "Inspect Firmware",
             "Read a firmware file and say what it is without touching any hardware: Intel HEX, a raw .bin, an ELF (with "
             "its machine, entry point and loadable sections), or an Espressif image (.bin starting with 0xE9: chip, "
             "flash mode/size/frequency, segments, checksum). Shows the address ranges it would write and the total "
             "size, and with preview=true attaches a little memory-map picture. Use it to check a build before flashing.",
             {{"file", ParamType::String, "the firmware file to inspect", true},
              {"address", ParamType::String, "load address for a raw .bin (e.g. 0x10000, 0x08000000); hex and k/m suffixes allowed", false},
              {"hex", ParamType::Bool, "also show the first bytes as hex", false, {}, false},
              {"preview", ParamType::Bool, "attach a memory-map image", false, {}, false}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    const std::uint32_t addr = has(p, "address") ? parseAddress(stringParam(p, "address")) : 0;
                    Firmware fw;
                    try {
                        fw = loadFirmware(stringParam(p, "file"), addr);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    Result r = Result::success("");
                    r.with("format", fw.format).with("bytes", static_cast<std::int64_t>(fw.byteCount()));
                    r.with("low_address", hex32(fw.lowAddress())).with("high_address", hex32(fw.highAddress()));
                    std::vector<std::string> regions;
                    for (const auto& s : fw.segments) regions.push_back(hex32(s.address) + " +" + std::to_string(s.data.size()));
                    r.with("regions", join(regions));
                    std::string msg = fw.format + ", " + std::to_string(fw.byteCount()) + " bytes in " + std::to_string(fw.segments.size()) +
                                      " region(s), " + hex32(fw.lowAddress()) + ".." + hex32(fw.highAddress());
                    if (fw.entry) r.with("entry", hex32(*fw.entry));
                    if (fw.format == "elf") {
                        r.with("machine", fw.elfMachine);
                        msg += "; ELF for " + fw.elfMachine;
                        std::vector<std::string> secs;
                        for (const auto& s : fw.sections) {
                            if (s.alloc() && s.size) secs.push_back(s.name + " @" + hex32(s.loadAddress) + " +" + std::to_string(s.size));
                        }
                        r.with("sections", join(secs, "\n"));
                    }
                    if (fw.esp) {
                        const auto& e = *fw.esp;
                        r.with("esp_chip", e.chipName()).with("flash_mode", e.flashModeName()).with("flash_size", e.flashSizeName());
                        r.with("flash_freq", e.flashFreqName()).with("esp_segments", static_cast<std::int64_t>(e.segmentCount));
                        r.with("esp_entry", hex32(e.entry)).with("checksum_ok", e.checksumOk);
                        if (e.hashAppended) r.with("sha256_appended", true);
                        if (fw.espMerged) r.with("merged_image_at", hex32(fw.espMergedBootOffset));
                        msg += "; Espressif " + e.chipName() + " image, flash " + e.flashModeName() + " " + e.flashSizeName() + " @" + e.flashFreqName() +
                               ", " + std::to_string(e.segmentCount) + " segments, checksum " + (e.checksumOk ? "ok" : "BAD");
                    }
                    if (boolParam(p, "hex", false)) {
                        Bytes flat = fw.flatten();
                        std::string hx;
                        char buf[4];
                        for (std::size_t i = 0; i < std::min<std::size_t>(flat.size(), 64); ++i) {
                            std::snprintf(buf, sizeof buf, "%02x ", flat[i]);
                            hx += buf;
                        }
                        r.with("first_bytes", hx);
                    }
                    r.message = msg;
                    if (boolParam(p, "preview", false)) r.attach(memoryMap(fw.segments, fw.lowAddress(), fw.format));
                    return r;
                });
            });
}

// ---- flash -------------------------------------------------------------------------------------

static void registerFlashCmd(Session& session) {
    auto& reg = session.registry();
    std::vector<ParamSpec> s = {
        portParam(),
        {"file", ParamType::String, "firmware file: .hex, .bin or .elf. For a multi-part ESP upload use parts= instead", false},
        {"parts", ParamType::String,
         "ESP only: several files at addresses, e.g. 0x1000=bootloader.bin,0x8000=partitions.bin,0x10000=app.bin (comma or newline separated)", false},
        boardParam(),
        {"protocol", ParamType::String, "force the protocol instead of the preset's", false,
         {"stk500v1", "stk500v2", "avr109", "esp", "stm32"}},
        {"baud", ParamType::Int, "bootloader baud (0 = the preset's; Uno/Mega 115200, old Nano/Pro Mini 57600)", false, {}, std::int64_t{0}},
        {"flash_baud", ParamType::Int, "ESP only: switch to this faster baud after connecting (e.g. 460800, 921600)", false, {}, std::int64_t{0}},
        {"reset", ParamType::String, "how to enter the bootloader (default: the preset's)", false,
         {"auto", "arduino", "1200bps", "esp-classic", "esp-usb-jtag", "dtr-boot0", "none"}, std::string("auto")},
        {"address", ParamType::String, "load address for a raw .bin (default: the board's, e.g. ESP 0x10000, STM32 0x08000000)", false},
        {"verify", ParamType::Bool, "read back (AVR/STM32) or MD5 (ESP) to confirm the write", false, {}, true},
        {"erase", ParamType::Bool, "STM32: mass-erase before writing (needed; flash only clears 1->0)", false, {}, true},
        {"run", ParamType::Bool, "start the new firmware when done (reset the board / jump)", false, {}, true},
        {"flash_size", ParamType::String, "ESP: the flash size to tell the ROM (e.g. 4m, 0x400000)", false},
        {"backend", backendParam().type, backendParam().description, false, backendParam().choices, std::string("native")},
    };
    reg.add({"flash.flash", "Flash Firmware",
             "Write firmware to an Arduino (AVR), ESP32/ESP8266 or STM32 board over serial, headless. Picks the board "
             "from the USB id (or use board=), resets into the bootloader, writes and verifies, then starts the new "
             "firmware. The protocols are built in, so no external tool is needed; pass backend= to use avrdude, "
             "esptool, STM32_Programmer_CLI, st-flash or dfu-util instead. Try it against sim:uno, sim:esp32 or "
             "sim:stm32 with no hardware. WRITING FLASH REPLACES THE PROGRAM ON THE BOARD.",
             s, false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    FlashRequest req;
                    try {
                        req = requestFrom(p);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    if (req.file.empty() && req.parts.empty()) return Result::failure("give a file= (or parts= for a multi-part ESP upload)");
                    const std::string backend = stringParam(p, "backend", "native");
                    if (backend != "native") {
                        ExternalPlan plan;
                        try {
                            plan = externalPlanFor(backend, req);
                        } catch (const std::exception& e) {
                            return Result::failure(e.what());
                        }
                        if (!plan.error.empty()) return Result::failure(plan.error);
                        if (isSimPort(req.port))
                            return Result::failure("backend=" + backend + " needs a real tool and port; sim: ports only work with the native backend");
                        auto exe = findProgram(plan.args[0]);
                        if (!exe) return Result::failure(plan.args[0] + " isn't installed");
                        plan.args[0] = exe->string();
                        ProcessResult pr = runProgram(plan.args);
                        Result r = pr.exitCode == 0 ? Result::success("flashed with " + backend) : Result::failure(backend + " failed (exit " + std::to_string(pr.exitCode) + ")");
                        r.with("backend", backend).with("command", join(plan.args, " "));
                        if (!pr.output.empty()) r.with("output", pr.output);
                        return r;
                    }
                    try {
                        FlashReport rep = flashPort(req);
                        return reportResult(rep, req);
                    } catch (const std::exception& e) {
                        return Result::failure(std::string("flash failed: ") + e.what());
                    }
                });
            });

    reg.add({"flash.command", "Show External Command",
             "Show the exact avrdude/esptool/STM32_Programmer_CLI/st-flash/dfu-util command line flash.flash would run "
             "for a given backend, without running it. Useful to see what native flashing does, or to copy into a script.",
             {portParam(),
              {"file", ParamType::String, "firmware file", false},
              {"parts", ParamType::String, "ESP parts, as in flash.flash", false},
              boardParam(),
              {"backend", ParamType::String, "which tool", true, {"avrdude", "esptool", "stm32prog", "st-flash", "dfu-util"}},
              {"address", ParamType::String, "load address for a raw .bin", false},
              {"baud", ParamType::Int, "baud", false, {}, std::int64_t{0}},
              {"verify", ParamType::Bool, "include verification", false, {}, true}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    FlashRequest req;
                    try {
                        req = requestFrom(p);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    ExternalPlan plan;
                    try {
                        plan = externalPlanFor(stringParam(p, "backend"), req);
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                    if (plan.args.empty()) return Result::failure(plan.error);
                    Result r = Result::success(join(plan.args, " "));
                    r.with("program", plan.program).with("installed", findBackendProgram(stringParam(p, "backend")).has_value());
                    if (!plan.error.empty()) r.with("note", plan.error);
                    return r;
                });
            });
}

// ---- monitor / send ----------------------------------------------------------------------------

static void registerMonitor(Session& session) {
    auto& reg = session.registry();
    reg.add({"flash.monitor", "Serial Monitor",
             "Open the port and read for a few seconds, returning the text (other bytes shown as \\xNN). Good for seeing "
             "a board's boot messages or Serial.print output. Does not reset the board unless reset=true.",
             {portParam(),
              {"seconds", ParamType::Number, "how long to read", false, {}, 3.0},
              {"baud", ParamType::Int, "serial baud (match your Serial.begin, often 9600 or 115200)", false, {}, std::int64_t{115200}},
              {"reset", ParamType::Bool, "pulse the reset line first to catch the boot messages", false, {}, false},
              {"max_bytes", ParamType::Int, "stop after this many bytes", false, {}, std::int64_t{65536}}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    SerialConfig cfg;
                    cfg.baud = static_cast<int>(intParam(p, "baud", 115200));
                    const double seconds = std::clamp(numberParam(p, "seconds", 3.0), 0.0, 300.0);
                    const std::size_t maxBytes = static_cast<std::size_t>(std::max<std::int64_t>(1, intParam(p, "max_bytes", 65536)));
                    try {
                        auto port = openPort(stringParam(p, "port"), cfg);
                        if (boolParam(p, "reset", false)) {
                            resetArduino(*port);
                        }
                        std::string text;
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
                        std::uint8_t buf[1024];
                        while (std::chrono::steady_clock::now() < deadline && text.size() < maxBytes) {
                            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                            std::size_t n = port->read(buf, sizeof buf, static_cast<int>(std::max<long long>(1, std::min<long long>(left, 200))));
                            text.append(reinterpret_cast<char*>(buf), n);
                        }
                        Result r = Result::success("read " + std::to_string(text.size()) + " bytes in " +
                                                   std::to_string(static_cast<int>(seconds + 0.5)) + "s");
                        r.with("bytes", static_cast<std::int64_t>(text.size())).with("text", printable(text));
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });

    reg.add({"flash.send", "Send Serial Data",
             "Write text to the port, then optionally read the reply for a moment. Escapes in the text are honoured: \\n "
             "\\r \\t \\\\ \\0 \\xNN. Use it to talk to a sketch's serial command interface.",
             {portParam(),
              {"text", ParamType::String, "what to send (e.g. \"AT\\r\\n\")", true},
              {"baud", ParamType::Int, "serial baud", false, {}, std::int64_t{115200}},
              {"read_seconds", ParamType::Number, "read the reply for this long after sending (0 = don't)", false, {}, 1.0},
              {"newline", ParamType::String, "append this line ending", false, {"none", "lf", "crlf", "cr"}, std::string("none")}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    SerialConfig cfg;
                    cfg.baud = static_cast<int>(intParam(p, "baud", 115200));
                    std::string data = unescape(stringParam(p, "text"));
                    const std::string nl = stringParam(p, "newline", "none");
                    if (nl == "lf") data += "\n";
                    else if (nl == "crlf") data += "\r\n";
                    else if (nl == "cr") data += "\r";
                    const double seconds = std::clamp(numberParam(p, "read_seconds", 1.0), 0.0, 120.0);
                    try {
                        auto port = openPort(stringParam(p, "port"), cfg);
                        port->write(data);
                        std::string reply;
                        if (seconds > 0) {
                            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
                            std::uint8_t buf[1024];
                            while (std::chrono::steady_clock::now() < deadline) {
                                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
                                std::size_t n = port->read(buf, sizeof buf, static_cast<int>(std::max<long long>(1, std::min<long long>(left, 200))));
                                reply.append(reinterpret_cast<char*>(buf), n);
                            }
                        }
                        Result r = Result::success("sent " + std::to_string(data.size()) + " bytes" +
                                                   (seconds > 0 ? ", read " + std::to_string(reply.size()) + " back" : ""));
                        r.with("sent", static_cast<std::int64_t>(data.size()));
                        if (seconds > 0) r.with("reply", printable(reply));
                        return r;
                    } catch (const std::exception& e) {
                        return Result::failure(e.what());
                    }
                });
            });
}

void registerCommands(Session& session) {
    registerPorts(session);
    registerInspect(session);
    registerFlashCmd(session);
    registerMonitor(session);
}

}  // namespace achclwss::flash
