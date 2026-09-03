#include "aether/vrans_codec.hpp"

#include <limits>
#include <numeric>
#include <type_traits>

namespace aether {
namespace {

constexpr uint32_t kRansLowerBound = 1U << 16;
constexpr uint32_t kStreamMagic = 0x31525341;

template <typename T>
void write_value(uint8_t*& destination, T value) {
    static_assert(std::is_unsigned_v<T>);
    for (std::size_t i = 0; i < sizeof(T); ++i)
        *destination++ = static_cast<uint8_t>(value >> (8 * i));
}

template <typename T>
T read_value(const uint8_t*& source, const uint8_t* end) {
    static_assert(std::is_unsigned_v<T>);
    if (static_cast<std::size_t>(end - source) < sizeof(T))
        throw std::runtime_error("truncated rANS stream");
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) value |= static_cast<T>(source[i]) << (8 * i);
    source += sizeof(T);
    return value;
}

}  // namespace

InterleavedRansEncoder::InterleavedRansEncoder(const std::vector<float>& probabilities) {
    normalize(probabilities);
}

void InterleavedRansEncoder::normalize(const std::vector<float>& probabilities) {
    std::vector<double> weights(256, probabilities.empty() ? 1.0 : 0.0);
    const std::size_t supplied = std::min<std::size_t>(weights.size(), probabilities.size());
    for (std::size_t i = 0; i < supplied; ++i) weights[i] = std::max(0.0f, probabilities[i]);

    if (!probabilities.empty()) {
        std::size_t used = supplied;
        while (used > 1 && weights[used - 1] == 0.0) --used;
        alphabet_size_ = static_cast<uint16_t>(used);
    }

    double weight_sum = std::accumulate(weights.begin(), weights.end(), 0.0);
    if (weight_sum <= 0.0) {
        std::fill(weights.begin(), weights.end(), 1.0);
        weight_sum = weights.size();
        alphabet_size_ = 256;
    }

    struct Remainder {
        double value;
        int symbol;
    };

    std::vector<Remainder> remainders;
    remainders.reserve(256);
    int allocated = 0;

    for (int symbol = 0; symbol < 256; ++symbol) {
        const double exact = weights[symbol] / weight_sum * SCALE_TOTAL;
        const int frequency =
            weights[symbol] > 0.0 ? std::max(1, static_cast<int>(std::floor(exact))) : 0;
        frequencies_[symbol] = static_cast<uint16_t>(frequency);
        allocated += frequency;
        remainders.push_back({exact - std::floor(exact), symbol});
    }

    std::sort(
        remainders.begin(), remainders.end(),
        [](const Remainder& left, const Remainder& right) { return left.value > right.value; });
    for (int i = 0; allocated < static_cast<int>(SCALE_TOTAL); ++i, ++allocated) {
        ++frequencies_[remainders[i % remainders.size()].symbol];
    }

    while (allocated > static_cast<int>(SCALE_TOTAL)) {
        int largest = -1;
        for (int symbol = 0; symbol < 256; ++symbol) {
            if (frequencies_[symbol] > 1 &&
                (largest < 0 || frequencies_[symbol] > frequencies_[largest])) {
                largest = symbol;
            }
        }
        if (largest < 0) {
            throw std::runtime_error("frequency normalization failed");
        }
        --frequencies_[largest];
        --allocated;
    }

    cumulative_[0] = 0;
    for (int symbol = 0; symbol < 256; ++symbol) {
        cumulative_[symbol + 1] = static_cast<uint16_t>(cumulative_[symbol] + frequencies_[symbol]);
    }
}

std::size_t InterleavedRansEncoder::encode_block(const uint8_t* symbols, std::size_t count,
                                                 uint8_t* output) const {
    std::array<uint32_t, RANS_STATES> states;
    states.fill(kRansLowerBound);
    std::array<std::vector<uint16_t>, RANS_STATES> words;

    // rANS runs backward. Interleaving by position keeps each lane independent.
    for (std::size_t position = count; position-- > 0;) {
        const unsigned lane = position & (RANS_STATES - 1);
        const unsigned symbol = symbols[position];
        const unsigned frequency = frequencies_[symbol];
        if (frequency == 0) {
            throw std::runtime_error("symbol has zero frequency");
        }

        // Promote before multiplying: a frequency of SCALE_TOTAL makes the
        // 32-bit intermediate exactly 2^32 and would otherwise wrap to zero,
        // causing an unbounded renormalization loop for single-symbol blocks.
        const uint64_t maximum_state =
            (static_cast<uint64_t>(kRansLowerBound >> SCALE_BITS) << 16) * frequency;
        while (states[lane] >= maximum_state) {
            words[lane].push_back(static_cast<uint16_t>(states[lane]));
            states[lane] >>= 16;
        }

        states[lane] = (states[lane] / frequency) * SCALE_TOTAL + (states[lane] % frequency) +
                       cumulative_[symbol];
    }

    uint8_t* cursor = output;
    write_value(cursor, kStreamMagic);
    write_value(cursor, static_cast<uint64_t>(count));
    uint16_t active_symbols = 0;
    for (uint16_t frequency : frequencies_) active_symbols += frequency != 0 ? 1U : 0U;
    write_value(cursor, active_symbols);
    for (unsigned symbol = 0; symbol < 256; ++symbol) {
        if (frequencies_[symbol] == 0) continue;
        write_value(cursor, static_cast<uint8_t>(symbol));
        write_value(cursor, frequencies_[symbol]);
    }
    for (unsigned lane = 0; lane < RANS_STATES; ++lane) {
        write_value(cursor, states[lane]);
        write_value(cursor, static_cast<uint32_t>(words[lane].size()));
    }

    // Renormalization words were produced backward, so store each lane reversed.
    for (unsigned lane = 0; lane < RANS_STATES; ++lane) {
        for (auto word = words[lane].rbegin(); word != words[lane].rend(); ++word) {
            write_value(cursor, *word);
        }
    }

    return static_cast<std::size_t>(cursor - output);
}

