#pragma once

#include "common.hpp"

namespace aether {

class GedEstimator {
   public:
    GedParameters estimate(const float* residuals, std::size_t count) const;
};

}  // namespace aether
