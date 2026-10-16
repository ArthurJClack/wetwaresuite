// Minimal test helper so the core has no test-framework dependency.
#pragma once

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <initializer_list>

#ifdef _WIN32
// the two kernel32 calls below, declared exactly as <windows.h> does so tests don't pull in its macros
struct _EXCEPTION_POINTERS;
extern "C" {
__declspec(dllimport) long(__stdcall* __stdcall SetUnhandledExceptionFilter(long(__stdcall*)(_EXCEPTION_POINTERS*)))(
    _EXCEPTION_POINTERS*);
__declspec(dllimport) int __stdcall SetThreadStackGuarantee(unsigned long*);
}
#endif

namespace check {
inline int failures = 0;
inline int passed = 0;
inline const char* lastFile = "";  // the last CHECK reached, reported if the test dies
inline int lastLine = 0;

// An uncaught exception or abort otherwise kills the test with no output (on Windows just
// 0xc0000409), so say what was thrown and how far the test got.
inline void onTerminate() {
    const char* what = "unknown exception";
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        what = "std::terminate without an exception";
    } catch (const std::exception& e) {
        std::fprintf(stderr, "uncaught exception: %s\n", e.what());
        what = nullptr;
    } catch (...) {
    }
    if (what) std::fprintf(stderr, "%s\n", what);
    std::fprintf(stderr, "last check reached: %s:%d\n", lastFile, lastLine);
    std::fflush(stdout);
    std::fflush(stderr);
    std::abort();
}

// a crash otherwise shows only "SegFault" with no output, so say how far the test got
// (not async-signal-safe, but good enough to point at the failing area of a test)
inline void onSignal(int sig) {
    std::fprintf(stderr, "crashed with signal %d; last check reached: %s:%d\n", sig, lastFile, lastLine);
    std::fflush(stderr);
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

#ifdef _WIN32
// stack overflows and crashes on other threads skip signal handlers on Windows; this sees them all
inline long __stdcall onException(_EXCEPTION_POINTERS* info) {
    // EXCEPTION_POINTERS starts with the EXCEPTION_RECORD pointer, which starts with the code
    const unsigned long code = **reinterpret_cast<unsigned long* const*>(info);
    std::fprintf(stderr, "crashed with exception 0x%08lx; last check reached: %s:%d\n", code, lastFile, lastLine);
    std::fflush(stderr);
    return 0;  // EXCEPTION_CONTINUE_SEARCH
}
#endif

inline const bool installed = [] {
#ifdef _WIN32
    unsigned long guarantee = 64 * 1024;  // room for the handler after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    SetUnhandledExceptionFilter(onException);
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate(onTerminate);
    for (int sig : {SIGSEGV, SIGFPE, SIGILL}) std::signal(sig, onSignal);
    return true;
}();

inline int summary(const char* suite) {
    std::printf("%s: %d passed, %d failed\n", suite, passed, failures);
    return failures == 0 ? 0 : 1;
}
}  // namespace check

#define CHECK(...)                                                              \
    do {                                                                         \
        check::lastFile = __FILE__;                                              \
        check::lastLine = __LINE__;                                              \
        if (__VA_ARGS__) {                                                              \
            ++check::passed;                                                     \
        } else {                                                                 \
            ++check::failures;                                                   \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);        \
        }                                                                        \
    } while (0)
