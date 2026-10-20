#ifndef CSPLENDOR_NEAR_MATE_PROBE_H
#define CSPLENDOR_NEAR_MATE_PROBE_H

// Probabilistic ("near") mate search: brackets the probability that the
// attacker forces a win, per depth, with threshold queries to
// RevealVerifiedSolver::near_mate (doc/mate_usage.md, 準詰み). Single-threaded
// and free of Python so the Python wrapper (csplendor.search_near_mate) and the
// WASM build share one result.

#include "action_encoder_v5.h"
#include "game.h"
#include "reveal_verified_solver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace csplendor {

struct NearMateSearchConfig {
  int attacker = 0;
  int min_depth = 1;
  int max_depth = 3;
  // Tried from the highest down; duplicates are dropped.
  std::vector<double> thresholds = {1.0, 0.9, 0.75, 0.5};
  uint64_t max_nodes = 0;          // cumulative over all queries, 0 = unlimited
  double time_limit_seconds = 0.0; // cumulative over all queries, 0 = unlimited
  bool no_take_probe = false;
  bool line_transplant = true;
  size_t max_memo_states = 0;
};

struct NearMateDepthBound {
  int depth = 0;
  double lower = 0.0;
  double upper = 1.0;
  bool decided = true; // every threshold above the proven one was decided
};

struct NearMateAttempt {
  int depth = 0;
  double threshold = 0.0;
  std::string verdict; // "proven", "refuted" or "undecided"
  uint64_t nodes = 0;
  double elapsed_ms = 0.0;
  std::string unknown_reason; // empty when the query completed
};

struct NearMateSearchResult {
  std::vector<double> thresholds; // the ladder actually used
  double lower = 0.0;
  double upper = 1.0;
  int lower_depth = -1;
  int upper_depth = -1;
  std::vector<NearMateDepthBound> bounds;
  bool has_best_action = false;
  uint64_t best_action = 0; // Action::pack() of the proven query's best move
  int best_action_id = -1;  // its ActionEncoderV5 id (attacker to move only)
  std::vector<NearMateRootAction> root_actions;
  std::vector<NearMateReveal> best_action_reveals;
  bool undecided = false;
  std::string stop_reason;
  std::vector<NearMateAttempt> attempts;
  uint64_t nodes = 0;
  double elapsed_ms = 0.0;
};

