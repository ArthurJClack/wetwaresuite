// Running helper programs (ffmpeg, LibreOffice, ...) without a shell, so file
// names can never be interpreted as commands.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace wetwaresuite {

// Finds an executable on PATH (adds .exe on Windows). Empty if missing.
std::optional<std::filesystem::path> findProgram(const std::string& name);

struct ProcessResult {
    int exitCode = -1;  // -1 when the program couldn't start
    std::string output;  // stdout and stderr together, truncated to the last 64 KiB
};

// Runs args[0] with the remaining args and waits for it.
ProcessResult runProgram(const std::vector<std::string>& args);

}  // namespace wetwaresuite
