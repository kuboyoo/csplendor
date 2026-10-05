#ifndef CSPLENDOR_ROOT_MATE_PROBE_H
#define CSPLENDOR_ROOT_MATE_PROBE_H

// Proven-mate probe of an AI's root position before its tree search
// (doc/mate_usage.md, "root の詰みの探り"). Single-threaded and free of
// Python so that the Python module and the WASM build share one result.
//
// The steps mirror the Python driver it replaces: an anytime sweep over
// [min_depth, max_depth] (MateSearchSession.search_anytime with jobs=1 and a
// fresh session), then, in the endgame, an exact sweep over
// [1, endgame_exact_depth] (search_reveal_verified_mate_depths). Unlike the
// Python driver, both sweeps share one deadline, so a call never exceeds its
// time limit.

#include "action_encoder_v4.h"
#include "card_data.h"
#include "game.h"
#include "noble_data.h"
#include "reveal_verified_solver.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace csplendor {

struct RootMateProbeConfig {
  bool enabled = true;
  int min_points = 9;
  // Probe below min_points when the attacker's previous root value reached
  // this; nullopt probes by points only.
  std::optional<double> trigger_value = 0.6;
  int endgame_points = 10;
  uint64_t max_nodes = 20000;
  double time_limit_ms = 20.0; // 0 = unlimited
  uint64_t endgame_max_nodes = 2000000;
  double endgame_time_limit_ms = 150.0; // 0 = unlimited
  int min_depth = 1;
  int max_depth = 3;
  int endgame_exact_depth = 2;
  size_t max_cache_states = 50000;
  uint64_t warm_start_nodes = 50000;
  double warm_start_time_ms = 50.0;
};

struct RootMateProbeResult {
  bool attempted = false;
  bool proven = false;       // mate with an identified root action
  bool value_proven = false; // mate, with or without a root action
  int depth = -1;
  int64_t action_code = -1; // Action::pack() of the mating root action
  int action_id = -1;       // ActionEncoderV4 id of that action
  uint64_t nodes = 0;
  double elapsed_ms = 0.0;
  std::string stop_reason; // empty when no search was attempted
  bool value_triggered = false;
};

namespace root_mate_probe_detail {

using Clock = std::chrono::steady_clock;
constexpr int WIN_POINTS = 15;

enum class Outcome { Mate, NoMate, Unknown };

inline Outcome outcome_of(const RevealVerifiedSearchResult &result) {
  if (result.proven)
    return Outcome::Mate;
  return result.unknown_reason.empty() ? Outcome::NoMate : Outcome::Unknown;
}

// A cheap, sound reason why the attacker can never win (empty if none).
inline std::string permanent_no_mate(const Game &game, int attacker) {
  const Board &board = game.board;
  if (game.is_game_over())
    return game.winner() == attacker ? "" : "terminal_non_attacker_result";
  int ceiling = board.players[attacker].points;
  for (const auto &level : board.visible)
    for (int8_t card : level)
      if (card >= 0)
        ceiling += get_card(card).points;
  for (const auto &deck : board.decks)
    for (uint8_t card : deck)
      ceiling += get_card(card).points;
  // An opponent's reservation never returns to the market.
  for (int8_t card : board.players[attacker].reserved)
    if (card >= 0)
      ceiling += get_card(card).points;
  for (uint8_t noble : board.nobles)
    ceiling += get_noble(noble).points;
  return ceiling >= WIN_POINTS ? "" : "attacker_score_ceiling_below_win_threshold";
}

class Budget {
public:
  Budget(uint64_t max_nodes, double time_limit_seconds)
      : max_nodes_(max_nodes), timed_(time_limit_seconds > 0.0),
        deadline_(Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(time_limit_seconds))) {}

  bool node_limited() const { return max_nodes_ != 0; }
  bool timed() const { return timed_; }
  bool nodes_exhausted() const { return node_limited() && used_ >= max_nodes_; }
  // 0 means unlimited for an unlimited budget, as for the solver.
  uint64_t nodes_left() const {
    return node_limited() && !nodes_exhausted() ? max_nodes_ - used_ : 0;
  }
  double seconds_left() const {
    if (!timed_)
      return 0.0;
    return std::max(0.0, std::chrono::duration<double>(deadline_ - Clock::now()).count());
  }
  bool time_exhausted() const { return timed_ && seconds_left() <= 0.0; }
  void spend(uint64_t nodes) { used_ += nodes; }
  uint64_t used() const { return used_; }

private:
  uint64_t max_nodes_;
  uint64_t used_ = 0;
  bool timed_;
  Clock::time_point deadline_;
};

