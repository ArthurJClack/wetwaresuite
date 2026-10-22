// Firmware builds through installed toolchains (see build.hpp).
#include "wetwaresuite/flash/build.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/flash/protocol.hpp"

namespace wetwaresuite::flash {

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> out;
    std::string item;
    for (char c : text + ",") {
        if (c == ',') {
            const auto a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
            if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
            item.clear();
        } else {
            item += c;
        }
    }
    return out;
}

bool installed(const std::string& program) { return findProgram(program).has_value(); }

// pio is also installed as platformio
std::string pioProgram() { return installed("pio") || !installed("platformio") ? "pio" : "platformio"; }

// every AVR preset here runs at 16 MHz
std::string cpuHz(const Board&) { return "16000000UL"; }

}  // namespace

std::vector<Toolchain> toolchains() {
    const std::vector<std::pair<std::string, std::string>> known = {
        {"arduino-cli", "Arduino sketches (.ino) for every board with an Arduino core"},
        {"pio", "PlatformIO projects (platformio.ini)"},
        {"avr-gcc", "plain C/C++ for AVR boards (Uno, Nano, Mega, Leonardo)"},
        {"avr-objcopy", "turns avr-gcc's ELF into an Intel HEX file"},
        {"arm-none-eabi-gcc", "bare-metal STM32 builds (from your own Makefile)"},
        {"xtensa-esp32-elf-gcc", "ESP-IDF builds (use idf.py)"},
        {"idf.py", "ESP-IDF projects"},
        {"make", "Makefile projects"},
    };
    std::vector<Toolchain> out;
    for (const auto& [name, purpose] : known) {
        std::string prog;
        if (auto p = findProgram(name)) prog = p->string();
        else if (name == "pio") {
            if (auto q = findProgram("platformio")) prog = q->string();
        }
        out.push_back({name, prog, purpose});
    }
    return out;
}

std::string fqbnFor(const std::string& boardIn) {
    if (std::count(boardIn.begin(), boardIn.end(), ':') >= 2) return boardIn;  // already an FQBN
    const Board* b = findBoard(lower(boardIn));
    if (!b) return "";
    static const std::vector<std::pair<std::string, std::string>> map = {
        {"uno", "arduino:avr:uno"},
        {"nano", "arduino:avr:nano:cpu=atmega328"},
        {"nano-old", "arduino:avr:nano:cpu=atmega328old"},
        {"pro-mini", "arduino:avr:pro:cpu=16MHzatmega328"},
        {"mega2560", "arduino:avr:mega:cpu=atmega2560"},
        {"leonardo", "arduino:avr:leonardo"},
        {"micro", "arduino:avr:micro"},
        {"esp32", "esp32:esp32:esp32"},
        {"esp32-s2", "esp32:esp32:esp32s2"},
        {"esp32-s3", "esp32:esp32:esp32s3"},
        {"esp32-c3", "esp32:esp32:esp32c3"},
        {"esp32-c6", "esp32:esp32:esp32c6"},
        {"esp32-h2", "esp32:esp32:esp32h2"},
        {"esp8266", "esp8266:esp8266:nodemcuv2"},
        {"stm32", "STMicroelectronics:stm32:GenF1:pnum=BLUEPILL_F103C8"},
    };
    for (const auto& [name, fqbn] : map) {
        if (name == b->name) return fqbn;
    }
    return "";
}

