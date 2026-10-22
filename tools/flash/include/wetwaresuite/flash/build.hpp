// Building firmware with whatever toolchain is installed: arduino-cli (sketches), PlatformIO
// (projects with platformio.ini) or avr-gcc (plain C/C++ for AVR boards). Nothing is bundled:
// when a tool is missing the plan says so and nothing runs.
#pragma once

#include <string>
#include <vector>

namespace wetwaresuite::flash {

struct Toolchain {
    std::string name;     // arduino-cli, pio, avr-gcc, avr-objcopy, arm-none-eabi-gcc, xtensa-esp32-elf-gcc
    std::string program;  // what was found on PATH ("" when missing)
    std::string purpose;
};
// Every toolchain we know of, found or not (program empty when missing).
std::vector<Toolchain> toolchains();

struct CompileRequest {
    std::string source;        // sketch folder or .ino, PlatformIO project folder, or .c/.cpp files (comma-separated)
    std::string board = "uno";  // a board preset (flash.boards) or, for arduino-cli, a full FQBN (arduino:avr:uno)
    std::string tool = "auto";  // auto, arduino-cli, pio, avr-gcc
    std::string outDir;        // default: <source>/build
    std::string env;           // PlatformIO environment (default: all)
    std::string defines;       // avr-gcc: extra -D flags, comma-separated (F_CPU is set from the board)
};

struct CompilePlan {
    std::string tool;                             // which toolchain
    std::vector<std::vector<std::string>> steps;  // programs to run, in order (args[0] = program)
    std::string output;                           // the firmware file it makes (may be a guess for PlatformIO)
    std::string board;                            // board preset to flash it with
    std::string error;                            // why it can't (then steps is empty)
    bool installed = false;                       // every program of the steps is on PATH
};
// Works out the commands without running anything.
CompilePlan planCompile(const CompileRequest& request);

// The FQBN arduino-cli wants for a board preset ("uno" -> "arduino:avr:uno"); "" if unknown.
std::string fqbnFor(const std::string& board);

// A starter sketch: blink, serial (echo) or empty. Returns the .ino text.
std::string sketchTemplate(const std::string& kind, const std::string& board);

}  // namespace wetwaresuite::flash
