#include "aether/common.hpp"

#include <bit>
#include <limits>

#include "aether/predictor.hpp"
#include "aether/vrans_codec.hpp"

namespace aether::internal {
namespace {

constexpr std::size_t kMinimumBlockBodySize = 28;
constexpr std::size_t kMaximumBlockBodySize = 16 * 1024 * 1024;
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

    float f32() { return std::bit_cast<float>(u32()); }

    std::span<const uint8_t> bytes(std::size_t count) {
        require(count);
        const auto result = input_.subspan(position_, count);
        position_ += count;
        return result;
    }

    std::size_t remaining() const noexcept { return input_.size() - position_; }

   private:
    void require(std::size_t count) const {
        if (count > input_.size() - position_)
            throw CorruptedStreamException("AetherStream block buffer overrun");
    }

    std::span<const uint8_t> input_;
    std::size_t position_ = 0;
};

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
    } catch (const CorruptedStreamException& error) {
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

}  // namespace

DecodedBlock decode_block_record(const uint8_t* block_ptr, std::size_t max_bytes,
                                 std::span<float> out_buffer, float initial_s1, float initial_s2) {
    if (block_ptr == nullptr) throw CorruptedStreamException("null compressed block pointer");
    Reader record(std::span<const uint8_t>(block_ptr, max_bytes));
    const uint32_t body_size = record.u32();
    if (body_size < kMinimumBlockBodySize || body_size > kMaximumBlockBodySize)
        throw CorruptedStreamException("invalid block body size");
    const std::size_t unpadded_size = 4ULL + body_size + 4ULL;
    const std::size_t record_size = align64(unpadded_size);
    if (record_size > max_bytes) throw CorruptedStreamException("truncated block record");
    const auto body = record.bytes(body_size);
    const uint32_t expected_crc = record.u32();
    for (uint8_t padding : record.bytes(record_size - unpadded_size))
        if (padding != 0) throw CorruptedStreamException("non-zero block padding");
    if (crc32c(body) != expected_crc) throw CorruptedStreamException("block CRC32-C mismatch");

    Reader block(body);
    const uint32_t count = block.u32();
    const uint8_t raw_mode = block.u8();
    const uint8_t raw_quantization = block.u8();
    if (block.u16() != 0) throw CorruptedStreamException("unsupported block flags");
    if (raw_mode > static_cast<uint8_t>(PredictorMode::LINEAR))
        throw CorruptedStreamException("invalid predictor mode");
    if (raw_quantization > kErrorMode) throw CorruptedStreamException("invalid quantization mode");
    if (count == 0 || count > BLOCK_SIZE || count > out_buffer.size())
        throw CorruptedStreamException("invalid block sample count");

    const PredictorMode predictor_mode = static_cast<PredictorMode>(raw_mode);
    const QuantizationMode quantization_mode = static_cast<QuantizationMode>(raw_quantization);
    const float harmonic_param = block.f32();
    const float scale_or_alpha = block.f32();
    const float ged_beta = block.f32();
    const uint16_t level_count = block.u16();
    if (block.u16() != 0) throw CorruptedStreamException("unsupported block extension");
    const uint32_t payload_bytes = block.u32();

    if (!std::isfinite(harmonic_param) ||
        (predictor_mode == PredictorMode::HARMONIC && std::abs(harmonic_param) > 0.999001f))
        throw CorruptedStreamException("invalid harmonic predictor parameter");
    if (!std::isfinite(scale_or_alpha) || !std::isfinite(ged_beta))
        throw CorruptedStreamException("non-finite block model parameter");
    if (quantization_mode == QuantizationMode::RATE_TARGETED &&
        (scale_or_alpha < 0.0f || ged_beta < 0.2f || ged_beta > 5.0f))
        throw CorruptedStreamException("invalid GED parameters");

    std::vector<float> levels;
    if (raw_quantization == kRateMode) {
        if (level_count < 4 || level_count > 256)
            throw CorruptedStreamException("invalid quantizer codebook size");
        levels.resize(level_count);
        for (float& level : levels) {
            level = block.f32();
            if (!std::isfinite(level)) throw CorruptedStreamException("non-finite codebook level");
        }
    } else if (level_count != 0) {
        throw CorruptedStreamException("error mode cannot contain a codebook");
    }
    if (payload_bytes != block.remaining())
        throw CorruptedStreamException("invalid compressed payload length");
    const auto payload = block.bytes(payload_bytes);

    float previous = initial_s1;
    float previous_two = initial_s2;
    if (quantization_mode == QuantizationMode::ERROR_BOUNDED) {
        if (scale_or_alpha <= 0.0f) throw CorruptedStreamException("invalid error-bounded delta");
        const auto quanta = read_error_payload(payload, count);
        for (std::size_t i = 0; i < count; ++i) {
            const float prediction =
                predict_sample(predictor_mode, harmonic_param, i, previous, previous_two);
            const float current = reconstructed_value(prediction, quanta[i], scale_or_alpha);
            if (!std::isfinite(current))
                throw CorruptedStreamException("non-finite reconstruction");
            out_buffer[i] = current;
            previous_two = previous;
            previous = current;
        }
    } else {
        std::vector<uint8_t> symbols;
        try {
            symbols = InterleavedRansDecoder::decode(payload.data(), payload.size(), count);
        } catch (const CorruptedStreamException& error) {
            throw CorruptedStreamException(std::string("invalid rANS payload: ") + error.what());
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (symbols[i] >= levels.size())
                throw CorruptedStreamException("symbol exceeds codebook");
            const float prediction =
                predict_sample(predictor_mode, harmonic_param, i, previous, previous_two);
            const float current = prediction + levels[symbols[i]];
            if (!std::isfinite(current))
                throw CorruptedStreamException("non-finite reconstruction");
            out_buffer[i] = current;
            previous_two = previous;
            previous = current;
        }
    }

    return DecodedBlock{count, record_size, previous, previous_two, quantization_mode};
}

}  // namespace aether::internal