// `order_hints` and `cancellation_token` are optional (the Python wrapper
// forwards its own); the WASM build passes neither.
inline NearMateSearchResult near_mate_search(
    const Game &game, const NearMateSearchConfig &config,
    std::shared_ptr<const PositionOrderHints> order_hints = {},
    std::shared_ptr<csplendor::solver_internal::RevealSearchCancellationToken>
        cancellation_token = {}) {
  using Clock = std::chrono::steady_clock;
  if (config.attacker < 0 || config.attacker >= Board::NUM_PLAYERS)
    throw std::invalid_argument("attacker must be 0 or 1");
  if (config.min_depth < 0 || config.max_depth < config.min_depth)
    throw std::invalid_argument("depth range must satisfy 0 <= min_depth <= max_depth");
  std::vector<double> ladder = config.thresholds;
  std::sort(ladder.begin(), ladder.end(), std::greater<double>());
  ladder.erase(std::unique(ladder.begin(), ladder.end()), ladder.end());
  if (ladder.empty() || ladder.front() > 1.0 || ladder.back() <= 0.0)
    throw std::invalid_argument("thresholds must lie in (0, 1]");

  const auto started = Clock::now();
  const auto seconds_since_start = [&] {
    return std::chrono::duration<double>(Clock::now() - started).count();
  };
  NearMateSearchResult result;
  result.thresholds = ladder;
  result.stop_reason = "max_depth_reached";

  // A restricted (no_take_probe) query that misses a threshold has not seen
  // every attacker move, so it refutes nothing: the threshold stays open.
  bool restricted_inconclusive = false;
  for (int depth = config.min_depth; depth <= config.max_depth; ++depth) {
    double depth_upper = 1.0;
    bool depth_decided = true;
    bool depth_cut = false;
    for (const double threshold : ladder) {
      if (threshold <= result.lower)
        break; // already proven at a shallower depth
      const uint64_t remaining_nodes =
          config.max_nodes == 0 || result.nodes >= config.max_nodes
              ? 0
              : config.max_nodes - result.nodes;
      const double remaining_time =
          config.time_limit_seconds > 0.0
              ? config.time_limit_seconds - seconds_since_start()
              : 0.0;
      if ((config.max_nodes != 0 && remaining_nodes == 0) ||
          (config.time_limit_seconds > 0.0 && remaining_time <= 0.0)) {
        result.stop_reason = "cumulative limit exceeded";
        depth_decided = false;
        depth_cut = true;
        break;
      }
      RevealVerifiedSolver solver(config.attacker, depth, remaining_nodes,
                                  remaining_time, {}, false, 0, 0, UINT64_MAX,
                                  false, 0, true, true, cancellation_token);
      solver.set_line_transplant(config.line_transplant);
      solver.set_no_take_probe(config.no_take_probe);
      solver.set_max_memo_states(config.max_memo_states);
      if (order_hints)
        solver.set_order_hints(order_hints, 0);
      const NearMateResult raw =
          solver.near_mate(game, false, std::max(0.0, threshold - 1e-6), threshold);
      result.nodes += raw.stats.nodes;
      const bool proven = raw.complete && raw.value >= threshold;
      const bool restricted_miss = raw.complete && !proven && config.no_take_probe;
      result.attempts.push_back(NearMateAttempt{
          depth, threshold,
          proven            ? "proven"
          : restricted_miss ? "inconclusive"
          : raw.complete    ? "refuted"
                            : "undecided",
          raw.stats.nodes, raw.stats.elapsed_ms, raw.unknown_reason});
      if (proven) {
        result.lower = threshold;
        result.lower_depth = depth;
        result.has_best_action = raw.has_best_action;
        result.best_action = raw.best_action;
        result.best_action_reveals = raw.best_action_reveals;
        result.root_actions = raw.root_actions;
        break;
      }
      if (restricted_miss) {
        restricted_inconclusive = true;
        depth_decided = false;
      } else if (raw.complete) {
        depth_upper = std::min(depth_upper, threshold);
      } else {
        result.undecided = true;
        depth_decided = false;
        depth_cut = true;
        result.stop_reason =
            raw.unknown_reason.empty() ? "search limit exceeded" : raw.unknown_reason;
        break;
      }
    }
    result.bounds.push_back(NearMateDepthBound{depth, result.lower, depth_upper, depth_decided});
    if (result.lower >= ladder.front()) {
      result.stop_reason = "mate_proven";
      break;
    }
    if (depth_cut)
      break;
  }
  if (restricted_inconclusive && result.stop_reason == "max_depth_reached")
    result.stop_reason = "restricted_probe_exhausted";

  // The upper bound belongs to the deepest depth whose ladder was decided (a
  // deeper search can only raise the probability).
  for (const NearMateDepthBound &entry : result.bounds) {
    if (entry.decided || entry.upper < 1.0) {
      result.upper = entry.decided ? entry.upper : std::max(entry.upper, result.lower);
      result.upper_depth = entry.depth;
    }
  }
  if (result.lower >= 1.0) {
    result.upper = 1.0;
    result.upper_depth = result.lower_depth;
  } else if (result.upper <= result.lower) {
    result.upper = result.lower;
  }

  if (result.has_best_action && game.current_player() == config.attacker) {
    for (const Action &action : game.legal_actions()) {
      if (action.pack() == result.best_action) {
        result.best_action_id = ActionEncoderV5::encode(action, game);
        break;
      }
    }
  }
  result.elapsed_ms = seconds_since_start() * 1000.0;
  return result;
}

} // namespace csplendor

#endif // CSPLENDOR_NEAR_MATE_PROBE_H
