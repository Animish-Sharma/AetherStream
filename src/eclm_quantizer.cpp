#include "aether/eclm_quantizer.hpp"

#include <limits>

#include "aether/simd_dispatch.hpp"

namespace aether {
namespace {

constexpr int kGammaIterations = 240;
constexpr double kGammaTolerance = 1e-13;

double regularized_gamma_p(double shape, double x) {
    if (x <= 0.0) return 0.0;
    if (!std::isfinite(x) || x > 750.0) return 1.0;

    if (x < shape + 1.0) {
        double term = 1.0 / shape;
        double sum = term;
        double denominator = shape;
        for (int i = 1; i < kGammaIterations; ++i) {
            denominator += 1.0;
            term *= x / denominator;
            sum += term;
            if (std::abs(term) < std::abs(sum) * kGammaTolerance) {
                break;
            }
        }
        return sum * std::exp(-x + shape * std::log(x) - std::lgamma(shape));
    }

    double b = x + 1.0 - shape;
    double c = 1e300;
    double d = 1.0 / b;
    double fraction = d;
    for (int i = 1; i < kGammaIterations; ++i) {
        const double numerator = -i * (i - shape);
        b += 2.0;
        d = numerator * d + b;
        d = std::abs(d) < 1e-300 ? 1e-300 : d;
        c = b + numerator / c;
        c = std::abs(c) < 1e-300 ? 1e-300 : c;
        d = 1.0 / d;
        const double delta = d * c;
        fraction *= delta;
        if (std::abs(delta - 1.0) < kGammaTolerance) {
            break;
        }
    }

    return 1.0 - std::exp(-x + shape * std::log(x) - std::lgamma(shape)) * fraction;
}

double ged_cdf(double value, const GedParameters& ged) {
    if (value == -std::numeric_limits<double>::infinity()) return 0.0;
    if (value == std::numeric_limits<double>::infinity()) return 1.0;
    if (ged.alpha <= 0.0f) return value < ged.mean ? 0.0 : 1.0;
    const double centered = (value - ged.mean) / ged.alpha;
    const double argument = std::pow(std::abs(centered), ged.beta);
    const double mass = std::clamp(regularized_gamma_p(1.0 / ged.beta, argument), 0.0, 1.0);
    return centered < 0.0 ? 0.5 * (1.0 - mass) : 0.5 * (1.0 + mass);
}

double ged_partial_moment(double value, const GedParameters& ged) {
    if (value == -std::numeric_limits<double>::infinity()) return 0.0;
    if (value == std::numeric_limits<double>::infinity()) return ged.mean;
    const double probability = ged_cdf(value, ged);
    if (ged.alpha <= 0.0f) return ged.mean * probability;

    const double centered = (value - ged.mean) / ged.alpha;
    const double argument = std::pow(std::abs(centered), ged.beta);
    const double gamma_one = std::tgamma(1.0 / ged.beta);
    const double gamma_two = std::tgamma(2.0 / ged.beta);
    const double partial = regularized_gamma_p(2.0 / ged.beta, argument);
    const double centered_moment = centered < 0.0
                                       ? -0.5 * ged.alpha * gamma_two * (1.0 - partial) / gamma_one
                                       : 0.5 * ged.alpha * gamma_two * (partial - 1.0) / gamma_one;
    return ged.mean * probability + centered_moment;
}

std::size_t levels_for_rate(float target_rate) {
    if (!std::isfinite(target_rate)) throw std::invalid_argument("target rate must be finite");
    const double requested = std::exp2(std::clamp(target_rate, 0.0f, 8.0f));
    return static_cast<std::size_t>(std::clamp(std::lround(requested), 4L, 256L));
}

}  // namespace

ECLMQuantizer::ECLMQuantizer(float target_rate, float deadzone_factor)
    : ECLMQuantizer(levels_for_rate(target_rate), target_rate, deadzone_factor) {}

ECLMQuantizer::ECLMQuantizer(std::size_t levels, float target_rate, float deadzone_factor)
    : level_count_(levels),
      center_index_(levels / 2),
      target_rate_(target_rate),
      deadzone_factor_(deadzone_factor) {
    if (levels < 4 || levels > 256) {
        throw std::invalid_argument("quantizer levels must be in [4, 256]");
    }
    if (!std::isfinite(target_rate) || target_rate < 0.0f || target_rate > 8.0f)
        throw std::invalid_argument("target rate must be finite and in [0, 8]");
    if (!std::isfinite(deadzone_factor) || deadzone_factor < 0.0f)
        throw std::invalid_argument("deadzone factor must be finite and non-negative");

    levels_.resize(level_count_);
    codebook_.resize(level_count_);
    boundaries_.resize(level_count_ + 1);
    probabilities_.resize(level_count_);
    codebook_probabilities_.resize(level_count_);
}

uint8_t ECLMQuantizer::code_for_cell(std::size_t cell) const {
    if (cell == center_index_) return 0;
    return static_cast<uint8_t>(cell < center_index_ ? cell + 1 : cell);
}

std::size_t ECLMQuantizer::cell_for_code(uint8_t code) const {
    if (code == 0) return center_index_;
    return code <= center_index_ ? static_cast<std::size_t>(code - 1)
                                 : static_cast<std::size_t>(code);
}

void ECLMQuantizer::rebuild_codebook() {
    for (std::size_t cell = 0; cell < level_count_; ++cell) {
        const uint8_t code = code_for_cell(cell);
        codebook_[code] = levels_[cell];
        codebook_probabilities_[code] = probabilities_[cell];
    }
    rebuild_polynomial();
}

void ECLMQuantizer::rebuild_polynomial() {
    float scale = std::max(ged_.alpha, 1e-12f);
    for (std::size_t i = 1; i + 1 < boundaries_.size(); ++i) {
        if (std::isfinite(boundaries_[i])) scale = std::max(scale, std::abs(boundaries_[i]));
    }
    polynomial_.inverse_scale = 1.0f / scale;
    polynomial_.cell_count = static_cast<uint32_t>(level_count_);

    // Least-squares fit the exact monotone boundary rank in each normalized
    // half-quadrant. Sampling cell interiors rather than only boundaries makes
    // the truncated polynomial result stable under scalar and vector FMA.
    constexpr unsigned samples = 65;
    for (unsigned segment = 0; segment < 4; ++segment) {
        double normal[4][5]{};
        const double segment_start = -1.0 + 0.5 * segment;
        for (unsigned sample = 0; sample < samples; ++sample) {
            const double epsilon = (sample + 0.5) / samples;
            const double normalized = segment_start + 0.5 * epsilon;
            const float value = static_cast<float>(normalized * scale);
            const auto boundary =
                std::upper_bound(boundaries_.begin() + 1, boundaries_.end() - 1, value);
            const double target = static_cast<double>(boundary - boundaries_.begin() - 1) + 0.45;
            const double powers[7] = {1.0,
                                      epsilon,
                                      epsilon * epsilon,
                                      epsilon * epsilon * epsilon,
                                      std::pow(epsilon, 4),
                                      std::pow(epsilon, 5),
                                      std::pow(epsilon, 6)};
            for (unsigned row = 0; row < 4; ++row) {
                for (unsigned column = 0; column < 4; ++column)
                    normal[row][column] += powers[row + column];
                normal[row][4] += powers[row] * target;
            }
        }
        for (unsigned diagonal = 0; diagonal < 4; ++diagonal) {
            unsigned pivot = diagonal;
            for (unsigned row = diagonal + 1; row < 4; ++row) {
                if (std::abs(normal[row][diagonal]) > std::abs(normal[pivot][diagonal]))
                    pivot = row;
            }
            for (unsigned column = diagonal; column < 5; ++column)
                std::swap(normal[diagonal][column], normal[pivot][column]);
            const double divisor = normal[diagonal][diagonal];
            if (std::abs(divisor) < 1e-14)
                throw std::runtime_error("singular polynomial quantizer fit");
            for (unsigned column = diagonal; column < 5; ++column)
                normal[diagonal][column] /= divisor;
            for (unsigned row = 0; row < 4; ++row) {
                if (row == diagonal) continue;
                const double factor = normal[row][diagonal];
                for (unsigned column = diagonal; column < 5; ++column)
                    normal[row][column] -= factor * normal[diagonal][column];
            }
        }
        for (unsigned coefficient = 0; coefficient < 4; ++coefficient)
            polynomial_.coefficients[segment][coefficient] =
                static_cast<float>(normal[coefficient][4]);
    }
}

void ECLMQuantizer::update_cells() {
    double probability_sum = 0.0;
    for (std::size_t i = 0; i < level_count_; ++i) {
        const double probability =
            ged_cdf(boundaries_[i + 1], ged_) - ged_cdf(boundaries_[i], ged_);
        probabilities_[i] = static_cast<float>(std::max(probability, 1e-15));
        probability_sum += probabilities_[i];

        if (i != center_index_ && probability > 1e-14) {
            const double moment = ged_partial_moment(boundaries_[i + 1], ged_) -
                                  ged_partial_moment(boundaries_[i], ged_);
            levels_[i] = static_cast<float>(moment / probability);
        }
    }

    levels_[center_index_] = 0.0f;
    for (float& probability : probabilities_) {
        probability = static_cast<float>(probability / probability_sum);
    }
}

void ECLMQuantizer::design(const GedParameters& ged, unsigned iterations, float initial_lambda) {
    if (!std::isfinite(ged.alpha) || ged.alpha < 0.0f || !std::isfinite(ged.beta) ||
        ged.beta < 0.2f || ged.beta > 5.0f || !std::isfinite(initial_lambda) ||
        initial_lambda < 0.0f)
        throw std::invalid_argument("invalid GED quantizer parameters");
    ged_ = ged;
    ged_.mean = 0.0f;
    deadzone_threshold_ = deadzone_factor_ * ged.alpha;

    if (ged.alpha <= std::numeric_limits<float>::min()) {
        std::fill(levels_.begin(), levels_.end(), 0.0f);
        std::fill(probabilities_.begin(), probabilities_.end(), 0.0f);
        probabilities_[center_index_] = 1.0f;
        std::fill(boundaries_.begin(), boundaries_.end(), 0.0f);
        rebuild_codebook();
        return;
    }

    const double tail_span = ged.alpha * std::pow(std::log(2e9), 1.0 / std::max(ged.beta, 0.2f));
    const double span = std::max(tail_span, static_cast<double>(deadzone_threshold_) * 1.01);

    boundaries_.front() = -std::numeric_limits<float>::infinity();
    boundaries_.back() = std::numeric_limits<float>::infinity();
    boundaries_[center_index_] = -deadzone_threshold_;
    boundaries_[center_index_ + 1] = deadzone_threshold_;

    for (std::size_t i = 1; i < center_index_; ++i) {
        const double fraction = static_cast<double>(i) / center_index_;
        boundaries_[i] = static_cast<float>(-span + fraction * (span - deadzone_threshold_));
    }

    const std::size_t positive_cells = level_count_ - center_index_ - 1;
    for (std::size_t offset = 1; offset < positive_cells; ++offset) {
        const double fraction = static_cast<double>(offset) / positive_cells;
        boundaries_[center_index_ + 1 + offset] =
            static_cast<float>(deadzone_threshold_ + fraction * (span - deadzone_threshold_));
    }

    for (std::size_t i = 0; i < level_count_; ++i) {
        const float left =
            std::isfinite(boundaries_[i]) ? boundaries_[i] : static_cast<float>(-span);
        const float right =
            std::isfinite(boundaries_[i + 1]) ? boundaries_[i + 1] : static_cast<float>(span);
        levels_[i] = 0.5f * (left + right);
    }
    update_cells();

    lambda_ = initial_lambda;
    float ascent_step = 0.06f * ged.alpha * ged.alpha;
    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        std::vector<float> next = boundaries_;
        for (std::size_t i = 1; i < level_count_; ++i) {
            if (i == center_index_ || i == center_index_ + 1) {
                continue;
            }

            const double gap = levels_[i] - levels_[i - 1];
            if (gap <= 1e-12) {
                continue;
            }
            const double midpoint = 0.5 * (levels_[i] + levels_[i - 1]);
            const double correction =
                lambda_ / (2.0 * gap) * std::log2(probabilities_[i - 1] / probabilities_[i]);
            const double guard = std::min(1e-7 * static_cast<double>(ged.alpha), 0.25 * gap);
            next[i] = static_cast<float>(std::clamp(midpoint + correction,
                                                    static_cast<double>(levels_[i - 1]) + guard,
                                                    static_cast<double>(levels_[i]) - guard));
        }

        boundaries_.swap(next);
        update_cells();
        lambda_ = std::max(0.0f, lambda_ + ascent_step * (entropy() - target_rate_));
        ascent_step *= 0.96f;
    }
    rebuild_codebook();
}

