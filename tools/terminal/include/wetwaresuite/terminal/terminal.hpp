// terminal: run commands in the system's shell (bash, zsh, sh, cmd or
// PowerShell) from the suite, in the GUI or headless. Each terminal (term1)
// keeps its folder and exported variables between commands, keeps a scrollback
// and a history, and can type into a running program or stop it. Every command
// runs as its own process, so stopping one never loses the terminal.
// There is no pseudo-terminal: full-screen programs (vim, top) are not supported;
// colours are stripped and progress lines collapsed so output reads cleanly.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "wetwaresuite/core/command.hpp"
#include "wetwaresuite/core/process.hpp"

namespace wetwaresuite::terminal {

namespace fs = std::filesystem;

struct ShellInfo {
    std::string id;       // bash, zsh, sh, cmd, powershell, pwsh
    std::string program;  // full path
    std::string family;   // posix, cmd, powershell
};
std::vector<ShellInfo> findShells();
// $SHELL (or the first of bash, zsh, sh) on macOS and Linux, cmd on Windows; `want` picks another.
ShellInfo pickShell(const std::string& want = {});

// Escape codes removed, "\r\n" made "\n", and a line rewritten with "\r" keeps only its last version.
std::string cleanOutput(const std::string& raw);

struct HistoryItem {
    std::string command;
    int exitCode = -1;
    std::string cwd;
    double seconds = 0;
};

class TerminalDoc : public Document {
public:
    TerminalDoc(ShellInfo shell, fs::path cwd);
    ~TerminalDoc() override;

    std::string kind() const override { return "terminal"; }
    std::string summary() const override;

    // Starts a command; false (with error) when one is still running.
    bool start(const std::string& command, std::string& error);
    // Reads what has arrived; true while the command is still running.
    bool pump();
    // Waits up to `seconds` for the command to finish; true when it did.
    bool wait(double seconds);
    // types into the running command; echo shows it in the output as a terminal would
    bool send(const std::string& text, bool echo = true);
    void closeInput();
    void interrupt();
    void clear();

    bool busy() const { return running_; }
    const ShellInfo& shell() const { return shell_; }
    const fs::path& cwd() const { return cwd_; }
    const std::string& scrollback() const { return scroll_; }
    const std::vector<HistoryItem>& history() const { return history_; }
    int lastExitCode() const { return lastExit_; }
    // output of the current or last command
    std::string commandOutput() const { return cleanOutput(current_); }
    // what was added to the scrollback since the last call
    std::string takeNew();

private:
    void finish(int code, const std::string& cwd);
    void append(const std::string& text);

    ShellInfo shell_;
    fs::path cwd_;
    fs::path envFile_;
    fs::path scriptFile_;
    std::unique_ptr<ChildProcess> proc_;
    bool running_ = false;
    std::string pending_;  // raw output not yet shown (might hold half a marker)
    std::string current_;  // output of the running/last command
    std::string scroll_;
    std::size_t readPos_ = 0;
    std::size_t dropped_ = 0;
    std::vector<HistoryItem> history_;
    int lastExit_ = 0;
    double started_ = 0;
};

// Runs one command and waits; for scripts that don't need a terminal kept open.
struct ExecResult {
    int exitCode = -1;
    std::string output;
    std::string cwd;
    bool timedOut = false;
};
ExecResult exec(const std::string& command, const fs::path& cwd, const ShellInfo& shell, double timeout);

// File and folder names in `cwd` that complete the last word of `line`.
std::vector<std::string> complete(const std::string& line, const fs::path& cwd);

void registerCommands(Session& session);

}  // namespace wetwaresuite::terminal
