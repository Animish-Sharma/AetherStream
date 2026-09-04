#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "influx_line_codec.hpp"

int main() {
    constexpr std::size_t count = 100'000;
    constexpr int64_t start = 1'672'531'199'000'000'000LL;
    constexpr int64_t interval = 10'000'000LL;

    std::ostringstream line_protocol;
    line_protocol << std::setprecision(9);
    std::vector<int64_t> timestamps(count);
    std::vector<float> values(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float phase = static_cast<float>(i);
        timestamps[i] = start + static_cast<int64_t>(i) * interval;
        values[i] =
            0.65f * std::sin(phase * 0.011f) + 0.08f * std::cos(phase * 0.071f) + 0.000002f * phase;
        line_protocol << "accelerometer,station=alpha value=" << values[i] << ' ' << timestamps[i]
                      << '\n';
    }

    aether::CodecConfig config;
    config.absolute_error_bound = 0.0001f;
    config.enable_index = true;
    const auto encoded = aether::tsdb::encode_influx_lines(line_protocol.str(), "value", config);
    const auto decoded = aether::tsdb::decode_chunk(encoded);
    assert(decoded.timestamps == timestamps);
    assert(decoded.values.size() == values.size());
    for (std::size_t i = 0; i < count; ++i)
        assert(std::abs(decoded.values[i] - values[i]) <= 0.000101f);

    const auto prometheus = aether::tsdb::encode_prometheus_chunk(timestamps, values, config);
    const auto prometheus_decoded = aether::tsdb::decode_chunk(prometheus);
    assert(prometheus_decoded.timestamps == timestamps);
    assert(prometheus_decoded.values == decoded.values);

    auto corrupted = encoded;
    corrupted.at(0) ^= 0x80U;
    bool rejected = false;
    try {
        (void)aether::tsdb::decode_chunk(corrupted);
    } catch (const aether::CorruptedStreamException&) {
        rejected = true;
    }
    assert(rejected);
    return 0;
}
