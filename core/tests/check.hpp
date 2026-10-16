// Minimal test helper so the core has no test-framework dependency.
#pragma once

#include <cstdio>

namespace check {
inline int failures = 0;
inline int passed = 0;

inline int summary(const char* suite) {
    std::printf("%s: %d passed, %d failed\n", suite, passed, failures);
    return failures == 0 ? 0 : 1;
}
}  // namespace check

#define CHECK(...)                                                              \
    do {                                                                         \
        if (__VA_ARGS__) {                                                              \
            ++check::passed;                                                     \
        } else {                                                                 \
            ++check::failures;                                                   \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #__VA_ARGS__);        \
        }                                                                        \
    } while (0)
