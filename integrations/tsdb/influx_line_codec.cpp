#include "influx_line_codec.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace aether::tsdb {
namespace {

constexpr uint32_t kMagic = 0x42445441U;  // ATDB
constexpr std::size_t kHeaderSize = 20;

void append_u32(std::vector<uint8_t>& output, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) output.push_back(static_cast<uint8_t>(value >> (8U * i)));
}

void append_u64(std::vector<uint8_t>& output, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) output.push_back(static_cast<uint8_t>(value >> (8U * i)));
}

uint32_t read_u32(std::span<const uint8_t> input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 4)
        throw CorruptedStreamException("truncated TSDB chunk header");
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<uint32_t>(input[offset + i]) << (8U * i);
    return value;
}

uint64_t read_u64(std::span<const uint8_t> input, std::size_t offset) {
    if (offset > input.size() || input.size() - offset < 8)
        throw CorruptedStreamException("truncated TSDB chunk header");
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= static_cast<uint64_t>(input[offset + i]) << (8U * i);
    return value;
}

uint64_t zigzag(int64_t value) {
    if (value >= 0) return static_cast<uint64_t>(value) << 1U;
    return (static_cast<uint64_t>(-(value + 1)) << 1U) | 1U;
}

int64_t unzigzag(uint64_t value) {
    const uint64_t magnitude = value >> 1U;
    if ((value & 1U) == 0) {
        if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw CorruptedStreamException("TSDB timestamp varint overflow");
        return static_cast<int64_t>(magnitude);
    }
    if (magnitude == static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::min();
    return -static_cast<int64_t>(magnitude) - 1;
}

void append_varint(std::vector<uint8_t>& output, uint64_t value) {
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7fU);
        value >>= 7U;
        if (value != 0) byte |= 0x80U;
        output.push_back(byte);
    } while (value != 0);
}

uint64_t take_varint(std::span<const uint8_t> input, std::size_t& cursor) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 10; ++i) {
        if (cursor == input.size())
            throw CorruptedStreamException("truncated TSDB timestamp varint");
        const uint8_t byte = input[cursor++];
        if (i == 9 && (byte & 0xfeU) != 0)
            throw CorruptedStreamException("TSDB timestamp varint overflow");
        value |= static_cast<uint64_t>(byte & 0x7fU) << (7U * i);
        if ((byte & 0x80U) == 0) return value;
    }
    throw CorruptedStreamException("unterminated TSDB timestamp varint");
}

std::size_t find_separator(std::string_view text, std::size_t start, char separator,
                           bool honor_quotes) {
    bool escaped = false;
    bool quoted = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char current = text[i];
        if (escaped) {
            escaped = false;
        } else if (current == '\\') {
            escaped = true;
        } else if (honor_quotes && current == '"') {
            quoted = !quoted;
        } else if (!quoted && current == separator) {
            return i;
        }
    }
    return std::string_view::npos;
}

int64_t parse_timestamp(std::string_view text) {
    int64_t timestamp = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), timestamp);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid InfluxDB timestamp");
    return timestamp;
}

float parse_field_value(std::string_view fields, std::string_view requested_name) {
    std::size_t offset = 0;
    while (offset < fields.size()) {
        const std::size_t end = find_separator(fields, offset, ',', true);
        const std::string_view field = fields.substr(
            offset, end == std::string_view::npos ? fields.size() - offset : end - offset);
        const std::size_t equals = find_separator(field, 0, '=', true);
        if (equals == std::string_view::npos || equals == 0 || equals + 1 == field.size())
            throw std::invalid_argument("invalid InfluxDB field set");
        const std::string_view name = field.substr(0, equals);
        const std::string_view encoded_value = field.substr(equals + 1);
        if ((requested_name.empty() || name == requested_name) && encoded_value.front() != '"' &&
            encoded_value != "true" && encoded_value != "false") {
            std::string value_text(encoded_value);
            if (!value_text.empty() && (value_text.back() == 'i' || value_text.back() == 'u'))
                value_text.pop_back();
            char* parsed_end = nullptr;
            const float value = std::strtof(value_text.c_str(), &parsed_end);
            if (parsed_end == value_text.c_str() || *parsed_end != '\0' || !std::isfinite(value))
                throw std::invalid_argument("InfluxDB field is not a finite float");
            return value;
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    throw std::invalid_argument("requested numeric InfluxDB field is missing");
}

Point parse_line(std::string_view line, std::string_view field_name) {
    const std::size_t fields_begin = find_separator(line, 0, ' ', false);
    if (fields_begin == std::string_view::npos)
        throw std::invalid_argument("InfluxDB point is missing fields and timestamp");
    const std::size_t timestamp_begin = find_separator(line, fields_begin + 1, ' ', true);
    if (timestamp_begin == std::string_view::npos || timestamp_begin + 1 == line.size())
        throw std::invalid_argument("InfluxDB point is missing a timestamp");
    const std::string_view fields =
        line.substr(fields_begin + 1, timestamp_begin - fields_begin - 1);
    const std::string_view timestamp = line.substr(timestamp_begin + 1);
    return Point{parse_timestamp(timestamp), parse_field_value(fields, field_name)};
}

}  // namespace

std::vector<uint8_t> encode_points(std::span<const Point> points, const CodecConfig& config) {
    if (points.empty()) throw std::invalid_argument("TSDB chunk requires at least one point");
    if (points.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("TSDB chunk contains too many points");

    std::vector<uint8_t> timestamp_bytes;
    timestamp_bytes.reserve(points.size() * 2);
    int64_t previous_delta = 0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].timestamp <= points[i - 1].timestamp)
            throw std::invalid_argument("TSDB timestamps must be strictly increasing");
        const uint64_t unsigned_delta = static_cast<uint64_t>(points[i].timestamp) -
                                        static_cast<uint64_t>(points[i - 1].timestamp);
        if (unsigned_delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            throw std::invalid_argument("TSDB timestamp delta is too large");
        const int64_t delta = static_cast<int64_t>(unsigned_delta);
        append_varint(timestamp_bytes, zigzag(i == 1 ? delta : delta - previous_delta));
        previous_delta = delta;
    }
    if (timestamp_bytes.size() > std::numeric_limits<uint32_t>::max())
        throw std::length_error("TSDB timestamp section is too large");

    std::vector<float> values;
    values.reserve(points.size());
    for (const Point& point : points) {
        if (!std::isfinite(point.value)) throw std::invalid_argument("TSDB value must be finite");
        values.push_back(point.value);
    }
    const std::vector<uint8_t> wire = AetherCodec(config).compress(values);

    std::vector<uint8_t> output;
    output.reserve(kHeaderSize + timestamp_bytes.size() + wire.size());
    append_u32(output, kMagic);
    append_u32(output, static_cast<uint32_t>(points.size()));
    append_u32(output, static_cast<uint32_t>(timestamp_bytes.size()));
    append_u64(output, std::bit_cast<uint64_t>(points.front().timestamp));
    output.insert(output.end(), timestamp_bytes.begin(), timestamp_bytes.end());
    output.insert(output.end(), wire.begin(), wire.end());
    return output;
}

