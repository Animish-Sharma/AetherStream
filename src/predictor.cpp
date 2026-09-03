#include "aether/predictor.hpp"

#include <algorithm>

namespace aether {

PredictorMode select_optimal_predictor(const float* block, std::size_t count,
                                       float& harmonic_param) {
    harmonic_param = 0.0f;
    if (block == nullptr && count != 0) throw std::invalid_argument("null predictor input");
    if (count == 0) return PredictorMode::CONSTANT;

    const std::size_t window = std::min<std::size_t>(count, 64);
    double sum = 0.0;
    for (std::size_t i = 0; i < window; ++i) {
        sum += block[i];
    }
    const double mean = sum / window;

    double energy = 0.0;
    double lag_one = 0.0;
    double lag_two = 0.0;
    for (std::size_t i = 0; i < window; ++i) {
        const double centered = block[i] - mean;
        energy += centered * centered;
        if (i >= 1) {
            lag_one += centered * (block[i - 1] - mean);
        }
        if (i >= 2) {
            lag_two += centered * (block[i - 2] - mean);
        }
    }

    const double variance = energy / window;
    if (variance < 1e-9 || energy == 0.0) {
        return PredictorMode::CONSTANT;
    }

    const double r1 = lag_one / energy;
    const double r2 = lag_two / energy;
    if (std::abs(r1) < 0.25) {
        return PredictorMode::CONSTANT;
    }

    if (2.0 * (1.0 - r1 * r1) > 1e-4 && r2 < 0.95 * r1) {
        const double pole = r1 * (1.0 + r2) / (2.0 * r2 + 1e-5);
        harmonic_param = static_cast<float>(std::clamp(pole, -0.999, 0.999));

        // The finite-window autocorrelation expression can saturate at its
        // stability guard for clean tones. Refine saturated estimates with
        // the least-squares spectral recurrence x[t]+x[t-2]=2c*x[t-1].
        // This retains autocorrelation-driven mode selection while avoiding
        // a systematic frequency bias near c=1.
        if (std::abs(harmonic_param) >= 0.9989f && window >= 3) {
            double numerator = 0.0;
            double denominator = 0.0;
            for (std::size_t i = 2; i < window; ++i) {
                const double middle = block[i - 1];
                numerator += middle * (block[i] + block[i - 2]);
                denominator += 2.0 * middle * middle;
            }
            if (denominator > 1e-20) {
                harmonic_param =
                    static_cast<float>(std::clamp(numerator / denominator, -0.999, 0.999));
            }
        }
        return PredictorMode::HARMONIC;
    }

    return PredictorMode::LINEAR;
}

float predict_sample(PredictorMode mode, float param, std::size_t index, float previous,
                     float previous_two) {
    if (index == 0) {
        return 0.0f;
    }
    if (index == 1) {
        return previous;
    }

    switch (mode) {
        case PredictorMode::CONSTANT:
            return previous;
        case PredictorMode::HARMONIC:
            return 2.0f * param * previous - previous_two;
        case PredictorMode::LINEAR:
            return 2.0f * previous - previous_two;
        case PredictorMode::ADAPTIVE_AR:
            return 2.0f * previous - previous_two;
    }
    return previous;
}

void Predictor::residuals(const float* samples, float* residuals, std::size_t count,
                          PredictorMode mode, float param) {
    if (count != 0 && (samples == nullptr || residuals == nullptr))
        throw std::invalid_argument("null predictor residual array");
    float previous_two = 0.0f;
    float previous = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float prediction = predict_sample(mode, param, i, previous, previous_two);
        residuals[i] = samples[i] - prediction;
        previous_two = previous;
        previous = samples[i];
    }
}

void Predictor::reconstruct(const float* residuals, float* samples, std::size_t count,
                            PredictorMode mode, float param) {
    if (count != 0 && (samples == nullptr || residuals == nullptr))
        throw std::invalid_argument("null predictor reconstruction array");
    float previous_two = 0.0f;
    float previous = 0.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const float prediction = predict_sample(mode, param, i, previous, previous_two);
        const float current = prediction + residuals[i];
        samples[i] = current;
        previous_two = previous;
        previous = current;
    }
}

}  // namespace aether
