#include "aether/stream_encoder.hpp"

#include <bit>
#include <cstring>
#include <limits>

#include "aether/eclm_quantizer.hpp"
#include "aether/ged_estimator.hpp"
#include "aether/predictor.hpp"
#include "aether/rate_controller.hpp"
#include "aether/table.hpp"
#include "aether/vrans_codec.hpp"

namespace aether {
namespace {

constexpr uint8_t kRateMode = static_cast<uint8_t>(QuantizationMode::RATE_TARGETED);
constexpr uint8_t kErrorMode = static_cast<uint8_t>(QuantizationMode::ERROR_BOUNDED);
constexpr std::size_t kPreambleSize = 24;
constexpr std::size_t kStreamHeaderSize = 64;
constexpr std::size_t kMinimumBlockBodySize = 28;
constexpr std::size_t kMaximumBlockBodySize = 16 * 1024 * 1024;
constexpr std::size_t kMaximumBufferedStreamBytes = 256 * 1024 * 1024;
constexpr std::size_t kIndexHeaderSize = 8;
constexpr std::size_t kIndexEntrySize = 16;
constexpr std::size_t kIndexCrcSize = 4;
constexpr std::size_t kIndexTrailerSize = kIndexCrcSize;

void append_u8(std::vector<uint8_t>& out, uint8_t value) { out.push_back(value); }
void append_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}
void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}
void append_f32(std::vector<uint8_t>& out, float value) {
    append_u32(out, std::bit_cast<uint32_t>(value));
}

class StreamReader {
   public:
    explicit StreamReader(std::span<const uint8_t> input) : input_(input) {}

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
    float f32() { return std::bit_cast<float>(u32()); }
    std::span<const uint8_t> bytes(std::size_t count) {
        require(count);
        const auto result = input_.subspan(position_, count);
        position_ += count;
        return result;
    }
    bool empty() const noexcept { return position_ == input_.size(); }
    std::size_t remaining() const noexcept { return input_.size() - position_; }

   private:
    void require(std::size_t count) const {
        if (count > input_.size() - position_)
            throw CorruptedStreamException("AetherStream buffer overrun");
    }
    std::span<const uint8_t> input_;
    std::size_t position_ = 0;
};

bool finite_float(float value) noexcept { return std::isfinite(value); }

constexpr std::size_t align64(std::size_t value) noexcept {
    return (value + 63U) & ~std::size_t{63U};
}

void append_varuint(std::vector<uint8_t>& tokens, uint32_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
        value >>= 7;
        if (value != 0) byte |= 0x80U;
        tokens.push_back(byte);
    } while (value != 0);
}

std::vector<uint8_t> encode_error_payload(std::span<const int32_t> quanta) {
    // Token 0 is followed by a positive zero-run length. Token 1 is followed
    // by a zig-zag encoded non-zero quantum. The complete token byte stream is
    // entropy coded, so run lengths and residual magnitudes share one model.
    std::vector<uint8_t> tokens;
    tokens.reserve(quanta.size());
    for (std::size_t i = 0; i < quanta.size();) {
        if (quanta[i] == 0) {
            std::size_t end = i + 1;
            while (end < quanta.size() && quanta[end] == 0 &&
                   end - i < std::numeric_limits<uint32_t>::max())
                ++end;
            tokens.push_back(0);
            append_varuint(tokens, static_cast<uint32_t>(end - i));
            i = end;
            continue;
        }
        tokens.push_back(1);
        const int32_t q = quanta[i++];
        const uint32_t zigzag = q >= 0 ? static_cast<uint32_t>(q) * 2U
                                       : static_cast<uint32_t>(-static_cast<int64_t>(q) * 2 - 1);
        append_varuint(tokens, zigzag);
    }
    std::vector<float> histogram(256, 0.0f);
    for (uint8_t token : tokens) histogram[token] += 1.0f;
    return InterleavedRansEncoder(histogram).encode(tokens.data(), tokens.size());
}

float reconstructed_value(float prediction, int32_t quantum, float delta) {
    // One final rounding is both more accurate and reproducible than rounding
    // the residual product before adding it to the predictor.
    return static_cast<float>(static_cast<double>(prediction) +
                              static_cast<double>(quantum) * delta);
}

