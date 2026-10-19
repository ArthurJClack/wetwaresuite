#include "launch.hpp"

#include <set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wetwaresuite::apps {

bool isCliKeyword(const std::string& arg) {
    static const std::set<std::string> words = {"list", "help", "run", "batch", "mcp", "--version", "--help", "-h"};
    return words.count(arg) > 0;
}

namespace {

void parseGuiArgs(const std::vector<std::string>& args, std::size_t start, Launch& out) {
    for (std::size_t i = start; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--action" || a == "--screenshot") {
            if (i + 1 >= args.size()) {
                out.error = a + (a == "--action" ? " needs a command, e.g. --action photo.resize" : " needs a file name");
                return;
            }
            (a == "--action" ? out.action : out.screenshot) = args[++i];
        } else if (a == "--all") {
            out.all = true;
        } else if (a.rfind("-psn_", 0) == 0) {
            continue;
        } else if (!a.empty() && a[0] == '-' && a != "-") {
            out.error = "unknown window option " + a + " (the window takes --action ID, --screenshot FILE and --all)";
            return;
        } else {
            out.files.push_back(a);
        }
    }
}

}  // namespace

Launch decideLaunch(const std::vector<std::string>& rawArgs, const std::function<bool(const std::string&)>& isFile,
                    const std::function<bool(const std::string&)>& isCommand) {
    std::vector<std::string> args;
    for (const auto& a : rawArgs)
        if (a.rfind("-psn_", 0) != 0) args.push_back(a);
    Launch out;
    if (args.empty()) {
        out.kind = LaunchKind::Gui;
        return out;
    }
    if (args[0] == "gui" || args[0] == "open") {
        out.kind = LaunchKind::Gui;
        parseGuiArgs(args, 1, out);
        return out;
    }
    if (isCliKeyword(args[0]) || (isCommand && isCommand(args[0]))) return out;
    for (const auto& a : args) {
        if (a.empty() || a[0] == '-' || !isFile || !isFile(a)) return out;
    }
    out.kind = LaunchKind::Gui;
    out.files = args;
    return out;
}

#ifdef _WIN32

void releaseConsoleIfOwned() {
    DWORD ids[2];
    // 1 = only us: Explorer started us and Windows made this console for us alone
    if (GetConsoleProcessList(ids, 2) == 1) FreeConsole();
}

namespace {

std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

}  // namespace

std::string spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args) {
    std::wstring line = quoteArg(program.wstring());
    for (const auto& a : args) line += L" " + quoteArg(std::filesystem::path(a).wstring());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CREATE_NO_WINDOW: the app gets a console nobody sees, frees it and shows only its window
    if (!CreateProcessW(program.wstring().c_str(), line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi)) {
        return "couldn't start " + program.string() + " (Windows error " + std::to_string(GetLastError()) + ")";
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

#else

void releaseConsoleIfOwned() {}

std::string spawnDetached(const std::filesystem::path& program, const std::vector<std::string>& args) {
    // everything the child needs is built before fork: after it only async-signal-safe calls
    const std::string prog = program.string();
    std::vector<std::string> store;
    store.push_back(prog);
    for (const auto& a : args) store.push_back(a);
    std::vector<char*> argv;
    for (auto& s : store) argv.push_back(s.data());
    argv.push_back(nullptr);
    if (access(prog.c_str(), X_OK) != 0) return "couldn't start " + prog + " (it isn't there or can't be run)";

    pid_t child = fork();
    if (child < 0) return "couldn't start " + prog + " (fork failed)";
    if (child == 0) {
        // the middle process starts the real one and leaves at once, so nothing waits on it
        setsid();
        pid_t grandchild = fork();
        if (grandchild == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, 0);
                dup2(devnull, 1);
                dup2(devnull, 2);
            }
            execv(argv[0], argv.data());
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "couldn't start " + prog;
    return {};
}

#endif

}  // namespace wetwaresuite::apps
