#include <cassert>
#include <random>
#include <type_traits>
#include <vector>

#include "aether/vrans_codec.hpp"

int main() {
    static_assert(std::is_base_of_v<aether::StreamError, aether::CorruptedStreamException>);

    std::mt19937 random(19);
    std::vector<uint8_t> symbols(100'003);
    std::vector<float> counts(256, 0.0f);
    for (uint8_t& symbol : symbols) {
        symbol = static_cast<uint8_t>(random() & 63U);
        counts[symbol] += 1.0f;
    }

    aether::InterleavedRansEncoder encoder(counts);
    const std::vector<uint8_t> encoded = encoder.encode(symbols.data(), symbols.size());
    const std::vector<uint8_t> decoded =
        aether::InterleavedRansDecoder::decode(encoded.data(), encoded.size(), symbols.size());
    assert(decoded == symbols);

    for (std::size_t count = 0; count < 40; ++count) {
        const std::vector<uint8_t> short_stream = encoder.encode(symbols.data(), count);
        const std::vector<uint8_t> short_result =
            aether::InterleavedRansDecoder::decode(short_stream.data(), short_stream.size(), count);
        assert(std::equal(short_result.begin(), short_result.end(), symbols.begin()));
    }

    // A one-symbol model has frequency SCALE_TOTAL. The renormalization
    // threshold must be calculated in 64 bits rather than wrapping at 2^32.
    std::vector<float> single_probability(1, 1.0f);
    aether::InterleavedRansEncoder single_encoder(single_probability);
    const uint8_t zero = 0;
    const auto single_stream = single_encoder.encode(&zero, 1);
    const auto single_result =
        aether::InterleavedRansDecoder::decode(single_stream.data(), single_stream.size(), 1);
    assert(single_result.size() == 1 && single_result[0] == 0);

    bool rejected = false;
    try {
        static_cast<void>(aether::InterleavedRansDecoder::decode(nullptr, 0));
    } catch (const aether::CorruptedStreamException&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        static_cast<void>(encoder.encode_block(symbols.data(), symbols.size(), nullptr));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
