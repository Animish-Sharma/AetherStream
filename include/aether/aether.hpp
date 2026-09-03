#pragma once

#include "common.hpp"
#include "rate_controller.hpp"

namespace aether {

struct CodecConfig {
    float target_rate = 4.0f;
    float absolute_error_bound = 0.0f;
    float deadzone_factor = 0.3f;
    bool enable_index = false;
};

class AetherCodec {
   public:
    explicit AetherCodec(float target_rate = 4.0f, float deadzone_factor = 0.3f,
                         float absolute_error_bound = 0.0f, bool enable_index = false);
    explicit AetherCodec(const CodecConfig& config)
        : AetherCodec(config.target_rate, config.deadzone_factor, config.absolute_error_bound,
                      config.enable_index) {}

    std::vector<uint8_t> compress(std::span<const float> input) const;
    void decompress(std::span<const uint8_t> input, std::span<float> output) const;

    float target_rate() const { return target_rate_; }

    float deadzone_factor() const { return deadzone_factor_; }

    float absolute_error_bound() const { return absolute_error_bound_; }

    bool index_enabled() const { return enable_index_; }

   private:
    float target_rate_;
    float deadzone_factor_;
    float absolute_error_bound_;
    bool enable_index_;
};

}  // namespace aether
