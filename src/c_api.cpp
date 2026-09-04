#include "aether/c_api.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>

#include "aether/aether.hpp"
#include "aether/table.hpp"

namespace {

constexpr const char* kVersion = "0.0.1";

uint16_t read_u16_le(const uint8_t* input) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(input[0]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U));
}

uint32_t read_u32_le(const uint8_t* input) noexcept {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(input[i]) << (8U * i);
    return value;
}

uint64_t read_u64_le(const uint8_t* input) noexcept {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(input[i]) << (8U * i);
    return value;
}

aether_status stream_sample_count(const uint8_t* input, size_t input_bytes,
                                  size_t& sample_count) noexcept {
    if (input_bytes < 24 || read_u32_le(input) != aether::MAGIC_HEADER)
        return AETHER_ERR_CORRUPTED_STREAM;
    const uint16_t version = read_u16_le(input + 4);
    if (read_u16_le(input + 6) != 0) return AETHER_ERR_CORRUPTED_STREAM;
    const uint64_t count = read_u64_le(input + 8);
    const uint32_t blocks = read_u32_le(input + 16);
    const uint32_t flags = read_u32_le(input + 20);
    if (version == 5 && (flags & aether::STREAM_FLAG_BLOCK_INDEX) != 0)
        return AETHER_ERR_DEPRECATED_FORMAT;
    if (version < aether::MINIMUM_COMPATIBLE_WIRE_FORMAT_VERSION ||
        version > aether::WIRE_FORMAT_VERSION)
        return AETHER_ERR_CORRUPTED_STREAM;
    if ((flags & ~(aether::STREAM_FLAG_ERROR_BOUNDED | aether::STREAM_FLAG_BLOCK_INDEX)) != 0)
        return AETHER_ERR_CORRUPTED_STREAM;
    const uint64_t expected_blocks =
        count / aether::BLOCK_SIZE + (count % aether::BLOCK_SIZE != 0 ? 1U : 0U);
    if (expected_blocks > std::numeric_limits<uint32_t>::max() || blocks != expected_blocks ||
        count > std::numeric_limits<size_t>::max())
        return AETHER_ERR_CORRUPTED_STREAM;
    sample_count = static_cast<size_t>(count);
    return AETHER_OK;
}

aether_status translate_current_exception() noexcept {
    try {
        throw;
    } catch (const aether::DeprecatedWireFormatException&) {
        return AETHER_ERR_DEPRECATED_FORMAT;
    } catch (const aether::RateBudgetExceeded&) {
        return AETHER_ERR_RATE_BUDGET_EXCEEDED;
    } catch (const aether::CorruptedStreamException&) {
        return AETHER_ERR_CORRUPTED_STREAM;
    } catch (const std::invalid_argument&) {
        return AETHER_ERR_INVALID_ARG;
    } catch (const std::out_of_range&) {
        return AETHER_ERR_INVALID_ARG;
    } catch (...) {
        return AETHER_ERR_INTERNAL;
    }
}

bool valid_config(const aether_config_t& config) noexcept {
    return std::isfinite(config.target_rate) && config.target_rate >= 2.0f &&
           config.target_rate <= 8.0f && std::isfinite(config.absolute_error_bound) &&
           config.absolute_error_bound >= 0.0f && std::isfinite(config.deadzone_factor) &&
           config.deadzone_factor >= 0.0f && config.enable_crc <= 1 && config.enable_index <= 1;
}

}  // namespace

extern "C" {

aether_status aether_compress(const float* input, size_t input_count, const aether_config_t* config,
                              uint8_t* output, size_t output_capacity, size_t* bytes_written) {
    if (input == nullptr || config == nullptr || output == nullptr || bytes_written == nullptr)
        return AETHER_ERR_INVALID_ARG;
    *bytes_written = 0;
    if (!valid_config(*config)) return AETHER_ERR_INVALID_ARG;
    try {
        aether::CodecConfig native_config;
        native_config.target_rate = config->target_rate;
        native_config.absolute_error_bound = config->absolute_error_bound;
        native_config.deadzone_factor = config->deadzone_factor;
        native_config.enable_index = config->enable_index != 0;
        const std::vector<uint8_t> encoded =
            aether::AetherCodec(native_config).compress(std::span<const float>(input, input_count));
        *bytes_written = encoded.size();
        if (output_capacity < encoded.size()) return AETHER_ERR_BUFFER_TOO_SMALL;
        if (!encoded.empty()) std::memcpy(output, encoded.data(), encoded.size());
        return AETHER_OK;
    } catch (...) {
        return translate_current_exception();
    }
}

aether_status aether_decompress(const uint8_t* input, size_t input_bytes, float* output,
                                size_t output_capacity, size_t* samples_written) {
    if (input == nullptr || output == nullptr || samples_written == nullptr)
        return AETHER_ERR_INVALID_ARG;
    *samples_written = 0;
    size_t required = 0;
    const aether_status header_status = stream_sample_count(input, input_bytes, required);
    if (header_status != AETHER_OK) return header_status;
    *samples_written = required;
    if (output_capacity < required) return AETHER_ERR_BUFFER_TOO_SMALL;
    try {
        aether::AetherCodec().decompress(std::span<const uint8_t>(input, input_bytes),
                                         std::span<float>(output, required));
        return AETHER_OK;
    } catch (...) {
        *samples_written = 0;
        return translate_current_exception();
    }
}

aether_status aether_decompress_slice(const uint8_t* input, size_t input_bytes, size_t start_sample,
                                      size_t count, float* output, size_t output_capacity,
                                      size_t* samples_written) {
    if (input == nullptr || output == nullptr || samples_written == nullptr)
        return AETHER_ERR_INVALID_ARG;
    *samples_written = count;
    if (output_capacity < count) return AETHER_ERR_BUFFER_TOO_SMALL;
    try {
        aether::decompress_slice(std::span<const uint8_t>(input, input_bytes), start_sample, count,
                                 std::span<float>(output, count));
        return AETHER_OK;
    } catch (...) {
        *samples_written = 0;
        return translate_current_exception();
    }
}

uint32_t aether_c_abi_version(void) { return AETHER_C_ABI_VERSION; }

const char* aether_version_string(void) { return kVersion; }

const char* aether_status_to_string(aether_status status) {
    switch (status) {
        case AETHER_OK:
            return "ok";
        case AETHER_ERR_INVALID_ARG:
            return "invalid argument";
        case AETHER_ERR_BUFFER_TOO_SMALL:
            return "buffer too small";
        case AETHER_ERR_CORRUPTED_STREAM:
            return "corrupted or unsupported stream";
        case AETHER_ERR_RATE_BUDGET_EXCEEDED:
            return "rate budget exceeded";
        case AETHER_ERR_DEPRECATED_FORMAT:
            return "deprecated wire format";
        case AETHER_ERR_INTERNAL:
            return "internal error";
        default:
            return "unknown status";
    }
}

}  // extern "C"
