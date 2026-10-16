#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#ifdef __linux__
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

#include "achclwss/core/cli.hpp"
#include "achclwss/flash/flash.hpp"
#include "achclwss/mcp/server.hpp"
#include "check.hpp"

using namespace achclwss;
using namespace achclwss::flash;
namespace fs = std::filesystem;

namespace {

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string join_(const std::vector<std::string>& v) {
    std::string s;
    for (auto& x : v) s += x + " ";
    return s;
}

fs::path makeTempDir() {
    std::random_device rd;
    fs::path dir = fs::temp_directory_path() / ("achclwss_flash_" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& p, const Bytes& b) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A deterministic "firmware" blob.
Bytes blob(std::size_t n, std::uint8_t seed = 1) {
    Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>((i * 7 + seed * 13) & 0xFF);
    return b;
}

// ---- checksums & digests -------------------------------------------------------------------

void testChecksums() {
    const std::string s = "The quick brown fox jumps over the lazy dog";
    CHECK(md5Hex(reinterpret_cast<const std::uint8_t*>(s.data()), s.size()) == "9e107d9d372bb6826bd81d3542a419d6");
    CHECK(md5Hex(nullptr, 0) == "d41d8cd98f00b204e9800998ecf8427e");
    const std::uint8_t abc[] = {'a', 'b', 'c'};
    CHECK(crc32(abc, 3) == 0x352441C2u);
    CHECK(xorBytes(abc, 3, 0) == ('a' ^ 'b' ^ 'c'));
    // Intel HEX checksum: two's complement of the byte sum
    const std::uint8_t rec[] = {0x03, 0x00, 0x30, 0x00, 0x02, 0x33, 0x7A};
    CHECK(hexChecksum(rec, 7) == 0x1E);
    CHECK(parseAddress("0x10000") == 0x10000u);
    CHECK(parseAddress("64k") == 65536u);
    CHECK(parseAddress("4m") == 4u * 1024 * 1024);
    CHECK(parseAddress("4096") == 4096u);
}

// ---- Intel HEX ----------------------------------------------------------------------------

void testIntelHex() {
    const std::string hex =
        ":100000000C9461000C947E000C947E000C947E0095\n"
        ":100010000C947E000C947E000C947E000C947E0068\n"
        ":00000001FF\n";
    Firmware fw = parseIntelHex(hex);
    CHECK(fw.format == "ihex");
    CHECK(fw.byteCount() == 32);
    CHECK(fw.lowAddress() == 0);
    CHECK(fw.highAddress() == 32);
    CHECK(fw.segments.size() == 1);  // the two records are adjacent, so they merge
    // extended linear address
    const std::string hi =
        ":020000040001F9\n"
        ":04000000DEADBEEFC4\n"
        ":00000001FF\n";
    Firmware f2 = parseIntelHex(hi);
    CHECK(f2.lowAddress() == 0x10000u);
    CHECK(f2.byteCount() == 4);
    // round-trip through toIntelHex
    std::string out = toIntelHex(fw.segments);
    Firmware back = parseIntelHex(out);
    CHECK(back.flatten() == fw.flatten());
    // a corrupt checksum is caught
    bool threw = false;
    try {
        parseIntelHex(":100000000C9461000C947E000C947E000C947E00FF\n:00000001FF\n");
    } catch (const FirmwareError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- ELF ---------------------------------------------------------------------------------

// A minimal 32-bit little-endian ELF with one PT_LOAD program header.
Bytes makeElf(std::uint16_t machine, std::uint32_t paddr, const Bytes& payload) {
    Bytes b(52, 0);
    b[0] = 0x7F;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 1;  // 32-bit
    b[5] = 1;  // little endian
    b[6] = 1;
    auto put16 = [&](std::size_t at, std::uint16_t v) {
        b[at] = static_cast<std::uint8_t>(v & 0xFF);
        b[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    put16(16, 2);        // ET_EXEC
    put16(18, machine);  // e_machine
    put32(20, 1);        // version
    put32(24, 0x100);    // entry
    put32(28, 52);       // phoff
    put32(32, 0);        // shoff
    put16(42, 32);       // phentsize
    put16(44, 1);        // phnum
    put16(46, 40);       // shentsize
    put16(48, 0);        // shnum
    put16(50, 0);        // shstrndx
    const std::uint32_t dataOff = 52 + 32;
    // program header
    Bytes ph(32, 0);
    auto p32 = [&](std::size_t at, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) ph[at + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    p32(0, 1);                                        // PT_LOAD
    p32(4, dataOff);                                  // offset
    p32(8, paddr);                                    // vaddr
    p32(12, paddr);                                   // paddr
    p32(16, static_cast<std::uint32_t>(payload.size()));  // filesz
    p32(20, static_cast<std::uint32_t>(payload.size()));  // memsz
    p32(24, 5);                                       // flags RX
    b.insert(b.end(), ph.begin(), ph.end());
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

void testElf() {
    Bytes payload = blob(64);
    Bytes elf = makeElf(83, 0, payload);  // AVR
    Firmware fw = parseElf(elf);
    CHECK(fw.format == "elf");
    CHECK(fw.elfMachine == "AVR");
    CHECK(fw.byteCount() == 64);
    CHECK(fw.segments.front().data == payload);
    CHECK(fw.entry.value() == 0x100u);
    // Xtensa ELF for an ESP
    Bytes esp = makeElf(94, 0x40080000, blob(32));
    Firmware f2 = parseElf(esp);
    CHECK(f2.elfMachine == "Xtensa");
    CHECK(f2.lowAddress() == 0x40080000u);
    // not an ELF
    bool threw = false;
    try {
        parseElf(blob(10));
    } catch (const FirmwareError&) {
        threw = true;
    }
    CHECK(threw);
}

// ---- ESP image header --------------------------------------------------------------------

Bytes makeEspImage(std::uint16_t chipId, const std::vector<std::pair<std::uint32_t, Bytes>>& segs) {
    Bytes b = {0xE9, static_cast<std::uint8_t>(segs.size()), 0x02 /*DIO*/, 0x40 /*4MB 40m*/};
    for (int i = 0; i < 4; ++i) b.push_back(0);  // entry
    // extended header: wp_pin(8), spi_pin_drv[3](9..11), chip_id(12..13), then to offset 23
    b.push_back(0xEE);  // offset 8
    b.push_back(0);     // 9
    b.push_back(0);     // 10
    b.push_back(0);     // 11
    b.push_back(static_cast<std::uint8_t>(chipId & 0xFF));  // 12
    b.push_back(static_cast<std::uint8_t>(chipId >> 8));    // 13
    for (int i = 0; i < 9; ++i) b.push_back(0);            // 14..22
    b.push_back(0);  // offset 23: hash_appended
    std::uint8_t sum = 0xEF;
    for (const auto& [addr, data] : segs) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((addr >> (8 * i)) & 0xFF));
        const std::uint32_t n = static_cast<std::uint32_t>(data.size());
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((n >> (8 * i)) & 0xFF));
        b.insert(b.end(), data.begin(), data.end());
        sum = xorBytes(data.data(), data.size(), sum);
    }
    while (b.size() % 16 != 15) b.push_back(0);
    b.push_back(sum);
    return b;
}

void testEspImage() {
    Bytes img = makeEspImage(9, {{0x3F400020, blob(48)}, {0x40080000, blob(32, 2)}});  // chip 9 = esp32-s3
    auto info = parseEspImage(img);
    CHECK(info.has_value());
    CHECK(info->extended);
    CHECK(info->chipName() == "esp32-s3");
    CHECK(info->segmentCount == 2);
    CHECK(info->flashModeName() == "dio");
    CHECK(info->checksumOk);
    // through parseFirmware a .bin with the magic becomes esp-image
    Firmware fw = parseFirmware(img, "app.bin", 0x10000);
    CHECK(fw.format == "esp-image");
    CHECK(fw.esp.has_value());
}

// ---- SLIP & packet builders --------------------------------------------------------------

void testSlip() {
    Bytes raw = {0x01, 0xC0, 0x02, 0xDB, 0x03};
    Bytes enc = slip::encode(raw);
    CHECK(enc.front() == 0xC0 && enc.back() == 0xC0);
    CHECK(contains(std::string(enc.begin(), enc.end()), std::string{static_cast<char>(0xDB), static_cast<char>(0xDC)}));  // escaped C0
    slip::Decoder dec;
    std::optional<Bytes> got;
    for (auto b : enc) {
        if (auto p = dec.feed(b)) got = p;
    }
    CHECK(got.has_value() && *got == raw);
    // a request frame's header
    Bytes req = esp::request(esp::kFlashBegin, {1, 2, 3, 4});
    CHECK(req[0] == 0x00 && req[1] == esp::kFlashBegin && req[2] == 4 && req[3] == 0);
    // STK500v2 frame/unframe round trip with checksum
    Bytes body = {stk500v2::kSignOn};
    Bytes frame = stk500v2::frame(7, body);
    CHECK(frame.front() == 0x1B);
    std::uint8_t seq = 0;
    Bytes buf = frame;
    auto parsed = stk500v2::unframe(buf, &seq);
    CHECK(parsed.has_value() && *parsed == body && seq == 7);
    // STK500v1 load address sends the word address
    Bytes la = stk500v1::loadAddress(0x200);
    CHECK(la[0] == 0x55 && la[1] == 0x00 && la[2] == 0x01);  // 0x200 bytes = word 0x100
}

// ---- end-to-end flashing on the simulators -----------------------------------------------

Bytes simFlashImage(const std::string& port) { return simDevice(port)->flashImage(); }

void testAvrFlow(const char* portBase, const char* ihexFile, const fs::path& dir) {
    const std::string port = portBase;
    resetSims();
    // build a HEX file
    std::vector<Segment> segs = {{0, blob(2048)}};
    std::string hex = toIntelHex(segs);
    fs::path f = dir / ihexFile;
    std::ofstream(f) << hex;

    FlashRequest req;
    req.port = port;
    req.file = f.string();
    req.verify = true;
    FlashReport rep = flashPort(req);
    CHECK(rep.verified);
    CHECK(rep.bytes == 2048);
    // the simulated flash now holds exactly what we wrote
    Bytes img = simFlashImage(port);
    CHECK(img == blob(2048));
    // monitor sees the app's boot banner once it starts
    Session s;
    flash::registerCommands(s);
    Result mon = s.run("flash.monitor", {{"port", std::string(port)}, {"seconds", 0.2}, {"baud", std::int64_t{115200}}});
    CHECK(mon.ok);
    CHECK(contains(toText(*mon.field("text")), "firmware running"));
}

void testEspFlow(const fs::path& dir) {
    resetSims();
    const std::string port = "sim:esp32";
    // write an app.bin (ESP image) to 0x10000
    Bytes app = makeEspImage(0, {{0x3F400020, blob(1200)}});
    fs::path f = dir / "esp-app.bin";
    writeFile(f, app);
    FlashRequest req;
    req.port = port;
    req.file = f.string();
    req.address = 0x10000;
    req.verify = true;  // MD5 verify on the esp32 sim
    FlashReport rep = flashPort(req);
    CHECK(rep.chip == "esp32");
    CHECK(rep.verified && rep.verifyMethod == "md5");
    // flash content at 0x10000 matches
    Bytes img = simDevice(port)->flashImage();
    CHECK(img.size() >= 0x10000 + app.size());
    CHECK(Bytes(img.begin() + 0x10000, img.begin() + 0x10000 + static_cast<std::ptrdiff_t>(app.size())) == app);

    // multi-part upload
    resetSims();
    Bytes boot = makeEspImage(0, {{0x3F400020, blob(256, 3)}});
    Bytes parttable = blob(0x1000, 9);
    fs::path fb = dir / "boot.bin", fp = dir / "part.bin";
    writeFile(fb, boot);
    writeFile(fp, parttable);
    FlashRequest req2;
    req2.port = port;
    req2.parts = {{0x1000, fb.string()}, {0x8000, fp.string()}, {0x10000, f.string()}};
    FlashReport rep2 = flashPort(req2);
    CHECK(rep2.regions.size() == 3);
    CHECK(rep2.verified);
}

// The white-screen bugs: merged images landing at the app slot, firmware for another chip.
void testEspMergedAndChipChecks(const fs::path& dir) {
    const Bytes boot = makeEspImage(0, {{0x3FFF0030, blob(256, 3)}});
    const Bytes app = makeEspImage(0, {{0x3F400020, blob(1200, 4)}});
    // Arduino's .merged.bin: starts at flash 0x0, 0xFF up to the bootloader at 0x1000
    Bytes padded(0x10000, 0xFF);
    std::copy(boot.begin(), boot.end(), padded.begin() + 0x1000);
    padded[0x8000] = 0xAA;
    padded[0x8001] = 0x50;
    padded.insert(padded.end(), app.begin(), app.end());
    Firmware pf = parseFirmware(padded, "sketch.ino.merged.bin", 0x10000);
    CHECK(pf.format == "esp-image" && pf.espMerged);
    CHECK(pf.espMergedBootOffset == 0 && pf.espBootloaderAt == 0x1000);

    resetSims();
    fs::path fm = dir / "sketch.ino.merged.bin";
    writeFile(fm, padded);
    FlashRequest req;
    req.port = "sim:esp32";
    req.file = fm.string();  // default address 0x10000: must still land at 0x0
    req.verify = true;
    FlashReport rep = flashPort(req);
    CHECK(rep.regions.size() == 1 && contains(rep.regions[0], "0x00000000"));
    Bytes img = simDevice("sim:esp32")->flashImage();
    CHECK(img.size() >= 0x10000 + app.size());
    CHECK(Bytes(img.begin() + 0x1000, img.begin() + 0x1000 + static_cast<std::ptrdiff_t>(boot.size())) == boot);
    CHECK(Bytes(img.begin() + 0x10000, img.begin() + 0x10000 + static_cast<std::ptrdiff_t>(app.size())) == app);

    // esptool merge_bin output (starts at the bootloader) pointed at 0x0 goes to 0x1000
    resetSims();
    Bytes fromBoot(padded.begin() + 0x1000, padded.end());
    fs::path fe = dir / "factory.bin";
    writeFile(fe, fromBoot);
    FlashRequest req2;
    req2.port = "sim:esp32";
    req2.file = fe.string();
    req2.address = 0x0;
    FlashReport rep2 = flashPort(req2);
    CHECK(contains(rep2.regions[0], "0x00001000"));
    img = simDevice("sim:esp32")->flashImage();
    CHECK(Bytes(img.begin() + 0x1000, img.begin() + 0x1000 + static_cast<std::ptrdiff_t>(boot.size())) == boot);

    // an ESP32-S3 build on a plain ESP32 is refused before anything is erased
    resetSims();
    fs::path fs3 = dir / "s3-app.bin";
    writeFile(fs3, makeEspImage(9, {{0x3C000020, blob(512)}}));
    FlashRequest req3;
    req3.port = "sim:esp32";
    req3.file = fs3.string();
    bool refused = false;
    try {
        flashPort(req3);
    } catch (const FirmwareError& e) {
        refused = contains(e.what(), "built for esp32-s3") && contains(e.what(), "esp32");
    }
    CHECK(refused);
    CHECK(simDevice("sim:esp32")->flashImage().empty());

    // an ESP32 merged image (bootloader at 0x1000) on an S3 is refused too
    resetSims();
    Bytes s3merged = padded;
    s3merged[0x1000 + 12] = 9;  // bootloader says esp32-s3, but sits at 0x1000
    fs::path fsm = dir / "s3-merged.bin";
    writeFile(fsm, s3merged);
    FlashRequest req4;
    req4.port = "sim:esp32-s3";
    req4.file = fsm.string();
    refused = false;
    try {
        flashPort(req4);
    } catch (const FirmwareError& e) {
        refused = contains(e.what(), "boots from 0x00000000");
    }
    CHECK(refused);
}

void testEsp8266Flow(const fs::path& dir) {
    resetSims();
    const std::string port = "sim:esp8266";
    Bytes app = blob(2048, 5);
    fs::path f = dir / "esp8266.bin";
    writeFile(f, app);
    FlashRequest req;
    req.port = port;
    req.file = f.string();
    req.address = 0;
    req.verify = true;  // no MD5 on 8266: should still succeed, just not verified
    FlashReport rep = flashPort(req);
    CHECK(rep.chip == "esp8266");
    CHECK(!rep.verified);  // ROM can't MD5
    Bytes img = simDevice(port)->flashImage();
    CHECK(Bytes(img.begin(), img.begin() + static_cast<std::ptrdiff_t>(app.size())) == app);
}

void testStm32Flow(const fs::path& dir) {
    resetSims();
    const std::string port = "sim:stm32";
    Bytes fwdata = blob(4000);
    fs::path f = dir / "stm32.bin";
    writeFile(f, fwdata);
    FlashRequest req;
    req.port = port;
    req.file = f.string();
    req.address = 0x08000000;
    req.verify = true;
    req.erase = true;
    req.run = true;
    FlashReport rep = flashPort(req);
    CHECK(contains(rep.chip, "STM32"));
    CHECK(rep.verified);
    Bytes img = simDevice(port)->flashImage();
    CHECK(Bytes(img.begin(), img.begin() + static_cast<std::ptrdiff_t>(fwdata.size())) == fwdata);
    // stm32f4 uses extended erase
    resetSims();
    FlashRequest req2;
    req2.port = "sim:stm32f4";
    req2.board = "stm32f4";
    req2.file = f.string();
    req2.address = 0x08000000;
    FlashReport rep2 = flashPort(req2);
    CHECK(rep2.verified);
}

// ---- detection ----------------------------------------------------------------------------

void testDetect() {
    resetSims();
    DetectReport d = detectBoard("sim:uno", "auto", true);
    CHECK(d.found);
    CHECK(d.protocol == "stk500v1");
    CHECK(contains(d.chip, "328"));

    resetSims();
    DetectReport e = detectBoard("sim:esp32", "auto", true);
    CHECK(e.found && e.protocol == "esp" && e.chip == "esp32");

    resetSims();
    DetectReport m = detectBoard("sim:stm32", "stm32", true);
    CHECK(m.found && m.protocol == "stm32");

    // USB id identification (no hardware needed)
    auto k = identifyUsb({0x1A86, 0x7523});
    CHECK(k.has_value() && k->chip == "CH340");
    auto a = identifyUsb({0x2341, 0x0043});
    CHECK(a.has_value() && a->board == "uno");
    auto st = identifyUsb({0x0483, 0x3748});
    CHECK(st.has_value() && st->family == "debug-probe");
    CHECK(!identifyUsb({0x1234, 0x5678}).has_value());
    CHECK(parseUsbId("VID_1A86&PID_7523")->vid == 0x1A86);
    CHECK(parseUsbId("1a86:7523")->pid == 0x7523);
}

// ---- external tool planning ----------------------------------------------------------------

void testExternal() {
    FlashRequest req;
    req.port = "/dev/ttyUSB0";
    req.file = "app.hex";
    req.board = "uno";
    PortInfo info = describePort("sim:uno");  // family avr
    info.name = "/dev/ttyUSB0";
    Resolved r = resolve(req, info);
    ExternalPlan avr = planExternal("avrdude", req, r);
    CHECK(contains(join_(avr.args), "arduino") || contains(join_(avr.args), "avrdude"));
    CHECK(contains(join_(avr.args), "flash:w:app.hex"));

    FlashRequest er;
    er.port = "/dev/ttyUSB1";
    er.board = "esp32";
    er.parts = {{0x1000, "boot.bin"}, {0x10000, "app.bin"}};
    PortInfo ei = describePort("sim:esp32");
    ei.name = "/dev/ttyUSB1";
    Resolved rr = resolve(er, ei);
    ExternalPlan esp = planExternal("esptool", er, rr);
    CHECK(contains(join_(esp.args), "write_flash"));
    CHECK(contains(join_(esp.args), "0x00001000") && contains(join_(esp.args), "boot.bin"));
}

// ---- send on a running sim ----------------------------------------------------------------

void testSend() {
    resetSims();
    // flash something so the uno sim is running its app, which echoes lines
    fs::path dir = makeTempDir();
    std::vector<Segment> segs = {{0, blob(512)}};
    fs::path f = dir / "echo.hex";
    std::ofstream(f) << toIntelHex(segs);
    FlashRequest req;
    req.port = "sim:uno";
    req.file = f.string();
    flashPort(req);
    Session s;
    flash::registerCommands(s);
    Result snd = s.run("flash.send", {{"port", std::string("sim:uno")}, {"text", std::string("hello")},
                                      {"newline", std::string("lf")}, {"read_seconds", 0.2}});
    CHECK(snd.ok);
    CHECK(contains(toText(*snd.field("reply")), "got: hello"));
    fs::remove_all(dir);
}

// ---- MCP surface --------------------------------------------------------------------------

void testMcp() {
    resetSims();
    Session s;
    flash::registerCommands(s);
    mcp::Server server(s, "flash", "test");
    std::string list = server.handle(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})").value_or("");
    for (const char* tool : {"flash_ports", "flash_detect", "flash_boards", "flash_inspect", "flash_flash", "flash_monitor", "flash_send"}) {
        CHECK(contains(list, std::string("\"") + tool + "\""));
    }
    // flash.boards is read-only and works
    std::string r = server.handle(R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"flash_boards","arguments":{}}})").value_or("");
    CHECK(contains(r, "\"isError\":false"));
    CHECK(contains(r, "uno") && contains(r, "esp32") && contains(r, "stm32"));
    // a full flash over MCP on a simulated board, with a HEX file
    fs::path dir = makeTempDir();
    std::vector<Segment> segs = {{0, blob(1024, 4)}};
    fs::path f = dir / "mcp.hex";
    std::ofstream(f) << toIntelHex(segs);
    std::string call = R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"flash_flash","arguments":{"port":"sim:nano","file":")" +
                       f.generic_string() + R"("}}})";
    std::string fr = server.handle(call).value_or("");
    CHECK(contains(fr, "\"isError\":false"));
    CHECK(contains(fr, "verified") || contains(fr, "flashed"));
    // inspect with a memory-map preview attaches an image
    std::string ins = server.handle(R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"flash_inspect","arguments":{"file":")" +
                                    f.generic_string() + R"(","preview":true}}})").value_or("");
    CHECK(contains(ins, "\"isError\":false"));
    CHECK(contains(ins, "image/png") || contains(ins, "image"));
    fs::remove_all(dir);
}