// Returns zero for an incomplete frame. Malformed framing is rejected before
// allocating according to attacker-controlled lengths.
std::size_t complete_frame_size(std::span<const uint8_t> bytes) {
    if (bytes.size() < kStreamHeaderSize) return 0;
    StreamReader reader(bytes);
    if (reader.u32() != MAGIC_HEADER) throw CorruptedStreamException("invalid AetherStream magic");
    const uint16_t version = reader.u16();
    if (reader.u16() != 0) throw CorruptedStreamException("unsupported stream header extension");
    const uint64_t sample_count = reader.u64();
    const uint32_t blocks = reader.u32();
    const uint32_t flags = reader.u32();
    validate_wire_format_version(version, (flags & STREAM_FLAG_BLOCK_INDEX) != 0);
    if ((flags & ~(STREAM_FLAG_ERROR_BOUNDED | STREAM_FLAG_BLOCK_INDEX)) != 0)
        throw CorruptedStreamException("unsupported incremental stream flags");
    const uint64_t expected_blocks =
        sample_count / BLOCK_SIZE + (sample_count % BLOCK_SIZE != 0 ? 1U : 0U);
    if (blocks != expected_blocks)
        throw CorruptedStreamException("inconsistent incremental frame block count");
    if (sample_count > kMaximumBufferedStreamBytes / sizeof(float))
        throw CorruptedStreamException("incremental frame sample limit exceeded");

    std::size_t offset = kStreamHeaderSize;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (bytes.size() - offset < 4) return 0;
        StreamReader size_reader(bytes.subspan(offset, 4));
        const uint32_t body_size = size_reader.u32();
        if (body_size < kMinimumBlockBodySize || body_size > kMaximumBlockBodySize)
            throw CorruptedStreamException("invalid block body size");
        const std::size_t record_size = align64(4ULL + body_size + 4ULL);
        if (record_size > bytes.size() - offset) return 0;
        offset += record_size;
    }
    if ((flags & STREAM_FLAG_BLOCK_INDEX) != 0) {
        const uint64_t footer_size_64 =
            kIndexHeaderSize + static_cast<uint64_t>(blocks) * kIndexEntrySize + kIndexTrailerSize;
        if constexpr (sizeof(std::size_t) < sizeof(uint64_t)) {
            if (footer_size_64 > std::numeric_limits<std::size_t>::max())
                throw CorruptedStreamException("index size overflow");
        }
        const std::size_t footer_size = static_cast<std::size_t>(footer_size_64);
        if (footer_size > bytes.size() - offset) return 0;
        StreamReader index(bytes.subspan(offset, footer_size));
        if (index.u32() != blocks || index.u32() != INDEX_MAGIC)
            throw CorruptedStreamException("invalid incremental stream index");
        index.bytes(static_cast<std::size_t>(blocks) * kIndexEntrySize);
        const std::size_t protected_size = footer_size - kIndexTrailerSize;
        if (index.u32() != crc32c(bytes.subspan(offset, protected_size)) || !index.empty())
            throw CorruptedStreamException("invalid incremental index footer");
        offset += footer_size;
    }
    return offset;
}

}  // namespace

AetherCodec::AetherCodec(float target_rate, float deadzone_factor, float absolute_error_bound,
                         bool enable_index, bool enable_adaptive_tail)
    : target_rate_(target_rate),
      deadzone_factor_(deadzone_factor),
      absolute_error_bound_(absolute_error_bound),
      enable_index_(enable_index),
      enable_adaptive_tail_(enable_adaptive_tail) {
    if (!finite_float(target_rate) || target_rate < 2.0f || target_rate > 8.0f)
        throw std::invalid_argument("target rate must be finite and in [2, 8]");
    if (!finite_float(deadzone_factor) || deadzone_factor < 0.0f)
        throw std::invalid_argument("deadzone factor must be finite and non-negative");
    if (!finite_float(absolute_error_bound) || absolute_error_bound < 0.0f)
        throw std::invalid_argument("absolute error bound must be finite and non-negative");
}

