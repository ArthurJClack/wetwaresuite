// Terminals: each command runs in a fresh shell process from a small script
// that first restores the folder and exported variables the last command left,
// and ends by printing a marker with the exit code and the new folder.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

#include "wetwaresuite/sysutil/sysutil.hpp"
#include "wetwaresuite/terminal/terminal.hpp"

namespace wetwaresuite::terminal {

namespace {

constexpr const char* kMarker = "__WW_DONE__ ";
constexpr const char* kEnd = " __WW_END__";
constexpr std::size_t kMaxScroll = 2u << 20;

double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::string single(const std::string& s) {  // 'quoted' for sh
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::string psQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("''") : std::string(1, c);
    return out + "'";
}

std::string native(const fs::path& p) {
#ifdef _WIN32
    std::string s = sysutil::fromPath(p);
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
#else
    return sysutil::fromPath(p);
#endif
}

std::string script(const ShellInfo& sh, const std::string& command, const fs::path& cwd, const fs::path& env) {
    if (sh.family == "cmd") {
        // plain lines only: a syntax error anywhere stops a batch file before its first line runs
        return "@echo off\r\nif not exist \"" + native(env) + "\" goto __ww_go\r\nfor /f \"usebackq delims=\" %%a in (\"" + native(env) +
               "\") do set \"%%a\"\r\n:__ww_go\r\ncd /d \"" + native(cwd) + "\"\r\n" + command + "\r\nset __ww_status=%errorlevel%\r\nset > \"" + native(env) +
               "\"\r\necho.\r\necho __WW_DONE__ %__ww_status% %cd% __WW_END__\r\n";
    }
    if (sh.family == "powershell") {
        return "$ProgressPreference = 'SilentlyContinue'\nSet-Location -LiteralPath " + psQuote(native(cwd)) + "\n$global:LASTEXITCODE = $null\n" + command +
               "\n$__ok = $?\n$__ww = if ($null -ne $global:LASTEXITCODE) { $global:LASTEXITCODE } elseif ($__ok) { 0 } else { 1 }\n"
               "Write-Output \"`n__WW_DONE__ $__ww $((Get-Location).Path) __WW_END__\"\n";
    }
    return "[ -r " + single(sysutil::fromPath(env)) + " ] && . " + single(sysutil::fromPath(env)) + " >/dev/null 2>&1\ncd -- " + single(sysutil::fromPath(cwd)) +
           " 2>/dev/null || cd\n" + command + "\n__ww_status=$?\nexport -p > " + single(sysutil::fromPath(env)) +
           " 2>/dev/null\nprintf '\\n__WW_DONE__ %s %s __WW_END__\\n' \"$__ww_status\" \"$(pwd)\"\n";
}

std::vector<std::string> launchArgs(const ShellInfo& sh, const fs::path& file) {
    if (sh.family == "cmd") return {sh.program, "/d", "/q", "/c", native(file)};
    if (sh.family == "powershell") return {sh.program, "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", native(file)};
    return {sh.program, sysutil::fromPath(file)};
}

const char* scriptExt(const ShellInfo& sh) { return sh.family == "cmd" ? ".bat" : sh.family == "powershell" ? ".ps1" : ".sh"; }

}  // namespace

std::vector<ShellInfo> findShells() {
    std::vector<ShellInfo> out;
    auto add = [&](const std::string& id, const std::string& family) {
        auto p = findProgram(id);
        if (!p) return;
        for (const auto& s : out) {
            if (s.id == id) return;
        }
        out.push_back({id, sysutil::fromPath(*p), family});
    };
#ifdef _WIN32
    add("cmd", "cmd");
    add("powershell", "powershell");
    add("pwsh", "powershell");
    add("bash", "posix");
#else
    std::string mine = sysutil::nameOf(sysutil::toPath(sysutil::env("SHELL")));
    if (mine == "bash" || mine == "zsh" || mine == "sh" || mine == "dash" || mine == "ksh") add(mine, "posix");
    for (const char* s : {"bash", "zsh", "sh", "dash", "ksh"}) add(s, "posix");
    add("pwsh", "powershell");
#endif
    return out;
}

ShellInfo pickShell(const std::string& want) {
    auto shells = findShells();
    if (shells.empty()) throw std::runtime_error("no shell found on PATH");
    if (want.empty()) return shells.front();
    for (const auto& s : shells) {
        if (s.id == want) return s;
    }
    std::string names;
    for (const auto& s : shells) names += (names.empty() ? "" : ", ") + s.id;
    throw std::runtime_error("shell '" + want + "' isn't installed (found: " + names + ")");
}

std::string cleanOutput(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    std::size_t lineStart = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '\x1b') {  // CSI ... letter, OSC ... BEL/ST, or a two-byte escape
            if (i + 1 < raw.size() && raw[i + 1] == '[') {
                i += 2;
                while (i < raw.size() && !(raw[i] >= '@' && raw[i] <= '~')) ++i;
            } else if (i + 1 < raw.size() && raw[i + 1] == ']') {
                i += 2;
                while (i < raw.size() && raw[i] != '\a' && !(raw[i] == '\x1b' && i + 1 < raw.size() && raw[i + 1] == '\\')) ++i;
                if (i < raw.size() && raw[i] == '\x1b') ++i;
            } else {
                ++i;
            }
            continue;
        }
        if (c == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
            out.resize(lineStart);  // the line is about to be written again
            continue;
        }
        if (c == '\b') {
            if (out.size() > lineStart) out.pop_back();
            continue;
        }
        if (static_cast<unsigned char>(c) < 32 && c != '\n' && c != '\t') continue;
        out += c;
        if (c == '\n') lineStart = out.size();
    }
    return out;
}

