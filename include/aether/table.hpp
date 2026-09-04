#pragma once

#include "common.hpp"

namespace aether {

inline constexpr uint32_t INDEX_MAGIC = 0x41494458U;

struct IndexEntry {
    uint64_t byte_offset = 0;
    uint32_t start_sample_index = 0;
    uint32_t sample_count = 0;
};

// A non-owning validated view of an indexed frame. The caller must keep the
// compressed byte span alive for the lifetime of the view. Header, footer CRC,
// and index semantics are validated once; slice lookup is O(log(block_count)).
class IndexedStreamView {
   public:
    explicit IndexedStreamView(std::span<const uint8_t> stream);

    std::size_t sample_count() const noexcept { return sample_count_; }
    std::size_t block_count() const noexcept { return entries_.size(); }
    void decompress_slice(std::size_t start_idx, std::size_t count, std::span<float> out) const;

   private:
    std::span<const uint8_t> stream_;
    std::vector<IndexEntry> entries_;
    std::size_t sample_count_ = 0;
    std::size_t index_offset_ = 0;
    uint32_t global_flags_ = 0;
};

// Convenience API. Constructing a cached IndexedStreamView is preferable when
// performing repeated slices because this function validates the index anew.
void decompress_slice(std::span<const uint8_t> stream, std::size_t start_idx, std::size_t count,
                      std::span<float> out);

}  // namespace aether
