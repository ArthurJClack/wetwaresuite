// Stream: a helper program (ffmpeg) we feed raw frames to, or read raw frames
// from, with its stderr kept in a file so the data stays clean.
#include <algorithm>
#include <atomic>
#include <random>

#include "media.hpp"
#include "wetwaresuite/core/process.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace wetwaresuite::cut {

namespace fs = std::filesystem;

namespace {

fs::path stderrPath() {
    static std::atomic<unsigned> counter{0};
    std::random_device rd;
    return fs::temp_directory_path() /
           ("wetwaresuite-cut-err-" + std::to_string(rd()) + "-" + std::to_string(counter.fetch_add(1)) + ".txt");
}

#ifdef _WIN32
std::wstring widenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring quoteArgW(const std::wstring& arg) {
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

struct Stream::Impl {
    fs::path errFile;
#ifdef _WIN32
    HANDLE proc = nullptr, in = nullptr, out = nullptr;
#else
    pid_t pid = -1;
    int in = -1, out = -1;
#endif
    bool running = false;
    int code = -1;
};

Stream::Stream() : impl_(new Impl) {}

Stream::~Stream() {
    stop();
    std::error_code ec;
    if (!impl_->errFile.empty()) fs::remove(impl_->errFile, ec);
    delete impl_;
}

bool Stream::start(const std::vector<std::string>& args, bool writeInput, bool readOutput, std::string& error) {
    if (args.empty()) {
        error = "nothing to run";
        return false;
    }
    auto program = findProgram(args[0]);
    if (!program) {
        error = args[0] == "ffmpeg" ? missingFfmpeg() : "couldn't find " + args[0];
        return false;
    }
    impl_->errFile = stderrPath();
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (writeInput) {
        if (!CreatePipe(&inRead, &inWrite, &sa, 1 << 20)) {
            error = "couldn't make a pipe";
            return false;
        }
        SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    }
    if (readOutput) {
        if (!CreatePipe(&outRead, &outWrite, &sa, 1 << 20)) {
            if (inRead) CloseHandle(inRead), CloseHandle(inWrite);
            error = "couldn't make a pipe";
            return false;
        }
        SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE errHandle = CreateFileW(impl_->errFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    std::wstring cmd = quoteArgW(program->wstring());
    for (std::size_t i = 1; i < args.size(); ++i) cmd += L" " + quoteArgW(widenUtf8(args[i]));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = writeInput ? inRead : nul;
    si.hStdOutput = readOutput ? outWrite : nul;
    si.hStdError = errHandle != INVALID_HANDLE_VALUE ? errHandle : nul;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (inRead) CloseHandle(inRead);
    if (outWrite) CloseHandle(outWrite);
    if (errHandle != INVALID_HANDLE_VALUE) CloseHandle(errHandle);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) {
        if (inWrite) CloseHandle(inWrite);
        if (outRead) CloseHandle(outRead);
        error = "couldn't start " + args[0];
        return false;
    }
    CloseHandle(pi.hThread);
    impl_->proc = pi.hProcess;
    impl_->in = inWrite;
    impl_->out = outRead;
#else
    static const bool ignorePipe = [] {
        signal(SIGPIPE, SIG_IGN);  // writing to a program that has ended must not end us
        return true;
    }();
    (void)ignorePipe;
    int inp[2] = {-1, -1}, outp[2] = {-1, -1};
    if (writeInput && pipe(inp) != 0) {
        error = "couldn't make a pipe";
        return false;
    }
    if (readOutput && pipe(outp) != 0) {
        if (inp[0] >= 0) {
            close(inp[0]);
            close(inp[1]);
        }
        error = "couldn't make a pipe";
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (writeInput) {
        posix_spawn_file_actions_adddup2(&actions, inp[0], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, inp[0]);
        posix_spawn_file_actions_addclose(&actions, inp[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    }
    if (readOutput) {
        posix_spawn_file_actions_adddup2(&actions, outp[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, outp[0]);
        posix_spawn_file_actions_addclose(&actions, outp[1]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    const std::string err = impl_->errFile.string();
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const std::string exe = program->string();
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(exe.c_str()));
    for (std::size_t i = 1; i < args.size(); ++i) argv.push_back(const_cast<char*>(args[i].c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (writeInput) close(inp[0]);
    if (readOutput) close(outp[1]);
    if (rc != 0) {
        if (writeInput) close(inp[1]);
        if (readOutput) close(outp[0]);
        error = "couldn't start " + args[0];
        return false;
    }
    // keep our ends out of programs started later
    if (writeInput) fcntl(inp[1], F_SETFD, FD_CLOEXEC);
    if (readOutput) fcntl(outp[0], F_SETFD, FD_CLOEXEC);
    impl_->pid = pid;
    impl_->in = writeInput ? inp[1] : -1;
    impl_->out = readOutput ? outp[0] : -1;
#endif
    impl_->running = true;
    impl_->code = -1;
    return true;
}

bool Stream::write(const void* data, std::size_t bytes) {
    const char* p = static_cast<const char*>(data);
#ifdef _WIN32
    if (!impl_->in) return false;
    while (bytes > 0) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, std::size_t{1} << 24));
        if (!WriteFile(impl_->in, p, chunk, &n, nullptr) || n == 0) return false;
        p += n;
        bytes -= n;
    }
#else
    if (impl_->in < 0) return false;
    while (bytes > 0) {
        const ssize_t n = ::write(impl_->in, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<std::size_t>(n);
    }
#endif
    return true;
}

bool Stream::read(void* data, std::size_t bytes) {
    char* p = static_cast<char*>(data);
#ifdef _WIN32
    if (!impl_->out) return false;
    while (bytes > 0) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, std::size_t{1} << 24));
        if (!ReadFile(impl_->out, p, chunk, &n, nullptr) || n == 0) return false;
        p += n;
        bytes -= n;
    }
#else
    if (impl_->out < 0) return false;
    while (bytes > 0) {
        const ssize_t n = ::read(impl_->out, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        bytes -= static_cast<std::size_t>(n);
    }
#endif
    return true;
}

void Stream::closeInput() {
#ifdef _WIN32
    if (impl_->in) {
        CloseHandle(impl_->in);
        impl_->in = nullptr;
    }
#else
    if (impl_->in >= 0) {
        close(impl_->in);
        impl_->in = -1;
    }
#endif
}

int Stream::wait() {
    closeInput();
#ifdef _WIN32
    if (impl_->out) {
        CloseHandle(impl_->out);
        impl_->out = nullptr;
    }
    if (!impl_->running) return impl_->code;
    WaitForSingleObject(impl_->proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(impl_->proc, &code);
    CloseHandle(impl_->proc);
    impl_->proc = nullptr;
    impl_->code = static_cast<int>(code);
#else
    if (impl_->out >= 0) {
        close(impl_->out);
        impl_->out = -1;
    }
    if (!impl_->running) return impl_->code;
    int status = 0;
    while (waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
    }
    impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
#endif
    impl_->running = false;
    return impl_->code;
}

void Stream::stop() {
    if (!impl_->running) {
        closeInput();
        return;
    }
#ifdef _WIN32
    TerminateProcess(impl_->proc, 1);
#else
    ::kill(impl_->pid, SIGKILL);
#endif
    wait();
}

std::string Stream::error() const {
    std::string text = impl_->errFile.empty() ? std::string() : readFile(impl_->errFile);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (text.size() > 1500) text = "..." + text.substr(text.size() - 1500);
    return text;
}

}  // namespace wetwaresuite::cut
