// Running helper programs (ffmpeg, LibreOffice, ...) without a shell, so file
// names can never be interpreted as commands.
#pragma once

#include <filesystem>
#include <optional>
#include <utility>
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

namespace wetwaresuite {

// A program running in the background whose output is read as it arrives: what
// a terminal, "run this file" and long builds need. Output is stdout and stderr
// together; input is a pipe the program reads as its stdin. The destructor stops
// the program (and on Windows and POSIX, the programs it started) if it still runs.
class ChildProcess {
public:
    ChildProcess();
    ~ChildProcess();
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    // Starts args[0] (found on PATH) in `cwd` ("" = this process's folder) with
    // `env` added to the environment. False with a plain-word `error` when it can't.
    bool start(const std::vector<std::string>& args, const std::filesystem::path& cwd, std::string& error,
               const std::vector<std::pair<std::string, std::string>>& env = {});
    // Output that arrived since the last call; never blocks.
    std::string read();
    // Waits up to `seconds` for more output or the end; returns what arrived.
    std::string readFor(double seconds);
    bool write(const std::string& text);
    void closeInput();
    bool running();         // false once it has ended (or never started)
    int exitCode() const;   // -1 while running or when it never started
    void kill();            // stops it and everything it started
    // Waits up to `seconds` for it to end, collecting output into `output`. True when it ended.
    bool wait(double seconds, std::string& output);
    long long pid() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace wetwaresuite
