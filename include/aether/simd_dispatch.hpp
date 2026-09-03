#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define AETHER_DISPATCH_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#else
#define AETHER_DISPATCH_X86 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#define AETHER_DISPATCH_ARM64 1
#include <arm_neon.h>
#else
#define AETHER_DISPATCH_ARM64 0
#endif
#if defined(__linux__) && AETHER_DISPATCH_ARM64
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif
#if defined(__APPLE__) && AETHER_DISPATCH_ARM64
#include <sys/sysctl.h>
#endif

#if (defined(__GNUC__) || defined(__clang__)) && AETHER_DISPATCH_X86
#define AETHER_TARGET_AVX2 __attribute__((target("avx2,fma")))
#define AETHER_TARGET_AVX512 __attribute__((target("avx512f,avx512dq,fma")))
#else
#define AETHER_TARGET_AVX2
#define AETHER_TARGET_AVX512
#endif

namespace aether::simd {

enum class Backend : uint8_t { SCALAR, NEON, AVX2, AVX512 };
using MomentFunction = void (*)(const float*, std::size_t, float&, float&);
using QuantizeFunction = void (*)(const float*, const float*, std::size_t, uint8_t*, std::size_t);

struct PiecewisePolynomialQuantizer {
    // Four cubic coefficient rows in c0,c1,c2,c3 order. Epsilon is local to
    // each of the symmetric normalized intervals [-1,-.5],[-.5,0],
    // [0,.5],[.5,1] and therefore lies in [0,1].
    float coefficients[4][4]{};
    float inverse_scale = 1.0f;
    uint32_t cell_count = 0;
};

inline uint8_t polynomial_cell(float value, const PiecewisePolynomialQuantizer& model) {
    const float normalized = std::clamp(value * model.inverse_scale, -1.0f, 1.0f);
    const unsigned segment = normalized < -0.5f  ? 0U
                             : normalized < 0.0f ? 1U
                             : normalized < 0.5f ? 2U
                                                 : 3U;
    const float epsilon = 2.0f * normalized + (segment == 0   ? 2.0f
                                               : segment == 1 ? 1.0f
                                               : segment == 2 ? 0.0f
                                                              : -1.0f);
    const float* c = model.coefficients[segment];
    const float estimate =
        std::fma(std::fma(std::fma(c[3], epsilon, c[2]), epsilon, c[1]), epsilon, c[0]);
    const float limited = std::clamp(estimate, 0.0f, static_cast<float>(model.cell_count - 1));
    return static_cast<uint8_t>(limited);
}

inline void polynomial_quantize_scalar(const float* values,
                                       const PiecewisePolynomialQuantizer& model, uint8_t* output,
                                       std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) output[i] = polynomial_cell(values[i], model);
}

inline void accumulate_moments_scalar(const float* data, std::size_t count, float& sum_abs,
                                      float& sum_sq) {
    double absolute = 0.0;
    double squared = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        absolute += std::abs(static_cast<double>(data[i]));
        squared += static_cast<double>(data[i]) * data[i];
    }
    sum_abs = static_cast<float>(absolute);
    sum_sq = static_cast<float>(squared);
}

inline void vector_quantize_scalar(const float* residuals, const float* boundaries,
                                   std::size_t boundary_count, uint8_t* output, std::size_t count) {
    if (boundary_count > 255)
        throw std::invalid_argument("SIMD quantizer supports at most 256 cells");
    for (std::size_t i = 0; i < count; ++i) {
        uint8_t cell = 0;
        for (std::size_t j = 0; j < boundary_count; ++j)
            cell = static_cast<uint8_t>(cell + (residuals[i] > boundaries[j]));
        output[i] = cell;
    }
}

#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
AETHER_TARGET_AVX2 inline void accumulate_moments_avx2(const float* data, std::size_t count,
                                                       float& sum_abs, float& sum_sq) {
    __m256 absolute = _mm256_setzero_ps();
    __m256 squared = _mm256_setzero_ps();
    const __m256 sign = _mm256_set1_ps(-0.0f);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 value = _mm256_loadu_ps(data + i);
        absolute = _mm256_add_ps(absolute, _mm256_andnot_ps(sign, value));
        squared = _mm256_fmadd_ps(value, value, squared);
    }
    alignas(32) float a[8], s[8];
    _mm256_store_ps(a, absolute);
    _mm256_store_ps(s, squared);
    double a_sum = 0.0, s_sum = 0.0;
    for (unsigned lane = 0; lane < 8; ++lane) {
        a_sum += a[lane];
        s_sum += s[lane];
    }
    for (; i < count; ++i) {
        a_sum += std::abs(static_cast<double>(data[i]));
        s_sum += static_cast<double>(data[i]) * data[i];
    }
    sum_abs = static_cast<float>(a_sum);
    sum_sq = static_cast<float>(s_sum);
}