void ECLMQuantizer::fit_samples(const float* samples, std::size_t count, unsigned iterations) {
    if (count == 0) return;
    if (samples == nullptr) throw std::invalid_argument("null quantizer training input");

    for (unsigned iteration = 0; iteration < iterations; ++iteration) {
        std::vector<double> sums(level_count_, 0.0);
        std::vector<std::size_t> counts(level_count_, 0);
        for (std::size_t i = 0; i < count; ++i) {
            const uint8_t code = quantize(samples[i]);
            if (code != 0) {
                sums[code] += samples[i];
                ++counts[code];
            }
        }
        for (std::size_t code = 1; code < level_count_; ++code) {
            if (counts[code] != 0) codebook_[code] = static_cast<float>(sums[code] / counts[code]);
        }
        codebook_[0] = 0.0f;
        for (std::size_t cell = 0; cell < level_count_; ++cell)
            levels_[cell] = codebook_[code_for_cell(cell)];
        for (std::size_t i = 1; i < level_count_; ++i) {
            if (i != center_index_ && i != center_index_ + 1)
                boundaries_[i] = 0.5f * (levels_[i - 1] + levels_[i]);
        }
    }

    std::fill(probabilities_.begin(), probabilities_.end(), 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t code = quantize(samples[i]);
        probabilities_[cell_for_code(code)] += 1.0f;
    }
    for (float& probability : probabilities_) probability /= static_cast<float>(count);
    rebuild_codebook();
}

uint8_t ECLMQuantizer::quantize(float value) const {
    if (!std::isfinite(value)) throw std::invalid_argument("quantizer input must be finite");
    if (std::abs(value) <= deadzone_threshold_) return 0;
    std::size_t cell = simd::polynomial_cell(value, polynomial_);
    if (cell == center_index_) cell = value < 0.0f ? center_index_ - 1 : center_index_ + 1;
    return code_for_cell(cell);
}

void ECLMQuantizer::quantize(const float* values, uint8_t* output, std::size_t count) const {
    if (count != 0 && (values == nullptr || output == nullptr))
        throw std::invalid_argument("null quantizer array");
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i]))
            throw std::invalid_argument("quantizer input must be finite");
    simd::polynomial_quantize(values, polynomial_, output, count);
    for (std::size_t i = 0; i < count; ++i) {
        if (std::abs(values[i]) <= deadzone_threshold_) {
            output[i] = 0;
        } else {
            std::size_t cell = output[i];
            if (cell == center_index_)
                cell = values[i] < 0.0f ? center_index_ - 1 : center_index_ + 1;
            output[i] = code_for_cell(cell);
        }
    }
}

}  // namespace aether
