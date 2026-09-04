#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#include "aether/aether.hpp"
#include "aether/simd_dispatch.hpp"

namespace {

uint32_t read_u32(const std::vector<uint8_t>& bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8 |
           static_cast<uint32_t>(bytes[offset + 2]) << 16 |
           static_cast<uint32_t>(bytes[offset + 3]) << 24;
}

void write_u16(std::vector<uint8_t>& bytes, std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void write_u32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}

void refresh_first_crc(std::vector<uint8_t>& bytes) {
    const uint32_t body_size = read_u32(bytes, 64);
    const auto body = std::span<const uint8_t>(bytes).subspan(68, body_size);
    write_u32(bytes, 68 + body_size, aether::crc32c(body));
}

bool rejected(const std::vector<uint8_t>& bytes, std::size_t count) {
    std::vector<float> output(count);
    try {
        aether::AetherCodec().decompress(bytes, output);
    } catch (const aether::CorruptedStreamException&) {
        return true;
    }
    return false;
}

}  // namespace

int main() {
    std::vector<float> signal(4096);
    for (std::size_t i = 0; i < signal.size(); ++i)
        signal[i] = std::sin(static_cast<float>(i) * 0.031f);
    const auto valid = aether::AetherCodec(4.0f).compress(signal);
    assert(valid.size() % 64 == 0);

    for (std::size_t length = 0; length < valid.size(); ++length) {
        std::vector<uint8_t> truncated(valid.begin(), valid.begin() + length);
        assert(rejected(truncated, signal.size()));
    }

    const uint32_t body_size = read_u32(valid, 64);
    (void)body_size;
    const uint16_t levels =
        static_cast<uint16_t>(static_cast<uint16_t>(valid[88]) |
                              static_cast<uint16_t>(static_cast<uint16_t>(valid[89]) << 8));
    const std::size_t payload = 68 + 28 + 4ULL * levels;

    auto invalid_k = valid;
    write_u16(invalid_k, 88, 257);
    refresh_first_crc(invalid_k);
    assert(rejected(invalid_k, signal.size()));

    auto invalid_alphabet = valid;
    write_u16(invalid_alphabet, payload + 12, 257);
    refresh_first_crc(invalid_alphabet);
    assert(rejected(invalid_alphabet, signal.size()));

    auto invalid_frequency_sum = valid;
    write_u16(invalid_frequency_sum, payload + 14, 0);
    refresh_first_crc(invalid_frequency_sum);
    assert(rejected(invalid_frequency_sum, signal.size()));

    auto nonfinite_level = valid;
    write_u32(nonfinite_level, 96, 0x7fc00000U);
    refresh_first_crc(nonfinite_level);
    assert(rejected(nonfinite_level, signal.size()));

    auto corrupt_state = valid;
    corrupt_state[payload + 20] ^= 0x80;
    assert(rejected(corrupt_state, signal.size()));

    // Every byte in the CRC-protected first block must either be rejected or
    // decode safely. Header mutations are included separately below.
    for (std::size_t i = 0; i < std::min<std::size_t>(valid.size(), 512); ++i) {
        auto corrupt = valid;
        corrupt[i] ^= static_cast<uint8_t>(1U << (i & 7U));
        std::vector<float> output(signal.size());
        try {
            aether::AetherCodec().decompress(corrupt, output);
            for (float value : output) assert(std::isfinite(value));
        } catch (const aether::CorruptedStreamException&) {
        }
    }

#ifdef AETHER_CORPUS_DIR
    for (const auto& entry : std::filesystem::directory_iterator(AETHER_CORPUS_DIR)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream stream(entry.path(), std::ios::binary);
        const std::vector<char> raw{std::istreambuf_iterator<char>(stream),
                                    std::istreambuf_iterator<char>()};
        const std::vector<uint8_t> seed(raw.begin(), raw.end());
        assert(rejected(seed, 0));
    }
#endif

    // Decoder loads must not depend on input alignment.
    std::vector<uint8_t> unaligned(valid.size() + 1, 0xaa);
    std::copy(valid.begin(), valid.end(), unaligned.begin() + 1);
    std::vector<float> reconstructed(signal.size());
    aether::AetherCodec().decompress(std::span<const uint8_t>(unaligned).subspan(1), reconstructed);

    std::vector<float> moments(1027);
    for (std::size_t i = 0; i < moments.size(); ++i)
        moments[i] = std::sin(static_cast<float>(i) * 0.17f);
    float scalar_abs = 0.0f, scalar_sq = 0.0f;
    float native_abs = 0.0f, native_sq = 0.0f;
    aether::simd::accumulate_moments_backend(aether::simd::Backend::SCALAR, moments.data(),
                                             moments.size(), scalar_abs, scalar_sq);
    const float boundaries[] = {-0.75f, -0.25f, 0.0f, 0.25f, 0.75f};
    std::vector<uint8_t> scalar_codes(moments.size()), native_codes(moments.size());
    aether::simd::vector_quantize_backend(aether::simd::Backend::SCALAR, moments.data(), boundaries,
                                          5, scalar_codes.data(), moments.size());
    for (aether::simd::Backend backend :
         {aether::simd::Backend::SCALAR, aether::simd::Backend::NEON, aether::simd::Backend::AVX2,
          aether::simd::Backend::AVX512}) {
        if (!aether::simd::backend_available(backend)) continue;
        native_abs = native_sq = 0.0f;
        aether::simd::accumulate_moments_backend(backend, moments.data(), moments.size(),
                                                 native_abs, native_sq);
        assert(std::abs(native_abs - scalar_abs) <= 2e-4f * scalar_abs);
        assert(std::abs(native_sq - scalar_sq) <= 2e-4f * scalar_sq);
        aether::simd::vector_quantize_backend(backend, moments.data(), boundaries, 5,
                                              native_codes.data(), moments.size());
        assert(scalar_codes == native_codes);
    }

    aether::simd::PiecewisePolynomialQuantizer polynomial;
    polynomial.inverse_scale = 0.5f;
    polynomial.cell_count = 32;
    for (unsigned segment = 0; segment < 6; ++segment) {
        polynomial.coefficients[segment][0] = 8.0f * static_cast<float>(segment);
        polynomial.coefficients[segment][1] = 7.5f;
        polynomial.coefficients[segment][2] = 0.25f;
        polynomial.coefficients[segment][3] = 0.125f;
    }
    std::vector<uint8_t> scalar_polynomial(moments.size());
    aether::simd::polynomial_quantize_backend(aether::simd::Backend::SCALAR, moments.data(),
                                              polynomial, scalar_polynomial.data(), moments.size());
    for (aether::simd::Backend backend : {aether::simd::Backend::NEON, aether::simd::Backend::AVX2,
                                          aether::simd::Backend::AVX512}) {
        if (!aether::simd::backend_available(backend)) continue;
        std::vector<uint8_t> accelerated(moments.size());
        aether::simd::polynomial_quantize_backend(backend, moments.data(), polynomial,
                                                  accelerated.data(), moments.size());
        assert(accelerated == scalar_polynomial);
    }

    for (float invalid :
         {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
          -std::numeric_limits<float>::infinity()}) {
        bool threw = false;
        try {
            const std::vector<float> bad{invalid};
            (void)aether::AetherCodec().compress(bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}
