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

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (expr) {                                                              \
            ++check::passed;                                                     \
        } else {                                                                 \
            ++check::failures;                                                   \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
        }                                                                        \
    } while (0)
