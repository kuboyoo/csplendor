#ifndef CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
#define CSPLENDOR_REVEAL_VERIFIED_SOLVER_H

#include "game.h"
#include "solver_types.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct RevealVerifiedFrontierEdge {
  uint64_t action_code = 0;
  int reveal_card = -1;
  int child_depth = 0;
  Game child{0};
};

struct RevealVerifiedFrontierResult {
  bool proven = false;
  bool complete = false;
  int attacker = -1;
  int depth = -1;
  int player = -1;
  int winner = -1;
  bool waiting_noble = false;
  std::string kind = "state";
  std::string resolution;
  std::string reason = "unknown";
  std::string unknown_reason;
  size_t memoized_states = 0;
  RevealVerifiedSearchStats stats;
  std::vector<RevealVerifiedFrontierEdge> edges;
};

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
                       size_t strict_preferred_attacker_prefix = 0,
                       bool exhaustive_attacker_actions = false,
                       bool exact_reveal_search = false,
                       std::shared_ptr<
                           csplendor::solver_internal::RevealSearchCancellationToken>
                           cancellation_token = {});
  ~RevealVerifiedSolver();

  RevealVerifiedSolver(const RevealVerifiedSolver &other);
  RevealVerifiedSolver &operator=(const RevealVerifiedSolver &other);
  RevealVerifiedSolver(RevealVerifiedSolver &&other) noexcept;
  RevealVerifiedSolver &operator=(RevealVerifiedSolver &&other) noexcept;

  RevealVerifiedSearchResult solve(const Game &input);
  RevealVerifiedSearchResult solve_reusing_exact_cache(
      const Game &input, int depth, uint64_t max_nodes,
      double time_limit_seconds,
      std::vector<uint64_t> preferred_attacker_actions = {},
      std::shared_ptr<
          csplendor::solver_internal::RevealSearchCancellationToken>
          cancellation_token = {},
      size_t max_cache_states = 0);
  void clear_exact_cache();
  void trim_exact_cache(size_t max_cache_states);
  size_t exact_cache_size() const noexcept;
  RevealVerifiedFrontierResult split_root(const Game &input,
                                          size_t edge_limit = 250000);
  RevealVerifiedFrontierResult expand_frontier(const Game &input,
                                               size_t edge_limit = 250000);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

#endif // CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
