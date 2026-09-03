#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__SSE4_2__)
#include <nmmintrin.h>
#elif defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace aether {

inline constexpr uint32_t MAGIC_HEADER = 0x41455448U;
inline constexpr std::size_t BLOCK_SIZE = 2048;
inline constexpr uint32_t RANS_STATES = 16;
inline constexpr uint32_t SCALE_BITS = 12;
inline constexpr uint32_t SCALE_TOTAL = 1U << SCALE_BITS;

enum class PredictorMode : uint8_t {
    CONSTANT = 0,
    HARMONIC = 1,
    LINEAR = 2,
    ADAPTIVE_AR = 3,
};

enum class QuantizationMode : uint8_t {
    RATE_TARGETED = 0,
    ERROR_BOUNDED = 1,
};

inline constexpr uint32_t STREAM_FLAG_ERROR_BOUNDED = 1U << 0U;
inline constexpr uint32_t STREAM_FLAG_BLOCK_INDEX = 1U << 1U;

struct BlockMetadata {
    PredictorMode mode = PredictorMode::CONSTANT;
    float param = 0.0f;
    float ged_alpha = 0.0f;
    float ged_beta = 2.0f;
    uint16_t num_levels = 0;
    uint32_t compressed_payload_bytes = 0;
    uint32_t crc32 = 0;
};

struct GedParameters {
    float mean = 0.0f;
    float alpha = 0.0f;
    float beta = 2.0f;
};

class CorruptedStreamException : public std::runtime_error {
   public:
    explicit CorruptedStreamException(const std::string& message) : std::runtime_error(message) {}
};

inline uint32_t crc32c(std::span<const uint8_t> bytes) {
    uint32_t crc = 0xffffffffU;
    const uint8_t* data = bytes.data();
    std::size_t size = bytes.size();

#if defined(__SSE4_2__) && defined(__x86_64__)
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, data, sizeof(word));
        crc = static_cast<uint32_t>(_mm_crc32_u64(crc, word));
        data += sizeof(word);
        size -= sizeof(word);
    }
    while (size-- != 0) {
        crc = _mm_crc32_u8(crc, *data++);
    }
#elif defined(__ARM_FEATURE_CRC32) && defined(__aarch64__)
    while (size >= sizeof(uint64_t)) {
        uint64_t word;
        __builtin_memcpy(&word, data, sizeof(word));
        crc = __crc32cd(crc, word);
        data += sizeof(word);
        size -= sizeof(word);
    }
    while (size-- != 0) {
        crc = __crc32cb(crc, *data++);
    }
#else
    while (size-- != 0) {
        crc ^= *data++;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82f63b78U & mask);
        }
    }
#endif

    return ~crc;
}

inline float entropy_bits(const std::vector<float>& probabilities) {
    double entropy = 0.0;
    for (float probability : probabilities) {
        if (probability > 0.0f) {
            entropy -=
                static_cast<double>(probability) * std::log2(static_cast<double>(probability));
        }
    }
    return static_cast<float>(entropy);
}

}  // namespace aether
