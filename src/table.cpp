#include "aether/table.hpp"

#include <limits>

namespace aether {
namespace {

constexpr std::size_t kStreamHeaderSize = 64;
constexpr std::size_t kPreambleSize = 24;
constexpr std::size_t kIndexHeaderSize = 8;
constexpr std::size_t kIndexEntrySize = 16;
constexpr std::size_t kIndexCrcSize = 4;
constexpr std::size_t kIndexTrailerSize = kIndexCrcSize;

class Reader {
   public:
    explicit Reader(std::span<const uint8_t> input) : input_(input) {}

    uint8_t u8() {
        require(1);
        return input_[position_++];
    }
    uint16_t u16() {
        require(2);
        const uint16_t value = static_cast<uint16_t>(
            static_cast<uint16_t>(input_[position_]) |
            static_cast<uint16_t>(static_cast<uint16_t>(input_[position_ + 1]) << 8));
        position_ += 2;
        return value;
    }
    uint32_t u32() {
        require(4);
        uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= static_cast<uint32_t>(input_[position_ + i]) << (8 * i);
        position_ += 4;
        return value;
    }
    uint64_t u64() {
        require(8);
        uint64_t value = 0;
        for (unsigned i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(input_[position_ + i]) << (8 * i);
        position_ += 8;
        return value;
    }
    std::span<const uint8_t> bytes(std::size_t count) {
        require(count);
        const auto result = input_.subspan(position_, count);
        position_ += count;
        return result;
    }
    bool empty() const noexcept { return position_ == input_.size(); }

   private:
    void require(std::size_t count) const {
        if (count > input_.size() - position_)
            throw CorruptedStreamException("AetherStream index buffer overrun");
    }
    std::span<const uint8_t> input_;
    std::size_t position_ = 0;
};

uint32_t read_u32_at(std::span<const uint8_t> input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 4)
        throw CorruptedStreamException("truncated block index footer");
    return Reader(input.subspan(offset, 4)).u32();
}

}  // namespace

IndexedStreamView::IndexedStreamView(std::span<const uint8_t> stream) : stream_(stream) {
    if (stream.size() < kStreamHeaderSize + kIndexHeaderSize + kIndexTrailerSize)
        throw CorruptedStreamException("indexed stream is too short");

    Reader header(stream.first(kStreamHeaderSize));
    if (header.u32() != MAGIC_HEADER) throw CorruptedStreamException("invalid AetherStream magic");
    const uint16_t version = header.u16();
    if (header.u16() != 0) throw CorruptedStreamException("unsupported stream header extension");
    const uint64_t sample_count_64 = header.u64();
    const uint32_t block_count = header.u32();
    global_flags_ = header.u32();
    validate_wire_format_version(version, (global_flags_ & STREAM_FLAG_BLOCK_INDEX) != 0);
    for (uint8_t padding : header.bytes(kStreamHeaderSize - kPreambleSize))
        if (padding != 0) throw CorruptedStreamException("non-zero stream-header padding");
    if ((global_flags_ & STREAM_FLAG_BLOCK_INDEX) == 0)
        throw CorruptedStreamException("stream does not contain a block index");
    if ((global_flags_ & ~(STREAM_FLAG_ERROR_BOUNDED | STREAM_FLAG_BLOCK_INDEX)) != 0)
        throw CorruptedStreamException("unsupported global stream flags");
    if (sample_count_64 > std::numeric_limits<std::size_t>::max() ||
        sample_count_64 > std::numeric_limits<uint32_t>::max())
        throw CorruptedStreamException("indexed sample count is unsupported");
    sample_count_ = static_cast<std::size_t>(sample_count_64);
    const uint64_t expected_blocks =
        sample_count_ / BLOCK_SIZE + (sample_count_ % BLOCK_SIZE != 0 ? 1U : 0U);
    if (block_count != expected_blocks)
        throw CorruptedStreamException("inconsistent indexed block count");

    const uint64_t footer_size_64 =
        kIndexHeaderSize + static_cast<uint64_t>(block_count) * kIndexEntrySize + kIndexTrailerSize;
    if (footer_size_64 > stream.size())
        throw CorruptedStreamException("invalid block index footer length");
    const std::size_t footer_size = static_cast<std::size_t>(footer_size_64);
    index_offset_ = stream.size() - footer_size;
    const std::size_t protected_size = footer_size - kIndexCrcSize;
    const uint32_t stored_crc = read_u32_at(stream, index_offset_ + protected_size);
    if (crc32c(stream.subspan(index_offset_, protected_size)) != stored_crc)
        throw CorruptedStreamException("block index CRC32-C mismatch");

    Reader index(stream.subspan(index_offset_, protected_size));
    if (index.u32() != block_count || index.u32() != INDEX_MAGIC)
        throw CorruptedStreamException("invalid block index header");
    entries_.reserve(block_count);
    uint64_t expected_sample = 0;
    uint64_t previous_offset = 0;
    for (uint32_t i = 0; i < block_count; ++i) {
        const IndexEntry entry{index.u64(), index.u32(), index.u32()};
        const uint32_t expected_count = static_cast<uint32_t>(
            std::min<std::size_t>(BLOCK_SIZE, sample_count_ - expected_sample));
        if (entry.start_sample_index != expected_sample || entry.sample_count != expected_count ||
            entry.byte_offset < kStreamHeaderSize || entry.byte_offset >= index_offset_ ||
            (entry.byte_offset & 63U) != 0 || (i != 0 && entry.byte_offset <= previous_offset))
            throw CorruptedStreamException("invalid block index entry");
        entries_.push_back(entry);
        expected_sample += entry.sample_count;
        previous_offset = entry.byte_offset;
    }
    if (!index.empty() || expected_sample != sample_count_ ||
        (!entries_.empty() && entries_.front().byte_offset != kStreamHeaderSize))
        throw CorruptedStreamException("inconsistent block index coverage");
}

void IndexedStreamView::decompress_slice(std::size_t start_idx, std::size_t count,
                                         std::span<float> out) const {
    if (out.size() != count) throw std::invalid_argument("slice output length must equal count");
    if (start_idx > sample_count_ || count > sample_count_ - start_idx)
        throw std::out_of_range("requested slice exceeds stream sample range");
    if (count == 0) return;

    const auto after_start = std::upper_bound(entries_.begin(), entries_.end(), start_idx,
                                              [](std::size_t value, const IndexEntry& entry) {
                                                  return value < entry.start_sample_index;
                                              });
    std::size_t block_index = static_cast<std::size_t>(after_start - entries_.begin() - 1);
    const std::size_t slice_end = start_idx + count;
    std::size_t written = 0;
    std::vector<float> block_samples(BLOCK_SIZE);
    while (block_index < entries_.size() && entries_[block_index].start_sample_index < slice_end) {
        const IndexEntry& entry = entries_[block_index];
        const std::size_t offset = static_cast<std::size_t>(entry.byte_offset);
        const std::size_t expected_end =
            block_index + 1 < entries_.size()
                ? static_cast<std::size_t>(entries_[block_index + 1].byte_offset)
                : index_offset_;
        if (expected_end <= offset) throw CorruptedStreamException("invalid indexed block extent");
        const auto decoded = internal::decode_block_record(
            stream_.data() + offset, expected_end - offset,
            std::span<float>(block_samples).first(entry.sample_count));
        if (decoded.samples_decoded != entry.sample_count ||
            decoded.bytes_consumed != expected_end - offset)
            throw CorruptedStreamException("indexed block metadata mismatch");
        const bool error_mode = decoded.quantization_mode == QuantizationMode::ERROR_BOUNDED;
        if (error_mode != ((global_flags_ & STREAM_FLAG_ERROR_BOUNDED) != 0))
            throw CorruptedStreamException("quantization mode disagrees with stream flags");

        const std::size_t block_start = entry.start_sample_index;
        const std::size_t copy_start = std::max(start_idx, block_start);
        const std::size_t copy_end = std::min(slice_end, block_start + entry.sample_count);
        const std::size_t copy_count = copy_end - copy_start;
        std::copy_n(block_samples.begin() + static_cast<std::ptrdiff_t>(copy_start - block_start),
                    copy_count, out.begin() + static_cast<std::ptrdiff_t>(written));
        written += copy_count;
        ++block_index;
    }
    if (written != count)
        throw CorruptedStreamException("block index did not cover requested slice");
}

void decompress_slice(std::span<const uint8_t> stream, std::size_t start_idx, std::size_t count,
                      std::span<float> out) {
    IndexedStreamView(stream).decompress_slice(start_idx, count, out);
}

}  // namespace aether
