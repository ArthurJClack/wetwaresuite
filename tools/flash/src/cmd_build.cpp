// flash.toolchains, flash.sketch and flash.compile: the Arduino IDE / PlatformIO "verify and
// upload" loop, through whichever toolchain is installed.
#include <filesystem>
#include <fstream>

#include "wetwaresuite/core/process.hpp"
#include "wetwaresuite/flash/build.hpp"
#include "wetwaresuite/flash/flash.hpp"

namespace wetwaresuite::flash {

namespace fs = std::filesystem;

namespace {

bool given(const Params& p, const std::string& key) {
    auto it = p.find(key);
    return it != p.end() && !std::holds_alternative<std::monostate>(it->second);
}

std::string commandLine(const std::vector<std::string>& args) {
    std::string s;
    for (const auto& a : args) {
        const bool quote = a.find(' ') != std::string::npos;
        s += (s.empty() ? "" : " ") + (quote ? "\"" + a + "\"" : a);
    }
    return s;
}

std::string tail(const std::string& text, std::size_t lines) {
    std::size_t pos = text.size(), n = 0;
    while (pos > 0 && n <= lines) {
        pos = text.rfind('\n', pos - 1);
        if (pos == std::string::npos) return text;
        ++n;
    }
    return text.substr(pos + 1);
}

const char* installHint(const std::string& tool) {
    if (tool == "arduino-cli") return "install arduino-cli (https://arduino.github.io/arduino-cli) and its core, e.g. arduino-cli core install arduino:avr";
    if (tool == "pio") return "install PlatformIO Core (pip install platformio)";
    return "install the AVR toolchain (avr-gcc, avr-libc and binutils; e.g. apt install gcc-avr avr-libc, brew install avr-gcc)";
}

}  // namespace

void registerBuildCommands(Session& session) {
    auto& reg = session.registry();
    reg.add({"flash.toolchains", "Build Toolchains",
             "Show which firmware toolchains are installed: arduino-cli (Arduino sketches), PlatformIO (pio), avr-gcc/avr-objcopy "
             "(plain C for AVR), arm-none-eabi-gcc, ESP-IDF. flash.compile uses them; none ships with wetwaresuite.",
             {}, false, true},
            [](const Params&) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([] {
                    std::string s;
                    int found = 0;
                    Result r = Result::success("");
                    for (const auto& t : toolchains()) {
                        s += "\n" + t.name + ": " + (t.program.empty() ? "not installed" : t.program) + " (" + t.purpose + ")";
                        found += !t.program.empty();
                        r.with(t.name, !t.program.empty());
                    }
                    r.message = std::to_string(found) + " toolchain(s) installed" + s;
                    r.with("found", static_cast<std::int64_t>(found));
                    return r;
                });
            });