std::vector<uint8_t> AetherCodec::compress(std::span<const float> input) const {
    for (float value : input)
        if (!finite_float(value)) throw std::invalid_argument("input contains a non-finite value");

    const std::size_t blocks_size =
        input.size() / BLOCK_SIZE + (input.size() % BLOCK_SIZE != 0 ? 1U : 0U);
    if (blocks_size > std::numeric_limits<uint32_t>::max())
        throw std::length_error("input contains too many blocks");
    const uint32_t block_count = static_cast<uint32_t>(blocks_size);
    const bool error_bounded = absolute_error_bound_ > 0.0f;
    if (!error_bounded && input.empty())
        throw RateBudgetExceeded("an empty stream cannot satisfy a zero-byte rate budget");
    std::vector<uint8_t> output;
    const std::size_t per_sample = error_bounded ? 2U : 1U;
    if (input.size() <= (std::numeric_limits<std::size_t>::max() - 64) / per_sample) {
        const std::size_t estimate = input.size() * per_sample + 64;
        if (block_count <= (std::numeric_limits<std::size_t>::max() - estimate) / 160)
            output.reserve(estimate + static_cast<std::size_t>(block_count) * 160);
    }
    append_u32(output, MAGIC_HEADER);
    append_u16(output, WIRE_FORMAT_VERSION);
    append_u16(output, 0);
    append_u64(output, static_cast<uint64_t>(input.size()));
    append_u32(output, block_count);
    uint32_t global_flags = error_bounded ? STREAM_FLAG_ERROR_BOUNDED : 0U;
    if (enable_index_) global_flags |= STREAM_FLAG_BLOCK_INDEX;
    append_u32(output, global_flags);
    output.resize(kStreamHeaderSize, 0);

    std::vector<IndexEntry> index_entries;
    if (enable_index_) index_entries.reserve(block_count);
    const std::size_t index_wire_bytes =
        enable_index_ ? kIndexHeaderSize + static_cast<std::size_t>(block_count) * kIndexEntrySize +
                            kIndexTrailerSize
                      : 0;
    GedEstimator estimator;
    std::size_t rate_wire_bytes = kStreamHeaderSize + index_wire_bytes;
    for (std::size_t offset = 0; offset < input.size(); offset += BLOCK_SIZE) {
        const std::size_t count = std::min(BLOCK_SIZE, input.size() - offset);
        const float* block = input.data() + offset;
        if (enable_index_) {
            if (offset > std::numeric_limits<uint32_t>::max())
                throw std::length_error("indexed stream exceeds 32-bit sample positions");
            index_entries.push_back(IndexEntry{static_cast<uint64_t>(output.size()),
                                               static_cast<uint32_t>(offset),
                                               static_cast<uint32_t>(count)});
        }
        float harmonic_param = 0.0f;
        const PredictorMode mode = select_optimal_predictor(block, count, harmonic_param);

        std::vector<uint8_t> payload;
        std::vector<float> levels;
        float scale_or_alpha = 0.0f;
        float ged_beta = 2.0f;
        const uint8_t quant_mode = error_bounded ? kErrorMode : kRateMode;

        if (error_bounded) {
            const float delta = 2.0f * absolute_error_bound_;
            if (!finite_float(delta) || delta <= 0.0f)
                throw std::invalid_argument("absolute error bound is too large or too small");
            scale_or_alpha = delta;
            std::vector<int32_t> quanta;
            quanta.reserve(count);
            float previous_two = 0.0f;
            float previous = 0.0f;
            for (std::size_t i = 0; i < count; ++i) {
                const float prediction =
                    predict_sample(mode, harmonic_param, i, previous, previous_two);
                if (!finite_float(prediction)) throw std::overflow_error("predictor overflow");
                const double scaled = (static_cast<double>(block[i]) - prediction) / delta;
                const double rounded = std::nearbyint(scaled);
                if (rounded < std::numeric_limits<int32_t>::min() ||
                    rounded > std::numeric_limits<int32_t>::max())
                    throw std::overflow_error("error-bounded residual quantum overflow");
                const int32_t base = static_cast<int32_t>(rounded);
                int32_t best = base;
                float current = reconstructed_value(prediction, best, delta);
                float best_error = std::abs(block[i] - current);
                for (int adjustment = -2; adjustment <= 2; ++adjustment) {
                    const int64_t candidate64 = static_cast<int64_t>(base) + adjustment;
                    if (candidate64 < std::numeric_limits<int32_t>::min() ||
                        candidate64 > std::numeric_limits<int32_t>::max())
                        continue;
                    const int32_t candidate = static_cast<int32_t>(candidate64);
                    const float candidate_value = reconstructed_value(prediction, candidate, delta);
                    const float candidate_error = std::abs(block[i] - candidate_value);
                    if (candidate_error < best_error) {
                        best = candidate;
                        current = candidate_value;
                        best_error = candidate_error;
                    }
                }
                const float rounding_tolerance = 4.0f * std::numeric_limits<float>::epsilon() *
                                                 std::max(1.0f, std::abs(block[i]));
                if (!finite_float(current) ||
                    best_error > absolute_error_bound_ + rounding_tolerance)
                    throw std::overflow_error(
                        "requested error bound is not representable for input scale");
                quanta.push_back(best);
                previous_two = previous;
                previous = current;
            }
            payload = encode_error_payload(quanta);
        } else {
            std::vector<float> model_residuals(count);
            Predictor::residuals(block, model_residuals.data(), count, mode, harmonic_param);
            const GedParameters ged = estimator.estimate(model_residuals.data(), count);
            scale_or_alpha = ged.alpha;
            ged_beta = ged.beta;

            const std::size_t nominal_levels = static_cast<std::size_t>(
                std::clamp(std::lround(std::exp2(target_rate_)), 4L, 256L));
            // Full and independently feasible tail blocks reserve a complete
            // stream header, keeping batch and streaming decisions identical.
            const long double block_bits = static_cast<long double>(count) * target_rate_;
            const std::size_t block_budget =
                static_cast<std::size_t>(std::floor(block_bits / 8.0L));
            const std::size_t per_block_index_bytes =
                enable_index_
                    ? kIndexEntrySize + (offset == 0 ? kIndexHeaderSize + kIndexTrailerSize : 0)
                    : 0;
            const std::size_t fixed_block_budget = kStreamHeaderSize + per_block_index_bytes;
            std::size_t available_budget =
                block_budget > fixed_block_budget ? block_budget - fixed_block_budget : 0;
            // A short final block may be impossible as a standalone frame.
            // Batch mode may spend savings accumulated by preceding blocks
            // while retaining independent decisions whenever they are feasible.
            constexpr std::size_t minimum_rate_record = 256;
            if (available_budget < minimum_rate_record) {
                const long double cumulative_bits =
                    static_cast<long double>(offset + count) * target_rate_;
                const std::size_t cumulative_budget =
                    static_cast<std::size_t>(std::floor(cumulative_bits / 8.0L));
                if (cumulative_budget <= rate_wire_bytes)
                    throw RateBudgetExceeded(
                        "fixed stream header exceeds requested cumulative rate budget");
                available_budget = cumulative_budget - rate_wire_bytes;
            }
            RateController controller(target_rate_, count, 36, nominal_levels * sizeof(float),
                                      available_budget);
            float maximum_residual = 0.0f;
            for (float residual : model_residuals)
                maximum_residual = std::max(maximum_residual, std::abs(residual));
            float maximum_value = maximum_residual;
            for (std::size_t i = 0; i < count; ++i)
                maximum_value = std::max(maximum_value, std::abs(block[i]));
            float low_deadzone = deadzone_factor_;
            float high_deadzone = deadzone_factor_;
            if (ged.alpha > std::numeric_limits<float>::min())
                high_deadzone = std::max(low_deadzone + 1.0f, maximum_value / ged.alpha + 2.0f);
            float trial_deadzone = low_deadzone;
            float lambda = 0.0f;
            bool accepted = false;
            std::vector<float> accepted_levels;
            std::vector<uint8_t> accepted_payload;

            // First evaluate the entropy-derived design, then use a bounded
            // secant/dead-zone bisection regulator against actual serialized
            // bytes. Accepted candidates are retained while the lower half is
            // searched for the highest-fidelity statutory-rate solution.
            for (unsigned attempt = 0; attempt < 10; ++attempt) {
                ECLMQuantizer quantizer(std::max(controller.target_entropy(), 0.0f),
                                        trial_deadzone);
                quantizer.design(
                    ged, 40, lambda,
                    enable_adaptive_tail_ && target_rate_ >= 3.0f ? maximum_residual : 0.0f);
                if (quantizer.uses_impulsive_tail()) {
                    quantizer.fit_reconstruction_samples(model_residuals.data(), count);
                    std::vector<float> closed_loop_residuals(count);
                    for (unsigned refinement = 0; refinement < 3; ++refinement) {
                        float training_previous_two = 0.0f;
                        float training_previous = 0.0f;
                        for (std::size_t i = 0; i < count; ++i) {
                            const float prediction = predict_sample(
                                mode, harmonic_param, i, training_previous, training_previous_two);
                            const float residual = block[i] - prediction;
                            closed_loop_residuals[i] = residual;
                            const float current =
                                prediction + quantizer.reconstruct(quantizer.quantize(residual));
                            training_previous_two = training_previous;
                            training_previous = current;
                        }
                        quantizer.fit_reconstruction_samples(closed_loop_residuals.data(), count);
                    }
                    ECLMQuantizer baseline(std::max(controller.target_entropy(), 0.0f),
                                           trial_deadzone);
                    baseline.design(ged, 40, lambda, 0.0f);
                    const auto closed_loop_distortion = [&](const ECLMQuantizer& candidate) {
                        long double distortion = 0.0L;
                        float candidate_previous_two = 0.0f;
                        float candidate_previous = 0.0f;
                        for (std::size_t i = 0; i < count; ++i) {
                            const float prediction =
                                predict_sample(mode, harmonic_param, i, candidate_previous,
                                               candidate_previous_two);
                            const float current =
                                prediction +
                                candidate.reconstruct(candidate.quantize(block[i] - prediction));
                            const long double error = static_cast<long double>(block[i]) - current;
                            distortion += error * error;
                            candidate_previous_two = candidate_previous;
                            candidate_previous = current;
                        }
                        return distortion;
                    };
                    if (closed_loop_distortion(baseline) <= closed_loop_distortion(quantizer))
                        quantizer = std::move(baseline);
                }
                std::vector<uint8_t> symbols(count);
                std::vector<float> histogram(quantizer.levels().size(), 0.0f);
                float previous_two = 0.0f;
                float previous = 0.0f;
                for (std::size_t i = 0; i < count; ++i) {
                    const float prediction =
                        predict_sample(mode, harmonic_param, i, previous, previous_two);
                    const uint8_t symbol = quantizer.quantize(block[i] - prediction);
                    symbols[i] = symbol;
                    histogram[symbol] += 1.0f;
                    const float current = prediction + quantizer.reconstruct(symbol);
                    if (!finite_float(current))
                        throw std::overflow_error("rate-targeted predictor overflow");
                    previous_two = previous;
                    previous = current;
                }
                auto trial_payload =
                    InterleavedRansEncoder(histogram).encode(symbols.data(), count);
                const std::size_t wire_bytes =
                    align64(36 + quantizer.levels().size() * sizeof(float) + trial_payload.size());
                const bool trial_accepted = controller.accepts(wire_bytes);
                if (trial_accepted) {
                    accepted = true;
                    accepted_levels = quantizer.levels();
                    accepted_payload = std::move(trial_payload);
                    high_deadzone = trial_deadzone;
                } else {
                    low_deadzone = trial_deadzone;
                    lambda = controller.secant_step(lambda, wire_bytes, ged.alpha);
                }
                // Evaluate both endpoints before bisection. The high endpoint
                // maps every finite block value into the dead zone.
                trial_deadzone = (attempt == 0 && !trial_accepted)
                                     ? high_deadzone
                                     : 0.5f * (low_deadzone + high_deadzone);
                if (attempt != 0 &&
                    high_deadzone - low_deadzone <= 1e-4f * std::max(1.0f, high_deadzone))
                    break;
            }
            if (!accepted)
                throw RateBudgetExceeded(
                    "fixed framing cannot satisfy requested rate for this block");
            levels = std::move(accepted_levels);
            payload = std::move(accepted_payload);
            const std::size_t accepted_wire_bytes =
                align64(36 + levels.size() * sizeof(float) + payload.size());
            controller.enforce(accepted_wire_bytes);
            rate_wire_bytes += accepted_wire_bytes;
        }

        std::vector<uint8_t> body;
        body.reserve(kMinimumBlockBodySize + levels.size() * sizeof(float) + payload.size());
        append_u32(body, static_cast<uint32_t>(count));
        append_u8(body, static_cast<uint8_t>(mode));
        append_u8(body, quant_mode);
        append_u16(body, 0);
        append_f32(body, harmonic_param);
        append_f32(body, scale_or_alpha);
        append_f32(body, ged_beta);
        append_u16(body, static_cast<uint16_t>(levels.size()));
        append_u16(body, 0);
        append_u32(body, static_cast<uint32_t>(payload.size()));
        for (float level : levels) append_f32(body, level);
        body.insert(body.end(), payload.begin(), payload.end());
        if (body.size() > std::numeric_limits<uint32_t>::max())
            throw std::length_error("compressed block is too large");
        append_u32(output, static_cast<uint32_t>(body.size()));
        output.insert(output.end(), body.begin(), body.end());
        append_u32(output, crc32c(body));
        output.resize(align64(output.size()), 0);
    }
    if (enable_index_) {
        const std::size_t protected_size =
            kIndexHeaderSize + index_entries.size() * kIndexEntrySize;
        const std::size_t footer_length = protected_size + kIndexCrcSize;
        if (footer_length > std::numeric_limits<uint32_t>::max())
            throw std::length_error("index footer is too large");
        const std::size_t index_offset = output.size();
        append_u32(output, static_cast<uint32_t>(index_entries.size()));
        append_u32(output, INDEX_MAGIC);
        for (const IndexEntry& entry : index_entries) {
            append_u64(output, entry.byte_offset);
            append_u32(output, entry.start_sample_index);
            append_u32(output, entry.sample_count);
        }
        append_u32(output,
                   crc32c(std::span<const uint8_t>(output).subspan(index_offset, protected_size)));
    }
    if (!error_bounded) {
        const long double statutory_bits = static_cast<long double>(input.size()) * target_rate_;
        const std::size_t statutory_bytes =
            static_cast<std::size_t>(std::floor(statutory_bits / 8.0L));
        if (output.size() > statutory_bytes)
            throw std::logic_error("internal wire-rate regulator failure");
    }
    return output;
}

