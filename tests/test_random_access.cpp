#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#include "aether/aether.hpp"
#include "aether/table.hpp"

namespace {

void check_slice(const std::vector<uint8_t>& encoded, const std::vector<float>& full,
                 std::size_t start, std::size_t count) {
    std::vector<float> slice(count);
    aether::decompress_slice(encoded, start, count, slice);
    for (std::size_t i = 0; i < count; ++i) assert(slice[i] == full[start + i]);
}

}  // namespace

int main() {
    std::vector<float> samples(aether::BLOCK_SIZE * 7 + 317);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float x = static_cast<float>(i);
        samples[i] = std::sin(x * 0.017f) + 0.31f * std::cos(x * 0.071f) + 0.00003f * x;
    }

    const aether::AetherCodec indexed(4.0f, 0.3f, 0.0f, true);
    const auto encoded = indexed.compress(samples);
    assert(encoded[4] == aether::WIRE_FORMAT_VERSION && encoded[5] == 0);
    auto deprecated_indexed = encoded;
    deprecated_indexed[4] = 5;
    bool deprecated_rejected = false;
    try {
        std::vector<float> ignored(samples.size());
        indexed.decompress(deprecated_indexed, ignored);
    } catch (const aether::DeprecatedWireFormatException& error) {
        deprecated_rejected = std::string(error.what()) ==
                              "Wire format v5 indexed footers are deprecated. Re-encode using v6.";
    }
    assert(deprecated_rejected);

    std::vector<float> full(samples.size());
    indexed.decompress(encoded, full);
    const aether::IndexedStreamView cached(encoded);
    assert(cached.sample_count() == samples.size());
    assert(cached.block_count() == 8);
    std::vector<float> cached_slice(97);
    cached.decompress_slice(aether::BLOCK_SIZE - 23, cached_slice.size(), cached_slice);
    for (std::size_t i = 0; i < cached_slice.size(); ++i)
        assert(cached_slice[i] == full[aether::BLOCK_SIZE - 23 + i]);

    check_slice(encoded, full, 0, 1);
    check_slice(encoded, full, 17, 301);
    check_slice(encoded, full, aether::BLOCK_SIZE - 23, 97);
    check_slice(encoded, full, aether::BLOCK_SIZE * 2, aether::BLOCK_SIZE);
    check_slice(encoded, full, aether::BLOCK_SIZE * 3 - 1, aether::BLOCK_SIZE * 2 + 9);
    check_slice(encoded, full, samples.size() - 211, 211);
    check_slice(encoded, full, samples.size(), 0);

    bool rejected = false;
    try {
        std::vector<float> invalid(2);
        aether::decompress_slice(encoded, samples.size() - 1, 2, invalid);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    assert(rejected);

    rejected = false;
    try {
        const auto unindexed = aether::AetherCodec(4.0f).compress(samples);
        std::vector<float> output(10);
        aether::decompress_slice(unindexed, 0, output.size(), output);
    } catch (const aether::CorruptedStreamException&) {
        rejected = true;
    }
    assert(rejected);

    const auto low_rate = aether::AetherCodec(2.0f, 0.3f, 0.0f, true).compress(samples);
    assert(low_rate.size() * 8ULL <= samples.size() * 2ULL);

    const aether::AetherCodec error_indexed(4.0f, 0.3f, 0.002f, true);
    const auto error_wire = error_indexed.compress(samples);
    std::vector<float> error_full(samples.size());
    error_indexed.decompress(error_wire, error_full);
    check_slice(error_wire, error_full, aether::BLOCK_SIZE - 9, 47);

    auto corrupted = encoded;
    corrupted[corrupted.size() - 8] ^= 0x20;
    rejected = false;
    try {
        std::vector<float> output(10);
        aether::decompress_slice(corrupted, 0, output.size(), output);
    } catch (const aether::CorruptedStreamException&) {
        rejected = true;
    }
    assert(rejected);
}