TerminalDoc::TerminalDoc(ShellInfo shell, fs::path cwd) : shell_(std::move(shell)), cwd_(std::move(cwd)) {
    static std::atomic<int> counter{0};
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "wetwaresuite-terminal";
    fs::create_directories(dir, ec);
    const std::string tag = std::to_string(static_cast<long long>(now() * 1000) % 100000000) + "-" + std::to_string(++counter);
    envFile_ = dir / ("env-" + tag + ".txt");
    scriptFile_ = dir / ("run-" + tag + scriptExt(shell_));
}

TerminalDoc::~TerminalDoc() {
    if (proc_) proc_->kill();
    std::error_code ec;
    fs::remove(envFile_, ec);
    fs::remove(scriptFile_, ec);
}

std::string TerminalDoc::summary() const {
    return shell_.id + " in " + sysutil::fromPath(cwd_) + (running_ ? ", running: " + (history_.empty() ? std::string() : history_.back().command) : "") + ", " +
           sysutil::plural(history_.size(), "command");
}

void TerminalDoc::append(const std::string& text) {
    scroll_ += text;
    if (scroll_.size() > kMaxScroll) {
        std::size_t cut = scroll_.size() - kMaxScroll / 2;
        cut = scroll_.find('\n', cut) == std::string::npos ? cut : scroll_.find('\n', cut) + 1;
        scroll_.erase(0, cut);
        dropped_ += cut;
    }
}

