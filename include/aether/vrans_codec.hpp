#pragma once

#include <array>
#include <limits>

#include "common.hpp"

namespace aether {

class InterleavedRansEncoder {
   public:
    explicit InterleavedRansEncoder(const std::vector<float>& probabilities = {});

    std::size_t max_compressed_size(std::size_t count) const {
        if (count > (std::numeric_limits<std::size_t>::max() - 1024U) / 2U)
            throw std::length_error("rANS input is too large");
        return 1024U + 2U * count;
    }

    std::size_t encode_block(const uint8_t* symbols, std::size_t count, uint8_t* output) const;
    std::vector<uint8_t> encode(const uint8_t* symbols, std::size_t count) const;

    const std::array<uint16_t, 256>& frequencies() const { return frequencies_; }

   private:
    void normalize(const std::vector<float>& probabilities);

    std::array<uint16_t, 256> frequencies_{};
    std::array<uint16_t, 257> cumulative_{};
};

class InterleavedRansDecoder {
   public:
    static std::vector<uint8_t> decode(const uint8_t* data, std::size_t size,
                                       std::size_t expected_count = 0,
                                       std::size_t maximum_count = (std::size_t{1} << 30));
};

// Kept as a small compatibility facade for applications using the v1 name.
class VRansCodec : public InterleavedRansEncoder {
   public:
    using InterleavedRansEncoder::InterleavedRansEncoder;

    static std::vector<uint8_t> decode(const uint8_t* data, std::size_t size,
                                       std::size_t expected_count = 0,
                                       std::size_t maximum_count = (std::size_t{1} << 30)) {
        return InterleavedRansDecoder::decode(data, size, expected_count, maximum_count);
    }
};

}  // namespace aether