CompilePlan planCompile(const CompileRequest& req) {
    CompilePlan plan;
    if (req.source.empty()) {
        plan.error = "give source= (a sketch folder or .ino, a PlatformIO project, or .c/.cpp files)";
        return plan;
    }
    const std::vector<std::string> sources = split(req.source);
    const fs::path first = sources.front();
    std::error_code ec;
    const bool isDir = fs::is_directory(first, ec);
    const std::string ext = lower(first.extension().string());
    const Board* board = findBoard(lower(req.board));
    plan.board = board ? board->name : req.board;
    // which kind of source is it?
    std::string kind;
    fs::path sketchDir;
    if (isDir && fs::exists(first / "platformio.ini", ec)) kind = "pio";
    else if (ext == ".ino") kind = "arduino", sketchDir = first.parent_path().empty() ? fs::path(".") : first.parent_path();
    else if (isDir) {
        kind = "arduino";
        sketchDir = first;
        if (!fs::exists(first / (first.filename().string() + ".ino"), ec)) {
            bool any = false;
            for (const auto& e : fs::directory_iterator(first, ec)) any = any || lower(e.path().extension().string()) == ".ino";
            if (!any) {
                plan.error = first.string() + " has no .ino sketch and no platformio.ini";
                return plan;
            }
        }
    } else if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".s" || ext == ".S") {
        kind = "avr-gcc";
    } else {
        plan.error = "don't know how to build " + first.string() + " (a sketch, a PlatformIO project or .c/.cpp files)";
        return plan;
    }
    std::string tool = lower(req.tool.empty() ? "auto" : req.tool);
    if (tool == "platformio") tool = "pio";
    if (tool == "auto") tool = kind == "arduino" ? "arduino-cli" : kind;
    // a sketch can also go through PlatformIO, but only with a project file; plain C needs avr-gcc
    if ((kind == "pio" && tool != "pio") || (kind == "avr-gcc" && tool != "avr-gcc") || (kind == "arduino" && tool != "arduino-cli")) {
        plan.error = "a " + std::string(kind == "pio" ? "PlatformIO project" : kind == "arduino" ? "sketch" : "C/C++ source") + " builds with " +
                     (kind == "arduino" ? std::string("arduino-cli") : kind) + ", not " + tool;
        return plan;
    }
    plan.tool = tool;
    if (tool == "arduino-cli") {
        const std::string fqbn = fqbnFor(req.board);
        if (fqbn.empty()) {
            plan.error = "no Arduino board id (FQBN) for '" + req.board + "': pass one, e.g. board=arduino:avr:uno";
            return plan;
        }
        const fs::path out = req.outDir.empty() ? sketchDir / "build" : fs::path(req.outDir);
        plan.steps.push_back({"arduino-cli", "compile", "--fqbn", fqbn, "--output-dir", out.string(), sketchDir.string()});
        std::string name = (ext == ".ino" ? first.filename().string() : sketchDir.filename().string() + ".ino");
        const bool esp = board && board->family == "esp";
        const bool stm = board && board->family == "stm32";
        plan.output = (out / (name + (esp || stm ? ".bin" : ".hex"))).string();
    } else if (tool == "pio") {
        std::vector<std::string> step{pioProgram(), "run", "-d", first.string()};
        if (!req.env.empty()) step.insert(step.end(), {"-e", req.env});
        plan.steps.push_back(step);
        // PlatformIO picks the name; firmware.hex for AVR, firmware.bin otherwise
        const std::string env = req.env.empty() ? (board ? board->name : std::string("default")) : req.env;
        const bool avr = !board || board->family == "avr";
        plan.output = (first / ".pio" / "build" / env / (avr ? "firmware.hex" : "firmware.bin")).string();
    } else {
        if (!board || board->family != "avr") {
            plan.error = "plain C builds are for AVR boards (uno, nano, mega2560, leonardo ...); for " + req.board +
                         " use an Arduino sketch (arduino-cli) or a PlatformIO project";
            return plan;
        }
        const fs::path out = req.outDir.empty() ? (first.parent_path().empty() ? fs::path("build") : first.parent_path() / "build") : fs::path(req.outDir);
        const std::string stem = first.stem().string();
        const std::string elf = (out / (stem + ".elf")).string(), hex = (out / (stem + ".hex")).string();
        std::vector<std::string> cc{"avr-gcc", "-mmcu=" + board->mcu, "-DF_CPU=" + cpuHz(*board), "-Os", "-Wall", "-ffunction-sections", "-fdata-sections",
                                    "-Wl,--gc-sections"};
        for (const auto& d : split(req.defines)) cc.push_back("-D" + d);
        cc.insert(cc.end(), {"-o", elf});
        cc.insert(cc.end(), sources.begin(), sources.end());
        plan.steps.push_back(cc);
        plan.steps.push_back({"avr-objcopy", "-O", "ihex", "-R", ".eeprom", elf, hex});
        plan.output = hex;
    }
    plan.installed = true;
    for (const auto& s : plan.steps) {
        const std::string prog = s.front();
        if (!installed(prog)) plan.installed = false;
    }
    return plan;
}

std::string sketchTemplate(const std::string& kind, const std::string& board) {
    const Board* b = findBoard(lower(board));
    const bool esp = b && b->family == "esp";
    const std::string led = esp ? "2" : "LED_BUILTIN";
    if (kind == "empty") return "void setup() {\n}\n\nvoid loop() {\n}\n";
    if (kind == "serial") {
        return "// Echoes what arrives on the serial port (flash.monitor / flash.send talk to it).\n"
               "void setup() {\n  Serial.begin(115200);\n  Serial.println(\"ready\");\n}\n\n"
               "void loop() {\n  while (Serial.available()) {\n    Serial.write(Serial.read());\n  }\n}\n";
    }
    return "// Blinks the on-board LED once a second.\nconst int led = " + led +
           ";\n\nvoid setup() {\n  pinMode(led, OUTPUT);\n  Serial.begin(115200);\n}\n\n"
           "void loop() {\n  digitalWrite(led, HIGH);\n  delay(500);\n  digitalWrite(led, LOW);\n  delay(500);\n  Serial.println(\"blink\");\n}\n";
}

}  // namespace wetwaresuite::flash
