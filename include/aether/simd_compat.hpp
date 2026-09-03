#pragma once

#include <cmath>
#include <cstddef>

#if defined(__AVX512F__)
#include <immintrin.h>
#define AETHER_SIMD_AVX512 1
#else
#define AETHER_SIMD_AVX512 0
#endif
#if !AETHER_SIMD_AVX512 && defined(__AVX2__)
#include <immintrin.h>
#define AETHER_SIMD_AVX2 1
#else
#define AETHER_SIMD_AVX2 0
#endif
#if !AETHER_SIMD_AVX512 && !AETHER_SIMD_AVX2 && (defined(__ARM_NEON) || defined(__ARM_NEON__))
#include <arm_neon.h>
#define AETHER_SIMD_NEON 1
#else
#define AETHER_SIMD_NEON 0
#endif

namespace aether::simd {

#if AETHER_SIMD_AVX512
using f32x = __m512;
inline constexpr std::size_t width = 16;
inline f32x loadu(const float* p) noexcept { return _mm512_loadu_ps(p); }
inline void storeu(float* p, f32x v) noexcept { _mm512_storeu_ps(p, v); }
inline f32x zero() noexcept { return _mm512_setzero_ps(); }
inline f32x set1(float v) noexcept { return _mm512_set1_ps(v); }
inline f32x add(f32x a, f32x b) noexcept { return _mm512_add_ps(a, b); }
inline f32x sub(f32x a, f32x b) noexcept { return _mm512_sub_ps(a, b); }
inline f32x mul(f32x a, f32x b) noexcept { return _mm512_mul_ps(a, b); }
inline f32x fmadd(f32x a, f32x b, f32x c) noexcept {
#if defined(__FMA__)
    return _mm512_fmadd_ps(a, b, c);
#else
    return add(mul(a, b), c);
#endif
}
inline f32x abs(f32x a) noexcept {
    return _mm512_castsi512_ps(
        _mm512_and_si512(_mm512_castps_si512(a), _mm512_set1_epi32(0x7fffffff)));
}
#elif AETHER_SIMD_AVX2
using f32x = __m256;
inline constexpr std::size_t width = 8;
inline f32x loadu(const float* p) noexcept { return _mm256_loadu_ps(p); }
inline void storeu(float* p, f32x v) noexcept { _mm256_storeu_ps(p, v); }
inline f32x zero() noexcept { return _mm256_setzero_ps(); }
inline f32x set1(float v) noexcept { return _mm256_set1_ps(v); }
inline f32x add(f32x a, f32x b) noexcept { return _mm256_add_ps(a, b); }
inline f32x sub(f32x a, f32x b) noexcept { return _mm256_sub_ps(a, b); }
inline f32x mul(f32x a, f32x b) noexcept { return _mm256_mul_ps(a, b); }
inline f32x fmadd(f32x a, f32x b, f32x c) noexcept {
#if defined(__FMA__)
    return _mm256_fmadd_ps(a, b, c);
#else
    return add(mul(a, b), c);
#endif
}
inline f32x abs(f32x a) noexcept { return _mm256_andnot_ps(_mm256_set1_ps(-0.0f), a); }
#elif AETHER_SIMD_NEON
using f32x = float32x4_t;
inline constexpr std::size_t width = 4;
inline f32x loadu(const float* p) noexcept { return vld1q_f32(p); }
inline void storeu(float* p, f32x v) noexcept { vst1q_f32(p, v); }
inline f32x zero() noexcept { return vdupq_n_f32(0.0f); }
inline f32x set1(float v) noexcept { return vdupq_n_f32(v); }
inline f32x add(f32x a, f32x b) noexcept { return vaddq_f32(a, b); }
inline f32x sub(f32x a, f32x b) noexcept { return vsubq_f32(a, b); }
inline f32x mul(f32x a, f32x b) noexcept { return vmulq_f32(a, b); }
inline f32x fmadd(f32x a, f32x b, f32x c) noexcept {
#if defined(__aarch64__)
    return vfmaq_f32(c, a, b);
#else
    return vaddq_f32(vmulq_f32(a, b), c);
#endif
}
inline f32x abs(f32x a) noexcept { return vabsq_f32(a); }
#else
struct f32x {
    float value;
};
inline constexpr std::size_t width = 1;
inline f32x loadu(const float* p) noexcept { return {*p}; }
inline void storeu(float* p, f32x v) noexcept { *p = v.value; }
inline f32x zero() noexcept { return {0.0f}; }
inline f32x set1(float v) noexcept { return {v}; }
inline f32x add(f32x a, f32x b) noexcept { return {a.value + b.value}; }
inline f32x sub(f32x a, f32x b) noexcept { return {a.value - b.value}; }
inline f32x mul(f32x a, f32x b) noexcept { return {a.value * b.value}; }
inline f32x fmadd(f32x a, f32x b, f32x c) noexcept { return {std::fma(a.value, b.value, c.value)}; }
inline f32x abs(f32x a) noexcept { return {std::abs(a.value)}; }
#endif

inline double horizontal_sum(f32x value) noexcept {
    alignas(64) float lanes[width];
    storeu(lanes, value);
    double result = 0.0;
    for (std::size_t i = 0; i < width; ++i) result += lanes[i];
    return result;
}

inline void abs_array(const float* input, float* output, std::size_t count) noexcept {
    std::size_t i = 0;
    for (; i + width <= count; i += width) storeu(output + i, abs(loadu(input + i)));
    for (; i < count; ++i) output[i] = std::abs(input[i]);
}

}  // namespace aether::simd
