#pragma once

#include <limits>

#include "common.hpp"

namespace aether {

class RateBudgetExceeded final : public StreamError {
   public:
    explicit RateBudgetExceeded(const std::string& message) : StreamError(message) {}
};

struct RateDecision {
    std::size_t budget_bytes = 0;
    float target_entropy = 0.0f;
    float next_lambda = 0.0f;
};

class RateController {
   public:
    RateController(float bits_per_sample, std::size_t sample_count, std::size_t metadata_bytes,
                   std::size_t table_bytes,
                   std::size_t available_wire_bytes = std::numeric_limits<std::size_t>::max()) {
        if (!std::isfinite(bits_per_sample) || bits_per_sample <= 0.0f)
            throw std::invalid_argument("wire-rate budget must be finite and positive");
        if (sample_count == 0) throw std::invalid_argument("wire-rate controller requires samples");
        const long double bits = static_cast<long double>(sample_count) * bits_per_sample;
        if (bits / 8.0L > std::numeric_limits<std::size_t>::max())
            throw std::overflow_error("wire-rate budget overflow");
        const std::size_t nominal_budget = static_cast<std::size_t>(std::floor(bits / 8.0L));
        budget_bytes_ = available_wire_bytes == std::numeric_limits<std::size_t>::max()
                            ? nominal_budget
                            : available_wire_bytes;
        const std::size_t overhead = metadata_bytes + table_bytes;
        const double overhead_rate =
            8.0 * static_cast<double>(overhead) / static_cast<double>(sample_count);
        target_entropy_ =
            static_cast<float>(std::max(0.0, static_cast<double>(bits_per_sample) - overhead_rate));
    }

    std::size_t budget_bytes() const noexcept { return budget_bytes_; }
    float target_entropy() const noexcept { return target_entropy_; }

    bool accepts(std::size_t actual_wire_bytes) const noexcept {
        return actual_wire_bytes <= budget_bytes_;
    }

    float secant_step(float lambda, std::size_t actual_wire_bytes,
                      float scale_hint) const noexcept {
        if (accepts(actual_wire_bytes)) return lambda;
        const double denominator = static_cast<double>(std::max<std::size_t>(budget_bytes_, 1));
        const double excess =
            (static_cast<double>(actual_wire_bytes) - static_cast<double>(budget_bytes_)) /
            denominator;
        const float seed = std::max(lambda, std::max(scale_hint * scale_hint, 1e-12f) * 1e-4f);
        return static_cast<float>(seed * (1.0 + excess));
    }

    void enforce(std::size_t actual_wire_bytes) const {
        if (!accepts(actual_wire_bytes))
            throw RateBudgetExceeded("requested per-block wire-rate budget is infeasible");
    }

   private:
    std::size_t budget_bytes_ = 0;
    float target_entropy_ = 0.0f;
};

}  // namespace aether