AETHER_TARGET_AVX2 inline void vector_quantize_avx2(const float* residuals, const float* boundaries,
                                                    std::size_t boundary_count, uint8_t* output,
                                                    std::size_t count) {
    if (boundary_count > 255)
        throw std::invalid_argument("SIMD quantizer supports at most 256 cells");
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 values = _mm256_loadu_ps(residuals + i);
        __m256i cells = _mm256_setzero_si256();
        for (std::size_t j = 0; j < boundary_count; ++j) {
            const __m256 mask = _mm256_cmp_ps(values, _mm256_set1_ps(boundaries[j]), _CMP_GT_OQ);
            cells = _mm256_sub_epi32(cells, _mm256_castps_si256(mask));
        }
        alignas(32) int lanes[8];
        _mm256_store_si256(reinterpret_cast<__m256i*>(lanes), cells);
        for (unsigned lane = 0; lane < 8; ++lane)
            output[i + lane] = static_cast<uint8_t>(lanes[lane]);
    }
    vector_quantize_scalar(residuals + i, boundaries, boundary_count, output + i, count - i);
}

AETHER_TARGET_AVX2 inline void polynomial_quantize_avx2(const float* values,
                                                        const PiecewisePolynomialQuantizer& model,
                                                        uint8_t* output, std::size_t count) {
    const __m256 inverse_scale = _mm256_set1_ps(model.inverse_scale);
    const __m256 negative_one = _mm256_set1_ps(-1.0f);
    const __m256 positive_one = _mm256_set1_ps(1.0f);
    const __m256 negative_half = _mm256_set1_ps(-0.5f);
    const __m256 zero = _mm256_setzero_ps();
    const __m256 positive_half = _mm256_set1_ps(0.5f);
    const __m256 two = _mm256_set1_ps(2.0f);
    const __m256 one = _mm256_set1_ps(1.0f);
    std::size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256 x = _mm256_mul_ps(_mm256_loadu_ps(values + i), inverse_scale);
        x = _mm256_min_ps(positive_one, _mm256_max_ps(negative_one, x));
        const __m256 mask0 = _mm256_cmp_ps(x, negative_half, _CMP_LT_OQ);
        const __m256 mask1 = _mm256_andnot_ps(mask0, _mm256_cmp_ps(x, zero, _CMP_LT_OQ));
        const __m256 mask2 = _mm256_andnot_ps(_mm256_or_ps(mask0, mask1),
                                              _mm256_cmp_ps(x, positive_half, _CMP_LT_OQ));
        __m256 offset = _mm256_set1_ps(-1.0f);
        offset = _mm256_blendv_ps(offset, zero, mask2);
        offset = _mm256_blendv_ps(offset, one, mask1);
        offset = _mm256_blendv_ps(offset, two, mask0);
        const __m256 epsilon = _mm256_fmadd_ps(two, x, offset);

        __m256 coefficients[4];
        for (unsigned coefficient = 0; coefficient < 4; ++coefficient) {
            __m256 selected = _mm256_set1_ps(model.coefficients[3][coefficient]);
            selected = _mm256_blendv_ps(selected,
                                        _mm256_set1_ps(model.coefficients[2][coefficient]), mask2);
            selected = _mm256_blendv_ps(selected,
                                        _mm256_set1_ps(model.coefficients[1][coefficient]), mask1);
            selected = _mm256_blendv_ps(selected,
                                        _mm256_set1_ps(model.coefficients[0][coefficient]), mask0);
            coefficients[coefficient] = selected;
        }
        __m256 estimate = _mm256_fmadd_ps(coefficients[3], epsilon, coefficients[2]);
        estimate = _mm256_fmadd_ps(estimate, epsilon, coefficients[1]);
        estimate = _mm256_fmadd_ps(estimate, epsilon, coefficients[0]);
        estimate = _mm256_min_ps(_mm256_set1_ps(static_cast<float>(model.cell_count - 1)),
                                 _mm256_max_ps(zero, estimate));
        alignas(32) float lanes[8];
        _mm256_store_ps(lanes, estimate);
        for (unsigned lane = 0; lane < 8; ++lane)
            output[i + lane] = static_cast<uint8_t>(lanes[lane]);
    }
    polynomial_quantize_scalar(values + i, model, output + i, count - i);
}
#endif

