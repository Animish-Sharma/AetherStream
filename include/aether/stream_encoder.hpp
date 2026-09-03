#pragma once

#include "aether/aether.hpp"

namespace aether {

class StreamEncoder {
   public:
    explicit StreamEncoder(float target_rate = 4.0f, float deadzone_factor = 0.3f,
                           float absolute_error_bound = 0.0f);

    std::vector<uint8_t> feed(std::span<const float> chunk);
    std::vector<uint8_t> flush();

   private:
    AetherCodec codec_;
    std::vector<float> buffer_;
};

class StreamDecoder {
   public:
    // Accepts arbitrary byte fragmentation and may decode multiple complete
    // frames in one call. Incomplete trailing bytes are retained internally.
    std::vector<float> feed(std::span<const uint8_t> chunk);
    void flush();

   private:
    std::vector<uint8_t> buffer_;
};

}  // namespace aether