void AetherCodec::decompress(std::span<const uint8_t> input, std::span<float> output) const {
    StreamReader reader(input);
    if (reader.u32() != MAGIC_HEADER) throw CorruptedStreamException("invalid AetherStream magic");
    const uint16_t version = reader.u16();
    if (reader.u16() != 0) throw CorruptedStreamException("unsupported stream header extension");
    const uint64_t sample_count = reader.u64();
    const uint32_t block_count = reader.u32();
    const uint32_t global_flags = reader.u32();
    validate_wire_format_version(version, (global_flags & STREAM_FLAG_BLOCK_INDEX) != 0);
    for (uint8_t padding : reader.bytes(kStreamHeaderSize - kPreambleSize))
        if (padding != 0) throw CorruptedStreamException("non-zero stream-header padding");
    if ((global_flags & ~(STREAM_FLAG_ERROR_BOUNDED | STREAM_FLAG_BLOCK_INDEX)) != 0)
        throw CorruptedStreamException("unsupported global stream flags");
    if (sample_count != output.size())
        throw CorruptedStreamException("output length does not match stream");
    const uint64_t expected_blocks =
        sample_count / BLOCK_SIZE + (sample_count % BLOCK_SIZE != 0 ? 1U : 0U);
    if (block_count != expected_blocks) throw CorruptedStreamException("inconsistent block count");

    const bool has_index = (global_flags & STREAM_FLAG_BLOCK_INDEX) != 0;
    std::vector<IndexEntry> observed_entries;
    if (has_index) observed_entries.reserve(block_count);
    std::size_t output_offset = 0;
    for (uint32_t block_index = 0; block_index < block_count; ++block_index) {
        const std::size_t block_byte_offset = input.size() - reader.remaining();
        const std::size_t expected_count =
            std::min<std::size_t>(BLOCK_SIZE, output.size() - output_offset);
        const auto decoded =
            internal::decode_block_record(input.data() + block_byte_offset, reader.remaining(),
                                          output.subspan(output_offset, expected_count));
        if (decoded.samples_decoded != expected_count)
            throw CorruptedStreamException("invalid block sample count");
        const bool error_mode = decoded.quantization_mode == QuantizationMode::ERROR_BOUNDED;
        if (error_mode != ((global_flags & STREAM_FLAG_ERROR_BOUNDED) != 0))
            throw CorruptedStreamException("quantization mode disagrees with stream flags");
        reader.bytes(decoded.bytes_consumed);
        if (has_index) {
            observed_entries.push_back(IndexEntry{static_cast<uint64_t>(block_byte_offset),
                                                  static_cast<uint32_t>(output_offset),
                                                  static_cast<uint32_t>(decoded.samples_decoded)});
        }
        output_offset += decoded.samples_decoded;
    }
    if (has_index) {
        const uint32_t index_count = reader.u32();
        if (index_count != block_count || reader.u32() != INDEX_MAGIC)
            throw CorruptedStreamException("invalid block index header");
        for (uint32_t i = 0; i < index_count; ++i) {
            const IndexEntry entry{reader.u64(), reader.u32(), reader.u32()};
            const IndexEntry& observed = observed_entries[i];
            if (entry.byte_offset != observed.byte_offset ||
                entry.start_sample_index != observed.start_sample_index ||
                entry.sample_count != observed.sample_count)
                throw CorruptedStreamException("block index entry disagrees with stream");
        }
        const uint64_t protected_size =
            kIndexHeaderSize + static_cast<uint64_t>(index_count) * kIndexEntrySize;
        if (protected_size > std::numeric_limits<std::size_t>::max())
            throw CorruptedStreamException("block index size overflow");
        const std::size_t index_offset =
            input.size() - reader.remaining() - static_cast<std::size_t>(protected_size);
        if (reader.u32() !=
            crc32c(input.subspan(index_offset, static_cast<std::size_t>(protected_size))))
            throw CorruptedStreamException("block index CRC32-C mismatch");
        if (!reader.empty()) throw CorruptedStreamException("invalid block index footer length");
    }
    if (output_offset != output.size() || !reader.empty())
        throw CorruptedStreamException("inconsistent stream framing");
}