#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX512F__))
AETHER_TARGET_AVX512 inline void accumulate_moments_avx512(const float* data, std::size_t count,
                                                           float& sum_abs, float& sum_sq) {
    __m512 absolute = _mm512_setzero_ps();
    __m512 squared = _mm512_setzero_ps();
    const __m512i magnitude = _mm512_set1_epi32(0x7fffffff);
    std::size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const __m512 value = _mm512_loadu_ps(data + i);
        const __m512 positive =
            _mm512_castsi512_ps(_mm512_and_si512(_mm512_castps_si512(value), magnitude));
        absolute = _mm512_add_ps(absolute, positive);
        squared = _mm512_fmadd_ps(value, value, squared);
    }
    alignas(64) float a[16], s[16];
    _mm512_store_ps(a, absolute);
    _mm512_store_ps(s, squared);
    double a_sum = 0.0, s_sum = 0.0;
    for (unsigned lane = 0; lane < 16; ++lane) {
        a_sum += a[lane];
        s_sum += s[lane];
    }
    for (; i < count; ++i) {
        a_sum += std::abs(static_cast<double>(data[i]));
        s_sum += static_cast<double>(data[i]) * data[i];
    }
    sum_abs = static_cast<float>(a_sum);
    sum_sq = static_cast<float>(s_sum);
}

AETHER_TARGET_AVX512 inline void vector_quantize_avx512(const float* residuals,
                                                        const float* boundaries,
                                                        std::size_t boundary_count, uint8_t* output,
                                                        std::size_t count) {
    if (boundary_count > 255)
        throw std::invalid_argument("SIMD quantizer supports at most 256 cells");
    std::size_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const __m512 values = _mm512_loadu_ps(residuals + i);
        __m512i cells = _mm512_setzero_si512();
        for (std::size_t j = 0; j < boundary_count; ++j) {
            const __mmask16 mask =
                _mm512_cmp_ps_mask(values, _mm512_set1_ps(boundaries[j]), _CMP_GT_OQ);
            cells = _mm512_mask_add_epi32(cells, mask, cells, _mm512_set1_epi32(1));
        }
        alignas(64) int lanes[16];
        _mm512_store_si512(lanes, cells);
        for (unsigned lane = 0; lane < 16; ++lane)
            output[i + lane] = static_cast<uint8_t>(lanes[lane]);
    }
    vector_quantize_scalar(residuals + i, boundaries, boundary_count, output + i, count - i);
}
#endif

#if AETHER_DISPATCH_ARM64
inline void accumulate_moments_neon(const float* data, std::size_t count, float& sum_abs,
                                    float& sum_sq) {
    float32x4_t absolute = vdupq_n_f32(0.0f);
    float32x4_t squared = vdupq_n_f32(0.0f);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const float32x4_t value = vld1q_f32(data + i);
        absolute = vaddq_f32(absolute, vabsq_f32(value));
        squared = vfmaq_f32(squared, value, value);
    }
    double a_sum = vaddvq_f32(absolute);
    double s_sum = vaddvq_f32(squared);
    for (; i < count; ++i) {
        a_sum += std::abs(static_cast<double>(data[i]));
        s_sum += static_cast<double>(data[i]) * data[i];
    }
    sum_abs = static_cast<float>(a_sum);
    sum_sq = static_cast<float>(s_sum);
}

inline void vector_quantize_neon(const float* residuals, const float* boundaries,
                                 std::size_t boundary_count, uint8_t* output, std::size_t count) {
    if (boundary_count > 255)
        throw std::invalid_argument("SIMD quantizer supports at most 256 cells");
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        const float32x4_t values = vld1q_f32(residuals + i);
        uint32x4_t cells = vdupq_n_u32(0);
        for (std::size_t j = 0; j < boundary_count; ++j) {
            const uint32x4_t mask = vcgtq_f32(values, vdupq_n_f32(boundaries[j]));
            cells = vaddq_u32(cells, vandq_u32(mask, vdupq_n_u32(1)));
        }
        alignas(16) uint32_t lanes[4];
        vst1q_u32(lanes, cells);
        for (unsigned lane = 0; lane < 4; ++lane)
            output[i + lane] = static_cast<uint8_t>(lanes[lane]);
    }
    vector_quantize_scalar(residuals + i, boundaries, boundary_count, output + i, count - i);
}

