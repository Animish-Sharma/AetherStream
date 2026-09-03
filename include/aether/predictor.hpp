#pragma once

#include "common.hpp"

namespace aether {

PredictorMode select_optimal_predictor(const float* block, std::size_t count,
                                       float& harmonic_param);

float predict_sample(PredictorMode mode, float param, std::size_t index, float previous,
                     float previous_two);

class Predictor {
   public:
    static PredictorMode select(const float* block, std::size_t count, float& harmonic_param) {
        return select_optimal_predictor(block, count, harmonic_param);
    }

    static float predict(PredictorMode mode, float param, std::size_t index, float previous,
                         float previous_two) {
        return predict_sample(mode, param, index, previous, previous_two);
    }

    static void residuals(const float* samples, float* residuals, std::size_t count,
                          PredictorMode mode, float param = 0.0f);
    static void reconstruct(const float* residuals, float* samples, std::size_t count,
                            PredictorMode mode, float param = 0.0f);
};

}  // namespace aether
