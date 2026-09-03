#include <cassert>
#include <cmath>
#include <vector>

#include "aether/predictor.hpp"

static double variance(const std::vector<float>& values) {
    double mean = 0.0;
    for (float v : values) mean += v;
    mean /= values.size();
    double var = 0.0;
    for (float v : values) {
        const double d = v - mean;
        var += d * d;
    }
    return var / values.size();
}

int main() {
    std::vector<float> constant(64, 7.0f);
    float parameter = 1.0f;
    assert(aether::select_optimal_predictor(constant.data(), constant.size(), parameter) ==
           aether::PredictorMode::CONSTANT);

    std::vector<float> harmonic(2048);
    for (std::size_t i = 0; i < harmonic.size(); ++i) {
        harmonic[i] = std::sin(0.31f * static_cast<float>(i));
    }
    const aether::PredictorMode harmonic_mode =
        aether::select_optimal_predictor(harmonic.data(), harmonic.size(), parameter);
    assert(harmonic_mode == aether::PredictorMode::HARMONIC);

    std::vector<float> harmonic_residuals(harmonic.size());
    std::vector<float> linear_residuals(harmonic.size());
    aether::Predictor::residuals(harmonic.data(), harmonic_residuals.data(), harmonic.size(),
                                 harmonic_mode, parameter);
    aether::Predictor::residuals(harmonic.data(), linear_residuals.data(), harmonic.size(),
                                 aether::PredictorMode::LINEAR, 0.0f);
    assert(variance(harmonic_residuals) <= 0.70 * variance(linear_residuals));

    std::vector<float> reconstructed(harmonic.size());
    aether::Predictor::reconstruct(harmonic_residuals.data(), reconstructed.data(), harmonic.size(),
                                   harmonic_mode, parameter);
    for (std::size_t i = 0; i < harmonic.size(); ++i) {
        assert(std::abs(harmonic[i] - reconstructed[i]) < 1e-5f);
    }

    static constexpr char text[] = "123456789";
    const auto* bytes = reinterpret_cast<const uint8_t*>(text);
    assert(aether::crc32c({bytes, 9}) == 0xe3069283U);
}
