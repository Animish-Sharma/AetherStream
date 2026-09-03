#pragma once

#include "common.hpp"

namespace aether {

inline constexpr uint32_t INDEX_MAGIC = 0x41494458U;

struct IndexEntry {
    uint64_t byte_offset = 0;
    uint32_t start_sample_index = 0;
    uint32_t sample_count = 0;
};

// Decode exactly `count` samples beginning at `start_idx` from an indexed
// AetherStream frame. The compressed input is never copied: index entries are
// validated and selected blocks are addressed directly through the input span.
void decompress_slice(std::span<const uint8_t> stream, std::size_t start_idx, std::size_t count,
                      std::span<float> out);

}  // namespace aether