StreamEncoder::StreamEncoder(float target_rate, float deadzone_factor, float absolute_error_bound)
    : codec_(target_rate, deadzone_factor, absolute_error_bound) {
    buffer_.reserve(BLOCK_SIZE);
}

std::vector<uint8_t> StreamEncoder::feed(std::span<const float> chunk) {
    std::vector<uint8_t> output;
    std::size_t cursor = 0;
    while (cursor < chunk.size()) {
        const std::size_t take = std::min(BLOCK_SIZE - buffer_.size(), chunk.size() - cursor);
        const auto segment = chunk.subspan(cursor, take);
        buffer_.insert(buffer_.end(), segment.begin(), segment.end());
        cursor += take;
        if (buffer_.size() == BLOCK_SIZE) {
            const auto frame = codec_.compress(buffer_);
            output.insert(output.end(), frame.begin(), frame.end());
            buffer_.clear();
        }
    }
    return output;
}

std::vector<uint8_t> StreamEncoder::flush() {
    if (buffer_.empty()) return {};
    auto output = codec_.compress(buffer_);
    buffer_.clear();
    return output;
}

std::vector<float> StreamDecoder::feed(std::span<const uint8_t> chunk) {
    if (chunk.size() > kMaximumBufferedStreamBytes - buffer_.size())
        throw CorruptedStreamException("stream decoder buffer limit exceeded");
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    std::vector<float> output;
    std::size_t consumed = 0;
    while (consumed < buffer_.size()) {
        const auto pending = std::span<const uint8_t>(buffer_).subspan(consumed);
        const std::size_t frame_size = complete_frame_size(pending);
        if (frame_size == 0) break;
        StreamReader header(pending.first(frame_size));
        header.u32();
        header.u16();
        header.u16();
        const uint64_t sample_count = header.u64();
        if (sample_count > std::numeric_limits<std::size_t>::max() - output.size())
            throw CorruptedStreamException("decoded sample count overflow");
        const std::size_t old_size = output.size();
        output.resize(old_size + static_cast<std::size_t>(sample_count));
        AetherCodec().decompress(pending.first(frame_size),
                                 std::span<float>(output).subspan(old_size));
        consumed += frame_size;
    }
    if (consumed != 0)
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return output;
}

void StreamDecoder::flush() {
    if (!buffer_.empty()) throw CorruptedStreamException("truncated incremental stream");
}

}  // namespace aether
