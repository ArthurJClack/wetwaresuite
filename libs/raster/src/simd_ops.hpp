// A four-lane float vector for the per-pixel loops: one RGBA pixel per register.
// SSE2 on x86-64 (always there, no runtime check), NEON on arm64, nothing elsewhere
// (WW_SIMD stays 0 and only the scalar loops are built). Every operation is a plain
// IEEE add/sub/mul/div/min/max, applied in the same order as the scalar loops, so both
// paths give the same bytes (the library builds with -ffp-contract=off so neither side
// fuses multiply-adds on its own).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define WW_SIMD 1
#define WW_SIMD_SSE2 1
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#define WW_SIMD 1
#define WW_SIMD_NEON 1
#else
#define WW_SIMD 0
#endif

namespace wetwaresuite::raster::simd {

#if WW_SIMD_SSE2

struct F4 {
    __m128 v;
};
inline F4 set1(float x) { return {_mm_set1_ps(x)}; }
inline F4 set(float a, float b, float c, float d) { return {_mm_setr_ps(a, b, c, d)}; }
inline F4 operator+(F4 a, F4 b) { return {_mm_add_ps(a.v, b.v)}; }
inline F4 operator-(F4 a, F4 b) { return {_mm_sub_ps(a.v, b.v)}; }
inline F4 operator*(F4 a, F4 b) { return {_mm_mul_ps(a.v, b.v)}; }
inline F4 operator/(F4 a, F4 b) { return {_mm_div_ps(a.v, b.v)}; }
// min(a, b) = b < a ? b : a and max(a, b) = a < b ? b : a, like std::min / std::max (no NaNs here)
inline F4 vmin(F4 a, F4 b) { return {_mm_min_ps(b.v, a.v)}; }
inline F4 vmax(F4 a, F4 b) { return {_mm_max_ps(b.v, a.v)}; }
inline F4 vabs(F4 a) { return {_mm_andnot_ps(_mm_set1_ps(-0.0f), a.v)}; }
inline F4 lessEq(F4 a, F4 b) { return {_mm_cmple_ps(a.v, b.v)}; }  // all-ones lanes where a <= b
inline F4 select(F4 mask, F4 a, F4 b) { return {_mm_or_ps(_mm_and_ps(mask.v, a.v), _mm_andnot_ps(mask.v, b.v))}; }
// lanes 0..2 from a, lane 3 from b
inline F4 withLast(F4 a, F4 b) {
    const __m128 m = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
    return {_mm_or_ps(_mm_and_ps(m, a.v), _mm_andnot_ps(m, b.v))};
}
inline F4 load4(const float* p) { return {_mm_loadu_ps(p)}; }
inline void store4(float* p, F4 a) { _mm_storeu_ps(p, a.v); }
inline F4 loadU8(const std::uint8_t* p) {
    std::int32_t w;
    std::memcpy(&w, p, 4);
    const __m128i z = _mm_setzero_si128();
    const __m128i b = _mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(w), z), z);
    return {_mm_cvtepi32_ps(b)};
}
// Truncates each lane (already in 0..255.99) to a byte, like static_cast<std::uint8_t>(float).
inline void storeU8(std::uint8_t* p, F4 a) {
    const __m128i i = _mm_cvttps_epi32(a.v);
    const __m128i b = _mm_packus_epi16(_mm_packs_epi32(i, i), _mm_setzero_si128());
    const std::int32_t w = _mm_cvtsi128_si32(b);
    std::memcpy(p, &w, 4);
}

// Four unsigned 32-bit lanes (box blur sums).
struct U4 {
    __m128i v;
};
inline U4 loadU32(const std::uint32_t* p) { return {_mm_loadu_si128(reinterpret_cast<const __m128i*>(p))}; }
inline void storeU32(std::uint32_t* p, U4 a) { _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a.v); }
inline U4 setU(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    return {_mm_setr_epi32(static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(d))};
}
inline std::uint32_t lastU(U4 a) { return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_shuffle_epi32(a.v, 0xFF))); }
inline U4 operator+(U4 a, U4 b) { return {_mm_add_epi32(a.v, b.v)}; }
inline U4 operator-(U4 a, U4 b) { return {_mm_sub_epi32(a.v, b.v)}; }
// (r*a, g*a, b*a, a) of one RGBA8 pixel, exact
inline U4 premul(const std::uint8_t* p) {
    std::int32_t w;
    std::memcpy(&w, p, 4);
    const __m128i z = _mm_setzero_si128();
    const __m128i px = _mm_unpacklo_epi8(_mm_cvtsi32_si128(w), z);                   // 16-bit lanes r g b a 0 0 0 0
    const __m128i al = _mm_insert_epi16(_mm_set1_epi16(static_cast<short>(p[3])), 1, 3);  // a a a 1 ...
    const __m128i prod = _mm_mullo_epi16(px, al);                                     // <= 65025, fits 16 bits unsigned
    return {_mm_unpacklo_epi16(prod, z)};
}
// Only for lanes below 2^31 (the callers check), where it matches static_cast<float>(std::uint32_t).
inline F4 toFloat(U4 a) { return {_mm_cvtepi32_ps(a.v)}; }

