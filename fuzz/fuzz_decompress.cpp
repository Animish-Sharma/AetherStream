#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <vector>

#include "aether/aether.hpp"
#include "aether/table.hpp"

namespace {

uint32_t little_u32(const uint8_t* data) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[i]) << (8 * i);
    return value;
}

uint64_t little_u64(const uint8_t* data) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (8 * i);
    return value;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
    if (size < 16) return 0;
    const uint64_t encoded_count = little_u64(data + 8);
    if (encoded_count > 1'000'000) return 0;
    std::vector<float> output(static_cast<std::size_t>(encoded_count));
    try {
        aether::AetherCodec().decompress(std::span<const uint8_t>(data, size), output);
        for (float value : output)
            if (!std::isfinite(value)) std::abort();
        if (size >= 24 && (little_u32(data + 20) & aether::STREAM_FLAG_BLOCK_INDEX) != 0 &&
            encoded_count != 0) {
            const std::size_t start = data[0] % static_cast<std::size_t>(encoded_count);
            const std::size_t count =
                std::min<std::size_t>(data[1], static_cast<std::size_t>(encoded_count) - start);
            std::vector<float> slice(count);
            aether::decompress_slice(std::span<const uint8_t>(data, size), start, count, slice);
            for (std::size_t i = 0; i < count; ++i)
                if (slice[i] != output[start + i]) std::abort();
        }
    } catch (const aether::CorruptedStreamException&) {
    }
    return 0;
}
