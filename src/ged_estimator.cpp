#include "aether/ged_estimator.hpp"

#include <array>
#include <bit>
#include <limits>

#include "aether/simd_dispatch.hpp"

namespace aether {
namespace {

bool finite_float(float value) noexcept {
    return (std::bit_cast<uint32_t>(value) & 0x7f800000U) != 0x7f800000U;
}

double moment_ratio(double beta) {
    return std::exp(2.0 * std::lgamma(2.0 / beta) - std::lgamma(1.0 / beta) -
                    std::lgamma(3.0 / beta));
}

// A dense deterministic table avoids a nonlinear solve for every telemetry
// block while retaining substantially more accuracy than a piecewise fit.
float shape_from_ratio(double ratio) {
    constexpr std::size_t table_size = 4097;
    constexpr double beta_min = 0.2;
    constexpr double beta_max = 5.0;
    static const std::array<double, table_size> table = [] {
        std::array<double, table_size> values{};
        for (std::size_t i = 0; i < table_size; ++i) {
            const double beta = beta_min + (beta_max - beta_min) * static_cast<double>(i) /
                                               static_cast<double>(table_size - 1);
            values[i] = moment_ratio(beta);
        }
        return values;
    }();

    if (ratio <= table.front()) return static_cast<float>(beta_min);
    if (ratio >= table.back()) return static_cast<float>(beta_max);
    const auto upper = std::lower_bound(table.begin(), table.end(), ratio);
    const std::size_t hi = static_cast<std::size_t>(upper - table.begin());
    const std::size_t lo = hi - 1;
    const double fraction = (ratio - table[lo]) / (table[hi] - table[lo]);
    const double index = static_cast<double>(lo) + fraction;
    return static_cast<float>(beta_min +
                              (beta_max - beta_min) * index / static_cast<double>(table_size - 1));
}

}  // namespace

GedParameters GedEstimator::estimate(const float* residuals, std::size_t count) const {
    if (residuals == nullptr || count == 0)
        throw std::invalid_argument("GED estimation requires residuals");

    double sum = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!finite_float(residuals[i])) throw std::invalid_argument("non-finite residual");
        sum += static_cast<double>(residuals[i]);
    }
    const float mean = static_cast<float>(sum / static_cast<double>(count));

    constexpr std::size_t chunk_size = 4096;
    std::array<float, chunk_size> centered{};
    double absolute_sum = 0.0;
    double squared_sum = 0.0;
    for (std::size_t offset = 0; offset < count; offset += chunk_size) {
        const std::size_t chunk = std::min(chunk_size, count - offset);
        for (std::size_t i = 0; i < chunk; ++i) centered[i] = residuals[offset + i] - mean;
        float chunk_absolute = 0.0f;
        float chunk_squared = 0.0f;
        simd::accumulate_moments(centered.data(), chunk, chunk_absolute, chunk_squared);
        absolute_sum += chunk_absolute;
        squared_sum += chunk_squared;
    }

    const double sample_count = static_cast<double>(count);
    const double first_absolute_moment = absolute_sum / sample_count;
    const double variance = squared_sum / sample_count;
    if (variance <= std::numeric_limits<float>::min()) return {mean, 0.0f, 2.0f};

    const double ratio =
        std::clamp(first_absolute_moment * first_absolute_moment / variance, 0.0, 1.0);
    const float beta = shape_from_ratio(ratio);
    const double alpha =
        std::sqrt(variance * std::exp(std::lgamma(1.0 / beta) - std::lgamma(3.0 / beta)));
    if (!std::isfinite(alpha)) throw StreamError("GED parameter estimation overflow");
    return {mean, static_cast<float>(alpha), beta};
}

}  // namespace aether
