#pragma once

#include "common.hpp"
#include "rate_controller.hpp"

namespace aether {

struct CodecConfig {
    float target_rate = 4.0f;
    float absolute_error_bound = 0.0f;
    float deadzone_factor = 0.3f;
    bool enable_index = false;
    bool enable_adaptive_tail = true;
};

class AetherCodec {
   public:
    explicit AetherCodec(float target_rate = 4.0f, float deadzone_factor = 0.3f,
                         float absolute_error_bound = 0.0f, bool enable_index = false,
                         bool enable_adaptive_tail = true);
    explicit AetherCodec(const CodecConfig& config)
        : AetherCodec(config.target_rate, config.deadzone_factor, config.absolute_error_bound,
                      config.enable_index, config.enable_adaptive_tail) {}

    std::vector<uint8_t> compress(std::span<const float> input) const;
    void decompress(std::span<const uint8_t> input, std::span<float> output) const;

    float target_rate() const { return target_rate_; }

    float deadzone_factor() const { return deadzone_factor_; }

    float absolute_error_bound() const { return absolute_error_bound_; }

    bool index_enabled() const { return enable_index_; }

    bool adaptive_tail_enabled() const { return enable_adaptive_tail_; }

   private:
    float target_rate_;
    float deadzone_factor_;
    float absolute_error_bound_;
    bool enable_index_;
    bool enable_adaptive_tail_;
};

}  // namespace aether
