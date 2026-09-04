#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "aether/aether.hpp"
#include "aether/stream_encoder.hpp"

int main() {
    std::vector<float> signal(100000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = std::sin(0.02f * static_cast<float>(i)) +
                    0.25f * std::sin(0.19f * static_cast<float>(i));
    }

    aether::AetherCodec codec(3.0f);
    const std::vector<uint8_t> batch_encoded = codec.compress(signal);
    std::vector<float> batch_decoded(signal.size());
    codec.decompress(batch_encoded, batch_decoded);
    assert(batch_encoded[4] == aether::WIRE_FORMAT_VERSION && batch_encoded[5] == 0);

    // Wire-v5 unindexed frames remain readable because their block framing is
    // unchanged. Versions outside the explicit v5-v6 compatibility window are
    // rejected with a dedicated exception.
    auto legacy_unindexed = batch_encoded;
    legacy_unindexed[4] = 5;
    std::vector<float> legacy_decoded(signal.size());
    codec.decompress(legacy_unindexed, legacy_decoded);
    assert(legacy_decoded == batch_decoded);
    for (uint8_t invalid_version : {uint8_t{4}, uint8_t{7}}) {
        auto unsupported = batch_encoded;
        unsupported[4] = invalid_version;
        bool unsupported_rejected = false;
        try {
            codec.decompress(unsupported, legacy_decoded);
        } catch (const aether::UnsupportedWireFormatException&) {
            unsupported_rejected = true;
        }
        assert(unsupported_rejected);
    }

    aether::StreamEncoder encoder(3.0f);
    std::vector<uint8_t> wire;
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> sample_sizes(13, 257);
    std::size_t offset = 0;
    while (offset < signal.size()) {
        const std::size_t take = std::min<std::size_t>(sample_sizes(rng), signal.size() - offset);
        const auto bytes = encoder.feed(std::span<const float>(signal.data() + offset, take));
        wire.insert(wire.end(), bytes.begin(), bytes.end());
        offset += take;
    }
    const auto tail = encoder.flush();
    wire.insert(wire.end(), tail.begin(), tail.end());
    assert(encoder.flush().empty());

    // Network byte boundaries are unrelated to sample packet boundaries.
    aether::StreamDecoder decoder;
    std::vector<float> streamed;
    std::uniform_int_distribution<int> byte_sizes(1, 997);
    offset = 0;
    while (offset < wire.size()) {
        const std::size_t take = std::min<std::size_t>(byte_sizes(rng), wire.size() - offset);
        const auto decoded = decoder.feed(std::span<const uint8_t>(wire.data() + offset, take));
        streamed.insert(streamed.end(), decoded.begin(), decoded.end());
        offset += take;
    }
    decoder.flush();

    assert(streamed.size() == batch_decoded.size());
    for (std::size_t i = 0; i < streamed.size(); ++i) assert(streamed[i] == batch_decoded[i]);

    // A single large feed emits several independently framed blocks, all of
    // which the incremental decoder must consume in one call.
    aether::StreamEncoder large_encoder(3.0f);
    auto large_wire = large_encoder.feed(signal);
    const auto large_tail = large_encoder.flush();
    large_wire.insert(large_wire.end(), large_tail.begin(), large_tail.end());
    aether::StreamDecoder large_decoder;
    const auto large_result = large_decoder.feed(large_wire);
    large_decoder.flush();
    assert(large_result == batch_decoded);

    aether::StreamDecoder truncated_decoder;
    truncated_decoder.feed(std::span<const uint8_t>(wire.data(), wire.size() - 1));
    bool rejected = false;
    try {
        truncated_decoder.flush();
    } catch (const aether::CorruptedStreamException&) {
        rejected = true;
    }
    assert(rejected);
}