inline void polynomial_quantize_neon(const float* values, const PiecewisePolynomialQuantizer& model,
                                     uint8_t* output, std::size_t count) {
    const float32x4_t inverse_scale = vdupq_n_f32(model.inverse_scale);
    const float32x4_t negative_one = vdupq_n_f32(-1.0f);
    const float32x4_t positive_one = vdupq_n_f32(1.0f);
    const float32x4_t negative_half = vdupq_n_f32(-0.5f);
    const float32x4_t zero = vdupq_n_f32(0.0f);
    const float32x4_t positive_half = vdupq_n_f32(0.5f);
    const float32x4_t two = vdupq_n_f32(2.0f);
    std::size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        float32x4_t x = vmulq_f32(vld1q_f32(values + i), inverse_scale);
        x = vminq_f32(positive_one, vmaxq_f32(negative_one, x));
        const uint32x4_t mask0 = vcltq_f32(x, negative_half);
        const uint32x4_t mask1 = vandq_u32(vmvnq_u32(mask0), vcltq_f32(x, zero));
        const uint32x4_t mask2 =
            vandq_u32(vmvnq_u32(vorrq_u32(mask0, mask1)), vcltq_f32(x, positive_half));
        float32x4_t offset = vdupq_n_f32(-1.0f);
        offset = vbslq_f32(mask2, zero, offset);
        offset = vbslq_f32(mask1, vdupq_n_f32(1.0f), offset);
        offset = vbslq_f32(mask0, two, offset);
        const float32x4_t epsilon = vfmaq_f32(offset, two, x);
        float32x4_t coefficients[4];
        for (unsigned coefficient = 0; coefficient < 4; ++coefficient) {
            float32x4_t selected = vdupq_n_f32(model.coefficients[3][coefficient]);
            selected = vbslq_f32(mask2, vdupq_n_f32(model.coefficients[2][coefficient]), selected);
            selected = vbslq_f32(mask1, vdupq_n_f32(model.coefficients[1][coefficient]), selected);
            selected = vbslq_f32(mask0, vdupq_n_f32(model.coefficients[0][coefficient]), selected);
            coefficients[coefficient] = selected;
        }
        float32x4_t estimate = vfmaq_f32(coefficients[2], coefficients[3], epsilon);
        estimate = vfmaq_f32(coefficients[1], estimate, epsilon);
        estimate = vfmaq_f32(coefficients[0], estimate, epsilon);
        estimate = vminq_f32(vdupq_n_f32(static_cast<float>(model.cell_count - 1)),
                             vmaxq_f32(zero, estimate));
        alignas(16) float lanes[4];
        vst1q_f32(lanes, estimate);
        for (unsigned lane = 0; lane < 4; ++lane)
            output[i + lane] = static_cast<uint8_t>(lanes[lane]);
    }
    polynomial_quantize_scalar(values + i, model, output + i, count - i);
}
#endif

inline bool arm_neon_available() noexcept {
#if defined(__linux__) && AETHER_DISPATCH_ARM64 && defined(HWCAP_ASIMD)
    return (getauxval(AT_HWCAP) & HWCAP_ASIMD) != 0;
#elif defined(__APPLE__) && AETHER_DISPATCH_ARM64
    int enabled = 1;
    std::size_t size = sizeof(enabled);
    const int status = sysctlbyname("hw.optional.neon", &enabled, &size, nullptr, 0);
    return status == 0 ? enabled != 0 : true;
#elif AETHER_DISPATCH_ARM64
    return true;
#else
    return false;
#endif
}

inline Backend detected_backend() noexcept {
    if (const char* forced = std::getenv("AETHER_SIMD")) {
        if (std::string_view(forced) == "scalar") return Backend::SCALAR;
    }
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__))
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512dq") &&
        __builtin_cpu_supports("fma"))
        return Backend::AVX512;
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return Backend::AVX2;
#elif AETHER_DISPATCH_X86 && defined(_MSC_VER)
    int leaf1[4]{};
    __cpuidex(leaf1, 1, 0);
    const bool osxsave = (leaf1[2] & (1 << 27)) != 0;
    const bool avx = (leaf1[2] & (1 << 28)) != 0;
    const bool fma = (leaf1[2] & (1 << 12)) != 0;
    const unsigned long long xcr0 = osxsave ? _xgetbv(0) : 0;
    if (avx && fma && (xcr0 & 0x6) == 0x6) {
        int leaf7[4]{};
        __cpuidex(leaf7, 7, 0);
#if defined(__AVX512F__)
        if ((leaf7[1] & (1 << 16)) && (leaf7[1] & (1 << 17)) && (xcr0 & 0xe6) == 0xe6)
            return Backend::AVX512;
#endif
#if defined(__AVX2__)
        if (leaf7[1] & (1 << 5)) return Backend::AVX2;
#endif
    }