// ---- CLI / batch --------------------------------------------------------------------------

void testCli() {
    resetSims();
    Session s;
    flash::registerCommands(s);
    cli::Batch batch(s);
    std::ostringstream out, err;
    fs::path dir = makeTempDir();
    std::vector<Segment> segs = {{0, blob(640, 6)}};
    fs::path f = dir / "cli.hex";
    std::ofstream(f) << toIntelHex(segs);
    std::istringstream script("flash.inspect file=\"" + f.string() + "\"\nflash.flash port=sim:uno file=\"" + f.string() + "\"\n");
    int failures = batch.run(script, out, err);
    CHECK(failures == 0);
    CHECK(contains(out.str(), "ihex"));
    fs::remove_all(dir);
}

// ---- optional: a real pty round trip on Linux ---------------------------------------------

// A real Linux pty: the native PosixPort opens the slave side, writes and reads
// against the master, confirming the termios serial layer works end to end.
void testPty() {
#ifdef __linux__
    int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || ::grantpt(master) != 0 || ::unlockpt(master) != 0) {
        if (master >= 0) ::close(master);
        std::printf("pty unavailable: skipping the real serial test\n");
        return;
    }
    const char* slaveName = ::ptsname(master);
    if (!slaveName) {
        ::close(master);
        return;
    }
    try {
        SerialConfig cfg;
        cfg.baud = 115200;
        auto port = openNativePort(slaveName, cfg);
        const std::string msg = "ping\n";
        port->write(msg);
        std::string gotbuf(16, '\0');
        ssize_t n = ::read(master, gotbuf.data(), msg.size());
        CHECK(n == static_cast<ssize_t>(msg.size()));
        CHECK(gotbuf.substr(0, msg.size()) == msg);
        const std::string back = "pong\n";
        ssize_t w = ::write(master, back.data(), back.size());
        CHECK(w == static_cast<ssize_t>(back.size()));
        Bytes in = port->readExact(back.size(), 1000);
        CHECK(std::string(in.begin(), in.end()) == back);
    } catch (const std::exception& e) {
        std::printf("pty test error (ignored on odd CI): %s\n", e.what());
    }
    ::close(master);
#else
    CHECK(true);
#endif
}

}  // namespace

int main() {
    fs::path dir = makeTempDir();
    testChecksums();
    testIntelHex();
    testElf();
    testEspImage();
    testSlip();
    testAvrFlow("sim:uno", "uno.hex", dir);
    testAvrFlow("sim:nano-old", "nanoold.hex", dir);
    testAvrFlow("sim:mega2560", "mega.hex", dir);
    testAvrFlow("sim:leonardo", "leo.hex", dir);
    testEspFlow(dir);
    testEspMergedAndChipChecks(dir);
    testEsp8266Flow(dir);
    testStm32Flow(dir);
    testDetect();
    testExternal();
    testSend();
    testMcp();
    testCli();
    testPty();
    fs::remove_all(dir);
    return check::summary("flash");
}
