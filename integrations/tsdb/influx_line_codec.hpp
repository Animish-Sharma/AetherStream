#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "aether/aether.hpp"

namespace aether::tsdb {

struct Point {
    int64_t timestamp = 0;
    float value = 0.0f;
};

struct DecodedChunk {
    std::vector<int64_t> timestamps;
    std::vector<float> values;
};

// Encodes timestamps with delta-of-delta varints and values as one AetherStream frame.
std::vector<uint8_t> encode_points(std::span<const Point> points,
                                   const CodecConfig& config = CodecConfig{});

// Parses one numeric field from each InfluxDB line-protocol point and encodes the batch.
// An empty field_name selects the first numeric field in each point.
std::vector<uint8_t> encode_influx_lines(std::string_view lines, std::string_view field_name,
                                         const CodecConfig& config = CodecConfig{});

// Prometheus remote-write adapters can pass each ordered series directly after protobuf decoding.
std::vector<uint8_t> encode_prometheus_chunk(std::span<const int64_t> timestamps,
                                             std::span<const float> values,
                                             const CodecConfig& config = CodecConfig{});

DecodedChunk decode_chunk(std::span<const uint8_t> payload);

}  // namespace aether::tsdb