struct SweepResult {
  bool mate = false;
  int depth = -1;
  bool has_root_action = false;
  uint64_t root_action = 0;
  std::string stop_reason;
};

inline void take_mate(SweepResult &sweep, int depth, const RevealVerifiedSearchResult &raw) {
  sweep.mate = true;
  sweep.depth = depth;
  if (!raw.line.empty()) {
    sweep.has_root_action = true;
    sweep.root_action = raw.line.front().action_code;
  }
}

// MateSearchSession.search_anytime (jobs=1, fresh session). Each depth gets
// its share of the remaining budget; a quarter of it warms the session's
// exact table first, and a still-open depth falls back to the heuristic
// positive-proof search.
inline SweepResult anytime_sweep(const Game &game, int attacker,
                                 const RootMateProbeConfig &config, Budget &budget) {
  SweepResult sweep;
  sweep.stop_reason = "max_depth_reached_without_proof";
  RevealVerifiedSolver session(attacker, 0, 0, 0.0, {}, false, 0, 0, UINT64_MAX,
                               false, 0, true, true);
  const double warm_start_seconds = config.warm_start_time_ms / 1000.0;
  for (int depth = config.min_depth; depth <= config.max_depth; ++depth) {
    const int attempts_left = config.max_depth - depth + 1;
    if (budget.nodes_exhausted() || budget.time_exhausted()) {
      sweep.stop_reason = "cumulative search limit exceeded";
      break;
    }
    const uint64_t depth_nodes =
        budget.node_limited() ? std::max<uint64_t>(1, budget.nodes_left() / attempts_left) : 0;
    const double depth_seconds = budget.timed() ? budget.seconds_left() / attempts_left : 0.0;
    uint64_t warm_nodes = config.warm_start_nodes;
    if (budget.node_limited())
      warm_nodes = std::min<uint64_t>(std::max<uint64_t>(1, warm_nodes),
                                      std::max<uint64_t>(1, depth_nodes / 4));
    double warm_seconds = warm_start_seconds;
    if (budget.timed())
      warm_seconds = std::min(warm_seconds, depth_seconds / 4.0);
    if (warm_nodes == 0 && warm_seconds == 0.0)
      warm_nodes = 1;

    const RevealVerifiedSearchResult warm = session.solve_reusing_exact_cache(
        game, depth, warm_nodes, warm_seconds, {}, {}, config.max_cache_states);
    budget.spend(warm.stats.nodes);
    const Outcome warm_outcome = outcome_of(warm);
    if (warm_outcome == Outcome::Mate) {
      take_mate(sweep, depth, warm);
      sweep.stop_reason = "mate_proven";
      break;
    }
    if (warm_outcome == Outcome::NoMate)
      continue;

    if (budget.nodes_exhausted() || budget.time_exhausted()) {
      sweep.stop_reason = "cumulative search limit exceeded";
      break;
    }
    // search_reveal_verified_mate_anytime over this single depth.
    const std::string certificate = permanent_no_mate(game, attacker);
    if (!certificate.empty()) {
      sweep.stop_reason = certificate;
      break;
    }
    const uint64_t attempt_nodes =
        budget.node_limited() ? std::max<uint64_t>(1, budget.nodes_left() / attempts_left) : 0;
    const double attempt_seconds = budget.timed() ? budget.seconds_left() / attempts_left : 0.0;
    RevealVerifiedSolver solver(attacker, depth, attempt_nodes, attempt_seconds);
    const RevealVerifiedSearchResult raw = solver.solve(game);
    budget.spend(raw.stats.nodes);
    if (outcome_of(raw) == Outcome::Mate) {
      take_mate(sweep, depth, raw);
      sweep.stop_reason = "mate_proven";
      break;
    }
  }
  return sweep;
}