#elif WW_SIMD_NEON

struct F4 {
    float32x4_t v;
};
inline F4 set1(float x) { return {vdupq_n_f32(x)}; }
inline F4 set(float a, float b, float c, float d) {
    const float t[4] = {a, b, c, d};
    return {vld1q_f32(t)};
}
inline F4 operator+(F4 a, F4 b) { return {vaddq_f32(a.v, b.v)}; }
inline F4 operator-(F4 a, F4 b) { return {vsubq_f32(a.v, b.v)}; }
inline F4 operator*(F4 a, F4 b) { return {vmulq_f32(a.v, b.v)}; }
inline F4 operator/(F4 a, F4 b) { return {vdivq_f32(a.v, b.v)}; }
inline F4 vmin(F4 a, F4 b) { return {vminq_f32(a.v, b.v)}; }
inline F4 vmax(F4 a, F4 b) { return {vmaxq_f32(a.v, b.v)}; }
inline F4 vabs(F4 a) { return {vabsq_f32(a.v)}; }
inline F4 lessEq(F4 a, F4 b) { return {vreinterpretq_f32_u32(vcleq_f32(a.v, b.v))}; }
inline F4 select(F4 mask, F4 a, F4 b) { return {vbslq_f32(vreinterpretq_u32_f32(mask.v), a.v, b.v)}; }
inline F4 withLast(F4 a, F4 b) { return {vsetq_lane_f32(vgetq_lane_f32(b.v, 3), a.v, 3)}; }
inline F4 load4(const float* p) { return {vld1q_f32(p)}; }
inline void store4(float* p, F4 a) { vst1q_f32(p, a.v); }
inline F4 loadU8(const std::uint8_t* p) {
    std::uint32_t w;
    std::memcpy(&w, p, 4);
    const uint16x8_t h = vmovl_u8(vcreate_u8(w));
    return {vcvtq_f32_u32(vmovl_u16(vget_low_u16(h)))};
}
inline void storeU8(std::uint8_t* p, F4 a) {
    const uint32x4_t i = vcvtq_u32_f32(a.v);  // truncates toward zero
    const uint8x8_t b = vmovn_u16(vcombine_u16(vmovn_u32(i), vdup_n_u16(0)));
    const std::uint32_t w = vget_lane_u32(vreinterpret_u32_u8(b), 0);
    std::memcpy(p, &w, 4);
}

struct U4 {
    uint32x4_t v;
};
inline U4 loadU32(const std::uint32_t* p) { return {vld1q_u32(p)}; }
inline void storeU32(std::uint32_t* p, U4 a) { vst1q_u32(p, a.v); }
inline U4 setU(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) {
    const std::uint32_t t[4] = {a, b, c, d};
    return {vld1q_u32(t)};
}
inline std::uint32_t lastU(U4 a) { return vgetq_lane_u32(a.v, 3); }
inline U4 operator+(U4 a, U4 b) { return {vaddq_u32(a.v, b.v)}; }
inline U4 operator-(U4 a, U4 b) { return {vsubq_u32(a.v, b.v)}; }
inline U4 premul(const std::uint8_t* p) {
    std::uint32_t w;
    std::memcpy(&w, p, 4);
    const uint16x4_t px = vget_low_u16(vmovl_u8(vcreate_u8(w)));
    const uint16x4_t al = vset_lane_u16(1, vdup_n_u16(p[3]), 3);
    return {vmull_u16(px, al)};
}
inline F4 toFloat(U4 a) { return {vcvtq_f32_u32(a.v)}; }

#endif

}  // namespace wetwaresuite::raster::simd
