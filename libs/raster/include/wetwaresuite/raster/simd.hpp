// Hand-written SIMD for the hottest pixel loops (layer blending, box blur, resize,
// colour adjust): SSE2 on x86-64, NEON on arm64, both part of the baseline CPU, so
// nothing is detected at runtime. Every SIMD loop has a scalar twin that gives exactly
// the same bytes; switching off SIMD (tests, `photo.benchmark simd=false`) runs those.
#pragma once

namespace wetwaresuite::raster {

bool simdAvailable();          // false on CPUs without SSE2/NEON builds: everything is scalar
const char* simdName();        // "sse2", "neon" or "scalar"
bool simdEnabled();            // available and not switched off
bool setSimdEnabled(bool on);  // returns the previous setting

}  // namespace wetwaresuite::raster