std::vector<uint8_t> InterleavedRansEncoder::encode(const uint8_t* symbols,
                                                    std::size_t count) const {
    if (count > (std::numeric_limits<std::size_t>::max() - 1024) / 2)
        throw std::length_error("rANS input is too large");
    if (count != 0 && symbols == nullptr) throw std::invalid_argument("null rANS symbol input");
    std::vector<uint8_t> output(max_compressed_size(count));
    output.resize(encode_block(symbols, count, output.data()));
    return output;
}

std::vector<uint8_t> InterleavedRansDecoder::decode(const uint8_t* data, std::size_t size,
                                                    std::size_t expected_count,
                                                    std::size_t maximum_count) {
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;

    if (read_value<uint32_t>(cursor, end) != kStreamMagic) {
        throw std::runtime_error("bad rANS magic");
    }

    const uint64_t count = read_value<uint64_t>(cursor, end);
    if (expected_count != 0 && count != expected_count) throw std::runtime_error("length mismatch");
    if (count > std::numeric_limits<std::size_t>::max() || count > maximum_count)
        throw std::runtime_error("unreasonable rANS symbol count");

    std::array<uint16_t, 256> frequencies{};
    std::array<uint16_t, 256> cumulative{};
    const uint16_t active_symbols = read_value<uint16_t>(cursor, end);
    if (active_symbols == 0 || active_symbols > 256)
        throw std::runtime_error("invalid rANS active-symbol count");
    uint32_t frequency_sum = 0;
    for (std::size_t entry = 0; entry < active_symbols; ++entry) {
        const uint8_t symbol = read_value<uint8_t>(cursor, end);
        const uint16_t frequency = read_value<uint16_t>(cursor, end);
        if (frequency == 0 || frequencies[symbol] != 0)
            throw std::runtime_error("invalid or duplicate rANS frequency entry");
        frequencies[symbol] = frequency;
        frequency_sum += frequency;
    }
    if (frequency_sum != SCALE_TOTAL) {
        throw std::runtime_error("invalid frequency table");
    }

    uint32_t cumulative_value = 0;
    for (int symbol = 0; symbol < 256; ++symbol) {
        cumulative[symbol] = static_cast<uint16_t>(cumulative_value);
        cumulative_value += frequencies[symbol];
    }

    std::array<uint32_t, RANS_STATES> states{};
    std::array<uint32_t, RANS_STATES> word_counts{};
    std::array<uint32_t, RANS_STATES> word_positions{};
    for (unsigned lane = 0; lane < RANS_STATES; ++lane) {
        states[lane] = read_value<uint32_t>(cursor, end);
        word_counts[lane] = read_value<uint32_t>(cursor, end);
    }

    std::array<const uint8_t*, RANS_STATES> lane_streams{};
    for (unsigned lane = 0; lane < RANS_STATES; ++lane) {
        lane_streams[lane] = cursor;
        const std::size_t byte_count = 2ULL * word_counts[lane];
        if (static_cast<std::size_t>(end - cursor) < byte_count) {
            throw std::runtime_error("truncated words");
        }
        cursor += byte_count;
    }
    if (cursor != end) {
        throw std::runtime_error("trailing rANS data");
    }

    std::array<uint8_t, SCALE_TOTAL> symbol_table{};
    for (int symbol = 0; symbol < 256; ++symbol) {
        const unsigned limit = cumulative[symbol] + frequencies[symbol];
        for (unsigned slot = cumulative[symbol]; slot < limit; ++slot) {
            symbol_table[slot] = static_cast<uint8_t>(symbol);
        }
    }

    std::vector<uint8_t> output(count);
    for (std::size_t position = 0; position < count; ++position) {
        const unsigned lane = position & (RANS_STATES - 1);
        const unsigned slot = states[lane] & (SCALE_TOTAL - 1);
        const unsigned symbol = symbol_table[slot];
        output[position] = static_cast<uint8_t>(symbol);

        states[lane] = static_cast<uint64_t>(frequencies[symbol]) * (states[lane] >> SCALE_BITS) +
                       slot - cumulative[symbol];

        while (states[lane] < kRansLowerBound) {
            if (word_positions[lane] >= word_counts[lane]) {
                throw std::runtime_error("rANS underflow");
            }

            const uint8_t* source = lane_streams[lane] + 2 * word_positions[lane]++;
            const uint16_t word =
                static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1]) << 8;
            states[lane] = (states[lane] << 16) | word;
        }
    }

    for (unsigned lane = 0; lane < RANS_STATES; ++lane) {
        if (word_positions[lane] != word_counts[lane] || states[lane] != kRansLowerBound)
            throw std::runtime_error("inconsistent final rANS state");
    }
    return output;
}

}  // namespace aether
