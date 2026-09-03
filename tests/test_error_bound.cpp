#include <cassert>
#include <cmath>
#include <vector>

#include "aether/aether.hpp"

int main() {
    std::vector<float> signal(10000);
    for (std::size_t i = 0; i < signal.size(); ++i) {
        signal[i] = 0.5f * std::sin(0.01f * static_cast<float>(i)) +
                    0.1f * std::cos(0.17f * static_cast<float>(i));
    }

    aether::AetherCodec codec(4.0f, 0.3f, 0.005f);
    const std::vector<uint8_t> encoded = codec.compress(signal);
    std::vector<float> reconstructed(signal.size());
    codec.decompress(encoded, reconstructed);

    float max_error = 0.0f;
    for (std::size_t i = 0; i < signal.size(); ++i) {
        max_error = std::max(max_error, std::abs(signal[i] - reconstructed[i]));
    }
    assert(max_error <= 0.005001f);
}
