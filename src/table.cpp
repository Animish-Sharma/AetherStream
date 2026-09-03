#include "aether/table.hpp"

#include <bit>
#include <cstring>
#include <limits>

#include "aether/predictor.hpp"
#include "aether/vrans_codec.hpp"

namespace aether {
namespace {

constexpr uint16_t kFormatVersion = 5;
constexpr std::size_t kStreamHeaderSize = 64;
constexpr std::size_t kPreambleSize = 24;
constexpr std::size_t kMinimumBlockBodySize = 28;
constexpr std::size_t kMaximumBlockBodySize = 16 * 1024 * 1024;
constexpr std::size_t kIndexHeaderSize = 8;
constexpr std::size_t kIndexEntrySize = 16;
constexpr std::size_t kIndexTrailerSize = 4;
constexpr uint8_t kRateMode = static_cast<uint8_t>(QuantizationMode::RATE_TARGETED);
constexpr uint8_t kErrorMode = static_cast<uint8_t>(QuantizationMode::ERROR_BOUNDED);

constexpr std::size_t align64(std::size_t value) noexcept {
    return (value + 63U) & ~std::size_t{63U};
}

class Reader {
   public:
    explicit Reader(std::span<const uint8_t> input) : input_(input) {}

    uint8_t u8() {
        require(1);
        return input_[position_++];
    }
    uint16_t u16() {
        require(2);
        const uint16_t value = static_cast<uint16_t>(input_[position_]) |
                               static_cast<uint16_t>(input_[position_ + 1]) << 8;
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
    float f32() { return std::bit_cast<float>(u32()); }
    std::span<const uint8_t> bytes(std::size_t count) {
        require(count);
        const auto result = input_.subspan(position_, count);
        position_ += count;
        return result;
    }
    std::size_t remaining() const noexcept { return input_.size() - position_; }
    bool empty() const noexcept { return position_ == input_.size(); }

   private:
    void require(std::size_t count) const {
        if (count > input_.size() - position_)
            throw CorruptedStreamException("AetherStream buffer overrun");
    }

    std::span<const uint8_t> input_;
    std::size_t position_ = 0;
};

uint32_t read_u32_at(std::span<const uint8_t> input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 4)
        throw CorruptedStreamException("truncated block index footer");
    Reader reader(input.subspan(offset, 4));
    return reader.u32();
}

uint32_t take_varuint(std::span<const uint8_t> tokens, std::size_t& cursor) {
    uint32_t value = 0;
    for (unsigned byte_index = 0; byte_index < 5; ++byte_index) {
        if (cursor == tokens.size()) throw CorruptedStreamException("truncated error token varint");
        const uint8_t byte = tokens[cursor++];
        if (byte_index == 4 && (byte & 0xf0U) != 0)
            throw CorruptedStreamException("error token varint overflow");
        value |= static_cast<uint32_t>(byte & 0x7fU) << (7 * byte_index);
        if ((byte & 0x80U) == 0) return value;
    }
    throw CorruptedStreamException("unterminated error token varint");
}

std::vector<int32_t> read_error_payload(std::span<const uint8_t> payload, std::size_t count) {
    std::vector<uint8_t> tokens;
    try {
        tokens =
            InterleavedRansDecoder::decode(payload.data(), payload.size(), 0, count * 7ULL + 2ULL);
    } catch (const std::exception& error) {
        throw CorruptedStreamException(std::string("invalid error rANS payload: ") + error.what());
    }
    if (tokens.size() > count * 7ULL + 2ULL)
        throw CorruptedStreamException("oversized error token stream");
    std::vector<int32_t> result;
    result.reserve(count);
    std::size_t cursor = 0;
    while (cursor < tokens.size() && result.size() < count) {
        const uint8_t tag = tokens[cursor++];
        const uint32_t value = take_varuint(tokens, cursor);
        if (tag == 0) {
            if (value == 0 || value > count - result.size())
                throw CorruptedStreamException("invalid zero-run length");
            result.insert(result.end(), value, 0);
        } else if (tag == 1) {
            if (value == 0) throw CorruptedStreamException("zero encoded as non-zero quantum");
            const int64_t decoded = (value & 1U) ? -static_cast<int64_t>(value / 2U) - 1
                                                 : static_cast<int64_t>(value / 2U);
            if (decoded < std::numeric_limits<int32_t>::min() ||
                decoded > std::numeric_limits<int32_t>::max())
                throw CorruptedStreamException("error quantum overflow");
            result.push_back(static_cast<int32_t>(decoded));
        } else {
            throw CorruptedStreamException("invalid error token tag");
        }
    }
    if (result.size() != count || cursor != tokens.size())
        throw CorruptedStreamException("inconsistent error token stream");
    return result;
}

float reconstructed_value(float prediction, int32_t quantum, float delta) {
    return static_cast<float>(static_cast<double>(prediction) +
                              static_cast<double>(quantum) * delta);
}

void decode_block(std::span<const uint8_t> stream, const IndexEntry& entry, uint32_t global_flags,
                  std::span<float> output) {
    if (entry.byte_offset > stream.size())
        throw CorruptedStreamException("block index offset exceeds stream");
    const std::size_t offset = static_cast<std::size_t>(entry.byte_offset);
    Reader record(stream.subspan(offset));
    const uint32_t body_size = record.u32();
    if (body_size < kMinimumBlockBodySize || body_size > kMaximumBlockBodySize)
        throw CorruptedStreamException("invalid indexed block body size");
    const auto body = record.bytes(body_size);
    const uint32_t expected_crc = record.u32();
    const std::size_t unpadded_size = 4ULL + body_size + 4ULL;
    const std::size_t padding_size = align64(unpadded_size) - unpadded_size;
    for (uint8_t padding : record.bytes(padding_size))
        if (padding != 0) throw CorruptedStreamException("non-zero indexed block padding");
    if (crc32c(body) != expected_crc) throw CorruptedStreamException("block CRC32-C mismatch");

    Reader block(body);
    const uint32_t count = block.u32();
    const uint8_t raw_mode = block.u8();
    const uint8_t quant_mode = block.u8();
    if (block.u16() != 0) throw CorruptedStreamException("unsupported block flags");
    if (raw_mode > static_cast<uint8_t>(PredictorMode::LINEAR))
        throw CorruptedStreamException("invalid predictor mode");
    const PredictorMode mode = static_cast<PredictorMode>(raw_mode);
    const float harmonic_param = block.f32();
    const float scale_or_alpha = block.f32();
    const float ged_beta = block.f32();
    const uint16_t num_levels = block.u16();
    if (block.u16() != 0) throw CorruptedStreamException("unsupported block extension");
    const uint32_t payload_bytes = block.u32();

    if (count != entry.sample_count || output.size() != count)
        throw CorruptedStreamException("indexed block sample count mismatch");
    if (!std::isfinite(harmonic_param) ||
        (mode == PredictorMode::HARMONIC && std::abs(harmonic_param) > 0.999001f))
        throw CorruptedStreamException("invalid harmonic predictor parameter");
    if (!std::isfinite(scale_or_alpha) || !std::isfinite(ged_beta))
        throw CorruptedStreamException("non-finite block model parameter");
    const bool error_mode = quant_mode == kErrorMode;
    if (!error_mode && (scale_or_alpha < 0.0f || ged_beta < 0.2f || ged_beta > 5.0f))
        throw CorruptedStreamException("invalid GED parameters");
    if (error_mode != ((global_flags & STREAM_FLAG_ERROR_BOUNDED) != 0))
        throw CorruptedStreamException("quantization mode disagrees with stream flags");

    std::vector<float> levels;
    if (quant_mode == kRateMode) {
        if (num_levels < 4 || num_levels > 256)
            throw CorruptedStreamException("invalid quantizer codebook size");
        levels.resize(num_levels);
        for (float& level : levels) {
            level = block.f32();
            if (!std::isfinite(level)) throw CorruptedStreamException("non-finite codebook level");
        }
    } else if (quant_mode != kErrorMode || num_levels != 0) {
        throw CorruptedStreamException("invalid quantization mode");
    }
    if (payload_bytes != block.remaining())
        throw CorruptedStreamException("invalid compressed payload length");
    const auto payload = block.bytes(payload_bytes);

    float previous_two = 0.0f;
    float previous = 0.0f;
    if (error_mode) {
        if (scale_or_alpha <= 0.0f) throw CorruptedStreamException("invalid error-bounded delta");
        const auto quanta = read_error_payload(payload, count);
        for (std::size_t i = 0; i < count; ++i) {
            const float prediction =
                predict_sample(mode, harmonic_param, i, previous, previous_two);
            const float current = reconstructed_value(prediction, quanta[i], scale_or_alpha);
            if (!std::isfinite(current))
                throw CorruptedStreamException("non-finite reconstruction");
            output[i] = current;
            previous_two = previous;
            previous = current;
        }
    } else {
        std::vector<uint8_t> symbols;
        try {
            symbols = InterleavedRansDecoder::decode(payload.data(), payload.size(), count);
        } catch (const std::exception& error) {
            throw CorruptedStreamException(std::string("invalid rANS payload: ") + error.what());
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (symbols[i] >= levels.size())
                throw CorruptedStreamException("symbol exceeds codebook");
            const float prediction =
                predict_sample(mode, harmonic_param, i, previous, previous_two);
            const float current = prediction + levels[symbols[i]];
            if (!std::isfinite(current))
                throw CorruptedStreamException("non-finite reconstruction");
            output[i] = current;
            previous_two = previous;
            previous = current;
        }
    }
}

}  // namespace

void decompress_slice(std::span<const uint8_t> stream, std::size_t start_idx, std::size_t count,
                      std::span<float> out) {
    if (out.size() != count) throw std::invalid_argument("slice output length must equal count");
    if (stream.size() < kStreamHeaderSize + kIndexHeaderSize + kIndexTrailerSize)
        throw CorruptedStreamException("indexed stream is too short");

    Reader header(stream.first(kStreamHeaderSize));
    if (header.u32() != MAGIC_HEADER) throw CorruptedStreamException("invalid AetherStream magic");
    if (header.u16() != kFormatVersion)
        throw CorruptedStreamException("unsupported AetherStream version");
    if (header.u16() != 0) throw CorruptedStreamException("unsupported stream header extension");
    const uint64_t sample_count_64 = header.u64();
    const uint32_t block_count = header.u32();
    const uint32_t global_flags = header.u32();
    for (uint8_t padding : header.bytes(kStreamHeaderSize - kPreambleSize))
        if (padding != 0) throw CorruptedStreamException("non-zero stream-header padding");
    if ((global_flags & STREAM_FLAG_BLOCK_INDEX) == 0)
        throw CorruptedStreamException("stream does not contain a block index");
    if ((global_flags & ~(STREAM_FLAG_ERROR_BOUNDED | STREAM_FLAG_BLOCK_INDEX)) != 0)
        throw CorruptedStreamException("unsupported global stream flags");
    if (sample_count_64 > std::numeric_limits<std::size_t>::max() ||
        sample_count_64 > std::numeric_limits<uint32_t>::max())
        throw CorruptedStreamException("indexed sample count is unsupported");
    const std::size_t sample_count = static_cast<std::size_t>(sample_count_64);
    const uint64_t expected_blocks =
        sample_count / BLOCK_SIZE + (sample_count % BLOCK_SIZE != 0 ? 1U : 0U);
    if (block_count != expected_blocks)
        throw CorruptedStreamException("inconsistent indexed block count");
    if (start_idx > sample_count || count > sample_count - start_idx)
        throw std::out_of_range("requested slice exceeds stream sample range");

    const uint32_t footer_length = read_u32_at(stream, stream.size() - 4);
    const uint64_t expected_footer_length =
        kIndexHeaderSize + static_cast<uint64_t>(block_count) * kIndexEntrySize;
    if (footer_length != expected_footer_length ||
        footer_length > stream.size() - kIndexTrailerSize)
        throw CorruptedStreamException("invalid block index footer length");
    const std::size_t index_offset = stream.size() - kIndexTrailerSize - footer_length;
    Reader index(stream.subspan(index_offset, footer_length));
    if (index.u32() != block_count || index.u32() != INDEX_MAGIC)
        throw CorruptedStreamException("invalid block index header");

    std::vector<IndexEntry> entries;
    entries.reserve(block_count);
    uint64_t expected_sample = 0;
    for (uint32_t i = 0; i < block_count; ++i) {
        const IndexEntry entry{index.u64(), index.u32(), index.u32()};
        const uint32_t expected_count = static_cast<uint32_t>(
            std::min<std::size_t>(BLOCK_SIZE, sample_count - expected_sample));
        if (entry.start_sample_index != expected_sample || entry.sample_count != expected_count ||
            entry.byte_offset < kStreamHeaderSize || entry.byte_offset >= index_offset ||
            (entry.byte_offset & 63U) != 0)
            throw CorruptedStreamException("invalid block index entry");
        entries.push_back(entry);
        expected_sample += entry.sample_count;
    }
    if (!index.empty() || expected_sample != sample_count)
        throw CorruptedStreamException("inconsistent block index coverage");

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t offset = static_cast<std::size_t>(entries[i].byte_offset);
        const uint32_t body_size = read_u32_at(stream, offset);
        if (body_size < kMinimumBlockBodySize || body_size > kMaximumBlockBodySize)
            throw CorruptedStreamException("invalid indexed block size");
        const std::size_t record_size = align64(4ULL + body_size + 4ULL);
        const std::size_t expected_end = i + 1 < entries.size()
                                             ? static_cast<std::size_t>(entries[i + 1].byte_offset)
                                             : index_offset;
        if (record_size > index_offset - offset || offset + record_size != expected_end)
            throw CorruptedStreamException("block index offset is not a record boundary");
    }

    if (count == 0) return;
    const auto after_start = std::upper_bound(entries.begin(), entries.end(), start_idx,
                                              [](std::size_t value, const IndexEntry& entry) {
                                                  return value < entry.start_sample_index;
                                              });
    std::size_t block_index = static_cast<std::size_t>(after_start - entries.begin() - 1);
    const std::size_t slice_end = start_idx + count;
    std::size_t written = 0;
    std::vector<float> block_samples(BLOCK_SIZE);
    while (block_index < entries.size() && entries[block_index].start_sample_index < slice_end) {
        const IndexEntry& entry = entries[block_index];
        decode_block(stream, entry, global_flags,
                     std::span<float>(block_samples).first(entry.sample_count));
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

}  // namespace aether