    reg.add({"flash.sketch", "New Sketch",
             "Start an Arduino sketch: makes the folder <path>/<name>.ino (Arduino needs the folder and the file to share a name) "
             "from a template: blink (the on-board LED), serial (echo) or empty. Build it with flash.compile.",
             {{"path", ParamType::String, "sketch folder, e.g. sketches/blink", true},
              {"template", ParamType::String, "what it starts with", false, {"blink", "serial", "empty"}, std::string("blink")},
              {"board", ParamType::String, "board it is for (picks the LED pin)", false, {}, std::string("uno")},
              {"overwrite", ParamType::Bool, "replace an existing sketch", false, {}, false}},
             false},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    fs::path dir = stringParam(p, "path");
                    if (dir.extension() == ".ino") dir = dir.parent_path() / dir.stem();
                    const fs::path file = dir / (dir.filename().string() + ".ino");
                    std::error_code ec;
                    if (fs::exists(file, ec) && !boolParam(p, "overwrite", false)) return Result::failure(file.string() + " already exists (overwrite=true replaces it)");
                    fs::create_directories(dir, ec);
                    std::ofstream out(file, std::ios::binary);
                    out << sketchTemplate(stringParam(p, "template", "blink"), stringParam(p, "board", "uno"));
                    if (!out) return Result::failure("can't write " + file.string());
                    Result r = Result::success("wrote " + file.string());
                    r.with("path", file.string()).with("folder", dir.string());
                    return r;
                });
            });

    reg.add({"flash.compile", "Compile Firmware",
             "Build firmware with the installed toolchain and (with port=) upload it, like Arduino's Verify/Upload: an Arduino "
             "sketch (folder or .ino) through arduino-cli, a PlatformIO project (folder with platformio.ini) through pio, or plain "
             ".c/.cpp files for AVR boards through avr-gcc. dry_run=true shows the commands without running them. When the "
             "toolchain isn't installed it says how to get it and runs nothing (flash.toolchains lists what is there).",
             {{"source", ParamType::String, "sketch folder or .ino, PlatformIO project folder, or .c/.cpp files (comma-separated)", true},
              {"board", ParamType::String, "board preset (flash.boards) or an Arduino FQBN such as arduino:avr:uno", false, {}, std::string("uno")},
              {"tool", ParamType::String, "toolchain", false, {"auto", "arduino-cli", "pio", "avr-gcc"}, std::string("auto")},
              {"out_dir", ParamType::String, "where the build goes (default: <source>/build)", false},
              {"env", ParamType::String, "PlatformIO environment", false},
              {"defines", ParamType::String, "avr-gcc: extra defines, comma-separated (LED=13,DEBUG)", false},
              {"dry_run", ParamType::Bool, "only show the commands", false, {}, false},
              {"port", ParamType::String, "upload to this port afterwards (as flash.flash; sim:uno ... work too)", false}},
             false, true},
            [](const Params& p) -> std::unique_ptr<Action> {
                return std::make_unique<FunctionAction>([p] {
                    CompileRequest req;
                    req.source = stringParam(p, "source");
                    req.board = stringParam(p, "board", "uno");
                    req.tool = stringParam(p, "tool", "auto");
                    req.outDir = stringParam(p, "out_dir");
                    req.env = stringParam(p, "env");
                    req.defines = stringParam(p, "defines");
                    const CompilePlan plan = planCompile(req);
                    if (!plan.error.empty()) return Result::failure(plan.error);
                    std::string cmds;
                    for (const auto& s : plan.steps) cmds += "\n  " + commandLine(s);
                    if (boolParam(p, "dry_run", false)) {
                        Result r = Result::success("would run (" + plan.tool + (plan.installed ? "" : ", not installed") + "):" + cmds + "\nfirmware: " + plan.output);
                        r.with("tool", plan.tool).with("installed", plan.installed).with("firmware", plan.output).with("board", plan.board);
                        return r;
                    }
                    if (!plan.installed) {
                        Result r = Result::failure(plan.tool + " isn't installed, so nothing was built: " + installHint(plan.tool) +
                                                   ". The commands would be:" + cmds);
                        r.with("tool", plan.tool).with("installed", false);
                        return r;
                    }
                    std::string log;
                    for (const auto& s : plan.steps) {
                        const ProcessResult pr = runProgram(s);
                        log += pr.output;
                        if (pr.exitCode != 0) {
                            Result r = Result::failure("build failed (" + commandLine(s) + " exited with " + std::to_string(pr.exitCode) + "):\n" + tail(pr.output, 40));
                            r.with("tool", plan.tool).with("log", log);
                            return r;
                        }
                    }
                    std::error_code ec;
                    if (!fs::exists(plan.output, ec)) {
                        Result r = Result::failure("the build ran but " + plan.output + " isn't there; look in the log for where it went");
                        r.with("log", log);
                        return r;
                    }
                    std::string msg = "built " + plan.output + " (" + std::to_string(fs::file_size(plan.output, ec)) + " bytes) with " + plan.tool;
                    Result r = Result::success(msg);
                    r.with("tool", plan.tool).with("firmware", plan.output).with("board", plan.board).with("log", tail(log, 20));
                    if (given(p, "port")) {
                        FlashRequest fr;
                        fr.port = stringParam(p, "port");
                        fr.file = plan.output;
                        fr.board = plan.board;
                        try {
                            const FlashReport rep = flashPort(fr);
                            r.message += "\nuploaded " + std::to_string(rep.bytes) + " bytes to " + fr.port + (rep.verified ? ", verified" : "");
                            r.with("uploaded", true);
                        } catch (const std::exception& e) {
                            return Result::failure(msg + "\nupload failed: " + e.what());
                        }
                    }
                    return r;
                });
            });
}

}  // namespace wetwaresuite::flash