#endif
#if AETHER_DISPATCH_ARM64
    if (arm_neon_available()) return Backend::NEON;
#endif
    return Backend::SCALAR;
}

inline bool backend_available(Backend backend) noexcept {
    if (backend == Backend::SCALAR) return true;
    const Backend native = detected_backend();
    if (backend == Backend::AVX2) return native == Backend::AVX2 || native == Backend::AVX512;
    return native == backend;
}

inline void accumulate_moments_backend(Backend backend, const float* data, std::size_t count,
                                       float& sum_abs, float& sum_sq) {
    if (count != 0 && data == nullptr) throw std::invalid_argument("null SIMD moment input");
    switch (backend) {
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX512F__))
        case Backend::AVX512:
            return accumulate_moments_avx512(data, count, sum_abs, sum_sq);
#endif
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
        case Backend::AVX2:
            return accumulate_moments_avx2(data, count, sum_abs, sum_sq);
#endif
#if AETHER_DISPATCH_ARM64
        case Backend::NEON:
            return accumulate_moments_neon(data, count, sum_abs, sum_sq);
#endif
        default:
            return accumulate_moments_scalar(data, count, sum_abs, sum_sq);
    }
}

inline void accumulate_moments(const float* data, std::size_t count, float& sum_abs,
                               float& sum_sq) {
    static const Backend backend = detected_backend();
    accumulate_moments_backend(backend, data, count, sum_abs, sum_sq);
}

inline void vector_quantize_backend(Backend backend, const float* residuals,
                                    const float* boundaries, std::size_t boundary_count,
                                    uint8_t* output, std::size_t count) {
    if (count != 0 && (residuals == nullptr || output == nullptr))
        throw std::invalid_argument("null SIMD quantizer array");
    if (boundary_count != 0 && boundaries == nullptr)
        throw std::invalid_argument("null SIMD quantizer boundaries");
    switch (backend) {
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX512F__))
        case Backend::AVX512:
            return vector_quantize_avx512(residuals, boundaries, boundary_count, output, count);
#endif
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
        case Backend::AVX2:
            return vector_quantize_avx2(residuals, boundaries, boundary_count, output, count);
#endif
#if AETHER_DISPATCH_ARM64
        case Backend::NEON:
            return vector_quantize_neon(residuals, boundaries, boundary_count, output, count);
#endif
        default:
            return vector_quantize_scalar(residuals, boundaries, boundary_count, output, count);
    }
}

inline void vector_quantize(const float* residuals, const float* boundaries,
                            std::size_t boundary_count, uint8_t* output, std::size_t count) {
    static const Backend backend = detected_backend();
    vector_quantize_backend(backend, residuals, boundaries, boundary_count, output, count);
}

inline void polynomial_quantize_backend(Backend backend, const float* values,
                                        const PiecewisePolynomialQuantizer& model, uint8_t* output,
                                        std::size_t count) {
    if (count != 0 && (values == nullptr || output == nullptr))
        throw std::invalid_argument("null polynomial quantizer array");
    if (model.cell_count == 0 || model.cell_count > 256 || !std::isfinite(model.inverse_scale) ||
        model.inverse_scale <= 0.0f)
        throw std::invalid_argument("invalid polynomial quantizer model");
    switch (backend) {
#if AETHER_DISPATCH_X86 && (defined(__GNUC__) || defined(__clang__) || defined(__AVX2__))
        case Backend::AVX512:
        case Backend::AVX2:
            return polynomial_quantize_avx2(values, model, output, count);
#endif
#if AETHER_DISPATCH_ARM64
        case Backend::NEON:
            return polynomial_quantize_neon(values, model, output, count);
#endif
        default:
            return polynomial_quantize_scalar(values, model, output, count);
    }
}

inline void polynomial_quantize(const float* values, const PiecewisePolynomialQuantizer& model,
                                uint8_t* output, std::size_t count) {
    static const Backend backend = detected_backend();
    polynomial_quantize_backend(backend, values, model, output, count);
}

}  // namespace aether::simd

#undef AETHER_TARGET_AVX2
#undef AETHER_TARGET_AVX512