// search_reveal_verified_mate_depths (jobs=1): every attacker move and every
// concrete reveal; a refuted depth advances, anything else stops.
inline SweepResult exact_sweep(const Game &game, int attacker, int max_depth,
                               uint64_t max_memo_states, Budget &budget) {
  SweepResult sweep;
  if (!permanent_no_mate(game, attacker).empty())
    return sweep;
  for (int depth = 1; depth <= max_depth; ++depth) {
    if (budget.nodes_exhausted() || budget.time_exhausted())
      break;
    RevealVerifiedSolver solver(attacker, depth, budget.nodes_left(), budget.seconds_left(),
                                {}, false, 100000, 500000, UINT64_MAX, false, 0, true, true);
    solver.set_max_memo_states(max_memo_states);
    const RevealVerifiedSearchResult raw = solver.solve(game);
    budget.spend(raw.stats.nodes);
    const Outcome outcome = outcome_of(raw);
    if (outcome == Outcome::Mate) {
      take_mate(sweep, depth, raw);
      break;
    }
    if (outcome == Outcome::Unknown)
      break;
  }
  return sweep;
}

inline bool opponent_holds_hidden_reserve(const Game &game, int attacker) {
  const PlayerState &opponent = game.board.players[1 - attacker];
  for (size_t slot = 0; slot < opponent.reserved.size(); ++slot)
    if (opponent.reserved[slot] >= 0 && opponent.reserved_is_hidden[slot])
      return true;
  return false;
}

} // namespace root_mate_probe_detail

// `time_limit_seconds` overrides the selected budget's time limit (0 =
// unlimited); it bounds the whole call, both sweeps included.
inline RootMateProbeResult root_mate_probe(const Game &game, const RootMateProbeConfig &config,
                                           std::optional<double> previous_value = std::nullopt,
                                           std::optional<double> time_limit_seconds = std::nullopt) {
  using namespace root_mate_probe_detail;
  const auto started = Clock::now();
  RootMateProbeResult result;
  if (!config.enabled)
    return result;
  const int attacker = game.current_player();
  const int points = game.board.players[attacker].points;
  result.value_triggered = points < config.min_points && config.trigger_value &&
                           previous_value && *previous_value >= *config.trigger_value;
  if (points < config.min_points && !result.value_triggered)
    return result;
  if (opponent_holds_hidden_reserve(game, attacker)) {
    result.stop_reason = "opponent_hidden_reserve";
    return result;
  }
  const bool endgame =
      result.value_triggered || (config.endgame_points > 0 && points >= config.endgame_points);
  uint64_t max_nodes = config.max_nodes;
  double time_limit_ms = config.time_limit_ms;
  if (endgame) {
    if (config.endgame_max_nodes > 0)
      max_nodes = config.endgame_max_nodes;
    if (config.endgame_time_limit_ms > 0.0)
      time_limit_ms = config.endgame_time_limit_ms;
  }
  const double seconds = time_limit_seconds ? *time_limit_seconds : time_limit_ms / 1000.0;
  Budget budget(max_nodes, seconds);

  result.attempted = true;
  SweepResult sweep = anytime_sweep(game, attacker, config, budget);
  const uint64_t anytime_nodes = budget.used();
  result.stop_reason = sweep.stop_reason;
  result.nodes = anytime_nodes;
  // The exact sweep has its own node budget (as the Python driver) but only
  // the time left before the shared deadline; none left means no sweep.
  if (!sweep.mate && endgame && !budget.time_exhausted()) {
    Budget exact_budget(max_nodes, budget.seconds_left());
    const SweepResult exact = exact_sweep(game, attacker, config.endgame_exact_depth,
                                          std::max<uint64_t>(1, max_nodes), exact_budget);
    result.nodes += exact_budget.used();
    if (exact.mate) {
      sweep = exact;
      result.stop_reason = "exact_endgame_mate";
    }
  }
  if (sweep.mate) {
    result.value_proven = true;
    result.depth = sweep.depth;
    if (!sweep.has_root_action) {
      result.stop_reason = "mate_proven_without_root_action";
    } else {
      for (const Action &action : game.legal_actions()) {
        if (action.pack() == sweep.root_action) {
          result.proven = true;
          result.action_code = static_cast<int64_t>(sweep.root_action);
          result.action_id = ActionEncoderV4::encode(action, game);
          break;
        }
      }
      if (!result.proven)
        throw std::logic_error("mate solver returned a non-legal root action");
    }
  }
  result.elapsed_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - started).count();
  return result;
}

} // namespace csplendor

#endif // CSPLENDOR_ROOT_MATE_PROBE_H
