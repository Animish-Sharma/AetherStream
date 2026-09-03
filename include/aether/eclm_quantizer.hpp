#pragma once

#include "common.hpp"
#include "simd_dispatch.hpp"

namespace aether {

class ECLMQuantizer {
   public:
    explicit ECLMQuantizer(float target_rate = 4.0f, float deadzone_factor = 0.3f);
    ECLMQuantizer(std::size_t levels, float target_rate, float deadzone_factor = 0.3f);

    void design(const GedParameters& ged, unsigned iterations = 60, float initial_lambda = 0.0f);
    void fit_samples(const float* samples, std::size_t count, unsigned iterations = 20);

    uint8_t quantize(float value) const;
    void quantize(const float* values, uint8_t* output, std::size_t count) const;

    float reconstruct(uint8_t index) const { return codebook_.at(index); }

    const std::vector<float>& levels() const { return codebook_; }

    const std::vector<float>& probabilities() const { return codebook_probabilities_; }

    float entropy() const { return entropy_bits(probabilities_); }

    float deadzone_threshold() const { return deadzone_threshold_; }

    std::size_t center_index() const { return 0; }

   private:
    uint8_t code_for_cell(std::size_t cell) const;
    std::size_t cell_for_code(uint8_t code) const;
    void rebuild_codebook();
    void rebuild_polynomial();
    void update_cells();

    std::size_t level_count_;
    std::size_t center_index_;
    float target_rate_;
    float deadzone_factor_;
    float deadzone_threshold_ = 0.0f;
    float lambda_ = 0.0f;
    GedParameters ged_{};
    // levels_ remains monotonic in cell order for Lloyd-Max updates;
    // codebook_ remaps the dead-zone reconstruction to wire symbol zero.
    std::vector<float> levels_;
    std::vector<float> codebook_;
    std::vector<float> boundaries_;
    std::vector<float> probabilities_;
    std::vector<float> codebook_probabilities_;
    simd::PiecewisePolynomialQuantizer polynomial_{};
};

}  // namespace aether
