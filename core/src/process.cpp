#include "achclwss/core/process.hpp"

#include <cerrno>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace achclwss {

namespace {

constexpr std::size_t kMaxOutput = 64 * 1024;

void keepTail(std::string& out) {
    if (out.size() > kMaxOutput) out.erase(0, out.size() - kMaxOutput);
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quotes one argument the way CommandLineToArgvW parses it.
std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(slashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}
#endif

}  // namespace

std::optional<std::filesystem::path> findProgram(const std::string& name) {
    namespace fs = std::filesystem;
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
        std::error_code ec;
        if (fs::exists(name, ec)) return fs::path(name);
        return std::nullopt;
    }
    const char* pathEnv = std::getenv("PATH");
    if (!pathEnv) return std::nullopt;
#ifdef _WIN32
    const char sep = ';';
    const std::vector<std::string> suffixes = {".exe", ".com", ".bat", ""};
#else
    const char sep = ':';
    const std::vector<std::string> suffixes = {""};
#endif
    std::string paths = pathEnv;
    std::size_t start = 0;
    while (start <= paths.size()) {
        auto end = paths.find(sep, start);
        std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty()) {
            for (const auto& suffix : suffixes) {
                fs::path candidate = fs::path(dir) / (name + suffix);
                std::error_code ec;
                if (fs::is_regular_file(candidate, ec)) {
#ifndef _WIN32
                    if (access(candidate.c_str(), X_OK) != 0) continue;
#endif
                    return candidate;
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return std::nullopt;
}

ProcessResult runProgram(const std::vector<std::string>& args) {
    ProcessResult result;
    if (args.empty()) return result;
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return result;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quoteArg(widen(a));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return result;
    }
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(readPipe, buf, sizeof buf, &n, nullptr) && n > 0) {
        result.output.append(buf, n);
        keepTail(result.output);
    }
    CloseHandle(readPipe);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    result.exitCode = static_cast<int>(code);
#else
    int fds[2];
    if (pipe(fds) != 0) return result;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", 0, 0);

    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return result;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0) {
        result.output.append(buf, static_cast<std::size_t>(n));
        keepTail(result.output);
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    return result;
}

}  // namespace achclwss
