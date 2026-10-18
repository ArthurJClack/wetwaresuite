// Minimal test helper so the core has no test-framework dependency.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

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

inline const bool installed = [] {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate(onTerminate);
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