bool TerminalDoc::start(const std::string& command, std::string& error) {
    if (running_) {
        error = "a command is still running (terminal.send types into it, terminal.interrupt stops it)";
        return false;
    }
    std::string cmd = command;
    while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
    if (sysutil::trim(cmd).empty()) {
        error = "nothing to run";
        return false;
    }
    try {
        sysutil::writeFile(scriptFile_, script(shell_, cmd, cwd_, envFile_));
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    proc_ = std::make_unique<ChildProcess>();
    if (!proc_->start(launchArgs(shell_, scriptFile_), cwd_, error,
                      {{"TERM", "dumb"}, {"PAGER", "cat"}, {"GIT_PAGER", "cat"}, {"NO_COLOR", "1"}, {"CLICOLOR", "0"}})) {
        proc_.reset();
        return false;
    }
    history_.push_back({cmd, -1, sysutil::fromPath(cwd_), 0});
    append(sysutil::nameOf(cwd_) + " $ " + cmd + "\n");
    current_.clear();
    pending_.clear();
    running_ = true;
    started_ = now();
    return true;
}

void TerminalDoc::finish(int code, const std::string& cwd) {
    running_ = false;
    lastExit_ = code;
    if (!cwd.empty()) {
        std::error_code ec;
        fs::path next = sysutil::toPath(cwd);
        if (fs::is_directory(next, ec)) cwd_ = next;
    }
    if (!history_.empty()) {
        history_.back().exitCode = code;
        history_.back().seconds = now() - started_;
    }
    if (!scroll_.empty() && scroll_.back() != '\n') append("\n");
    if (code != 0) append("[exit code " + std::to_string(code) + "]\n");
    proc_.reset();
}

bool TerminalDoc::pump() {
    if (!running_ || !proc_) return false;
    pending_ += proc_->read();
    const std::size_t m = pending_.find(kMarker);
    if (m != std::string::npos) {
        const std::size_t e = pending_.find(kEnd, m);
        if (e != std::string::npos) {
            std::string before = pending_.substr(0, m);
            if (!before.empty() && before.back() == '\n') before.pop_back();
            if (!before.empty() && before.back() == '\r') before.pop_back();
            const std::string clean = cleanOutput(before);
            current_ += clean;
            append(clean);
            std::string info = pending_.substr(m + std::char_traits<char>::length(kMarker), e - m - std::char_traits<char>::length(kMarker));
            const std::size_t sp = info.find(' ');
            int code = std::atoi(info.substr(0, sp).c_str());
            std::string cwd = sp == std::string::npos ? std::string() : sysutil::trim(info.substr(sp + 1));
            pending_.clear();
            std::string rest;
            proc_->wait(2, rest);
            finish(code, cwd);
            return false;
        }
    }
    // show whole lines now, keeping back the last line break (it may belong to the marker)
    const std::size_t nl = pending_.rfind('\n');
    if (nl != std::string::npos && nl > 0 && m == std::string::npos) {
        // keep a \r of a Windows \r\n with its \n: alone at the end it reads as "rewrite this line"
        const std::size_t cut = pending_[nl - 1] == '\r' ? nl - 1 : nl;
        const std::string chunk = cleanOutput(pending_.substr(0, cut));
        current_ += chunk;
        append(chunk);
        pending_.erase(0, cut);
    }
    if (!proc_->running()) {
        pending_ += proc_->read();
        if (pending_.find(kMarker) != std::string::npos) return pump();
        const std::string clean = cleanOutput(pending_);
        current_ += clean;
        append(clean);
        pending_.clear();
        finish(proc_->exitCode(), {});  // `exit` ended the script before the marker
        return false;
    }
    return true;
}

bool TerminalDoc::wait(double seconds) {
    const double until = now() + seconds;
    while (pump()) {
        if (now() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

bool TerminalDoc::send(const std::string& text, bool echo) {
    if (!running_ || !proc_) return false;
    if (!proc_->write(text)) return false;
    if (echo) {
        append(text);
        current_ += text;
    }
    return true;
}

void TerminalDoc::closeInput() {
    if (proc_) proc_->closeInput();
}

void TerminalDoc::interrupt() {
    if (!running_ || !proc_) return;
    proc_->kill();
    std::string rest;
    proc_->wait(2, rest);
    pending_ += rest;
    const std::size_t m = pending_.find(kMarker);
    if (m != std::string::npos) pending_.resize(m);
    const std::string clean = cleanOutput(pending_);
    current_ += clean;
    append(clean);
    pending_.clear();
    if (!scroll_.empty() && scroll_.back() != '\n') append("\n");
    append("[stopped]\n");
    running_ = false;
    lastExit_ = 130;
    if (!history_.empty()) {
        history_.back().exitCode = 130;
        history_.back().seconds = now() - started_;
    }
    proc_.reset();
}

void TerminalDoc::clear() {
    dropped_ += scroll_.size();
    scroll_.clear();
    readPos_ = dropped_;
}

std::string TerminalDoc::takeNew() {
    const std::size_t total = dropped_ + scroll_.size();
    std::string out;
    if (readPos_ < dropped_) readPos_ = dropped_;
    if (readPos_ < total) out = scroll_.substr(readPos_ - dropped_);
    readPos_ = total;
    return out;
}

ExecResult exec(const std::string& command, const fs::path& cwd, const ShellInfo& shell, double timeout) {
    TerminalDoc t(shell, cwd);
    ExecResult r;
    std::string error;
    if (!t.start(command, error)) throw std::runtime_error(error);
    t.closeInput();  // nothing to type: a program waiting for input sees the end at once
    if (!t.wait(timeout)) {
        t.interrupt();
        r.timedOut = true;
    }
    r.exitCode = t.lastExitCode();
    r.output = t.commandOutput();
    r.cwd = sysutil::fromPath(t.cwd());
    return r;
}

std::vector<std::string> complete(const std::string& line, const fs::path& cwd) {
    const std::size_t sp = line.find_last_of(" \t");
    const std::string word = sp == std::string::npos ? line : line.substr(sp + 1);
    std::string dirPart, prefix = word;
    const std::size_t slash = word.find_last_of("/\\");
    if (slash != std::string::npos) {
        dirPart = word.substr(0, slash + 1);
        prefix = word.substr(slash + 1);
    }
    fs::path dir = dirPart.empty() ? cwd : (dirPart[0] == '~' ? sysutil::homeDir() / sysutil::toPath(dirPart.substr(std::min<std::size_t>(2, dirPart.size()))) : sysutil::toPath(dirPart));
    if (dir.is_relative()) dir = cwd / dir;
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = sysutil::nameOf(e.path());
        if (name.size() < prefix.size()) continue;
#ifdef _WIN32
        if (sysutil::lowerAscii(name.substr(0, prefix.size())) != sysutil::lowerAscii(prefix)) continue;
#else
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
#endif
        if (prefix.empty() && !name.empty() && name[0] == '.') continue;
        std::error_code e2;
        out.push_back(dirPart + name + (e.is_directory(e2) ? "/" : ""));
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace wetwaresuite::terminal
