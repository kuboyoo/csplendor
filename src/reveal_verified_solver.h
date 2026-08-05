#ifndef CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
#define CSPLENDOR_REVEAL_VERIFIED_SOLVER_H

#include "game.h"
#include "solver_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class RevealVerifiedSolver {
public:
  RevealVerifiedSolver(int attacker, int depth, uint64_t max_nodes,
                       double time_limit_seconds,
                       std::vector<uint64_t> preferred_attacker_actions = {},
                       bool include_proof_dag = false,
                       size_t proof_dag_node_limit = 100000,
                       size_t proof_dag_edge_limit = 500000,
                       uint64_t required_root_action = UINT64_MAX,
                       bool strict_preferred_attacker_actions = false,
                       size_t strict_preferred_attacker_prefix = 0);
  ~RevealVerifiedSolver();

  RevealVerifiedSolver(const RevealVerifiedSolver &other);
  RevealVerifiedSolver &operator=(const RevealVerifiedSolver &other);
  RevealVerifiedSolver(RevealVerifiedSolver &&other) noexcept;
  RevealVerifiedSolver &operator=(RevealVerifiedSolver &&other) noexcept;

  RevealVerifiedSearchResult solve(const Game &input);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

#endif // CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
