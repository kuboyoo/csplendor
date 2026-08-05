#ifndef CSPLENDOR_MCTS_CONFIG_VALIDATOR_H
#define CSPLENDOR_MCTS_CONFIG_VALIDATOR_H

#include "mcts_types.h"

#include <cmath>
#include <stdexcept>

namespace mcts_internal {

// Validation is deliberately separate from MCTSConfig, which remains a
// passive public value type for source and binding compatibility.  Callers
// choose the execution profile whose constraints they need to enforce.
class MCTSConfigValidator final {
public:
  MCTSConfigValidator() = delete;

  static void validate_parallel(const MCTSConfig &config) {
    if (!std::isfinite(config.cpuct) || config.cpuct < 0.0f ||
        !std::isfinite(config.fpu) ||
        !std::isfinite(config.forced_playouts_k) ||
        config.forced_playouts_k < 0.0f || config.num_simulations < 0 ||
        !std::isfinite(config.dirichlet_epsilon) ||
        config.dirichlet_epsilon < 0.0f || config.dirichlet_epsilon > 1.0f)
      throw std::invalid_argument("parallel MCTS config is invalid");
    if (config.use_dirichlet_noise && (!std::isfinite(config.dirichlet_alpha) ||
                                       config.dirichlet_alpha <= 0.0f))
      throw std::invalid_argument("Dirichlet alpha must be positive");
    if (config.use_determinization && config.num_determinizations != 1)
      throw std::invalid_argument(
          "parallel hidden-information search requires one world");
  }
};

} // namespace mcts_internal

#endif // CSPLENDOR_MCTS_CONFIG_VALIDATOR_H