std::vector<uint8_t> encode_influx_lines(std::string_view lines, std::string_view field_name,
                                         const CodecConfig& config) {
    std::vector<Point> points;
    std::size_t offset = 0;
    while (offset < lines.size()) {
        const std::size_t newline = lines.find('\n', offset);
        std::string_view line = lines.substr(
            offset, newline == std::string_view::npos ? lines.size() - offset : newline - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (!line.empty()) points.push_back(parse_line(line, field_name));
        if (newline == std::string_view::npos) break;
        offset = newline + 1;
    }
    return encode_points(points, config);
}

std::vector<uint8_t> encode_prometheus_chunk(std::span<const int64_t> timestamps,
                                             std::span<const float> values,
                                             const CodecConfig& config) {
    if (timestamps.size() != values.size())
        throw std::invalid_argument("Prometheus timestamp and value counts differ");
    std::vector<Point> points;
    points.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        points.push_back(Point{timestamps[i], values[i]});
    return encode_points(points, config);
}

DecodedChunk decode_chunk(std::span<const uint8_t> payload) {
    if (payload.size() < kHeaderSize || read_u32(payload, 0) != kMagic)
        throw CorruptedStreamException("invalid TSDB chunk magic");
    const std::size_t count = read_u32(payload, 4);
    const std::size_t timestamp_size = read_u32(payload, 8);
    if (count == 0 || timestamp_size > payload.size() - kHeaderSize)
        throw CorruptedStreamException("invalid TSDB chunk metadata");

    const auto timestamp_bytes = payload.subspan(kHeaderSize, timestamp_size);
    const auto wire = payload.subspan(kHeaderSize + timestamp_size);
    if (wire.empty()) throw CorruptedStreamException("TSDB chunk is missing AetherStream data");

    DecodedChunk decoded;
    decoded.timestamps.resize(count);
    decoded.values.resize(count);
    decoded.timestamps[0] = std::bit_cast<int64_t>(read_u64(payload, 12));
    std::size_t cursor = 0;
    int64_t previous_delta = 0;
    for (std::size_t i = 1; i < count; ++i) {
        const int64_t encoded_delta = unzigzag(take_varint(timestamp_bytes, cursor));
        int64_t delta = encoded_delta;
        if (i != 1) {
            if ((encoded_delta > 0 &&
                 previous_delta > std::numeric_limits<int64_t>::max() - encoded_delta) ||
                (encoded_delta < 0 &&
                 previous_delta < std::numeric_limits<int64_t>::min() - encoded_delta))
                throw CorruptedStreamException("TSDB timestamp delta overflow");
            delta = previous_delta + encoded_delta;
        }
        if (delta <= 0 || decoded.timestamps[i - 1] > std::numeric_limits<int64_t>::max() - delta)
            throw CorruptedStreamException("invalid TSDB timestamp sequence");
        decoded.timestamps[i] = decoded.timestamps[i - 1] + delta;
        previous_delta = delta;
    }
    if (cursor != timestamp_bytes.size())
        throw CorruptedStreamException("trailing TSDB timestamp data");
    AetherCodec().decompress(wire, decoded.values);
    return decoded;
}

}  // namespace aether::tsdb
