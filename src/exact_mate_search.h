#ifndef CSPLENDOR_EXACT_MATE_SEARCH_H
#define CSPLENDOR_EXACT_MATE_SEARCH_H

// Exact mate depth sweep (doc/mate_usage.md, 厳密な詰みの深さ探索): the
// single-threaded search_reveal_verified_mate_depths over every attacker move
// and every concrete reveal. Free of Python so the WASM build can call it;
// root_mate_probe uses it for its endgame sweep.

#include "action_encoder_v4.h"
#include "card_data.h"
#include "game.h"
#include "noble_data.h"
#include "reveal_verified_solver.h"

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace csplendor {

// A cheap, sound reason why the attacker can never win (empty if none): the
// game is over without the attacker winning, or every card and noble it
// could still gain cannot lift it to 15 points.
inline std::string permanent_no_mate_reason(const Game &game, int attacker) {
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
  return ceiling >= 15 ? "" : "attacker_score_ceiling_below_win_threshold";
}

struct ExactMateSearchConfig {
  int attacker = 0;
  int min_depth = 1;
  int max_depth = 3;
  uint64_t max_nodes = 0;          // cumulative over all depths, 0 = unlimited
  double time_limit_seconds = 0.0; // cumulative over all depths, 0 = unlimited
  size_t max_memo_states = 0;      // 0 = unbounded
};

struct ExactMateSearchResult {
  // "mate", "no_mate_within_max_depth", "unknown" or "permanent_no_mate"
  std::string status;
  std::string stop_reason;
  int mate_depth = -1;
  int verified_no_mate_through_depth = -1;
  std::vector<RevealVerifiedLineEntry> line; // principal line of the mate
  bool has_winning_root_action = false;
  uint64_t winning_root_action = 0; // Action::pack()
  int winning_root_action_id = -1;  // ActionEncoderV4 id (attacker to move)
  uint64_t nodes = 0;
  double elapsed_ms = 0.0;
};

inline ExactMateSearchResult exact_mate_search(const Game &game,
                                               const ExactMateSearchConfig &config) {
  using Clock = std::chrono::steady_clock;
  if (config.attacker < 0 || config.attacker >= Board::NUM_PLAYERS)
    throw std::invalid_argument("attacker must be 0 or 1");
  if (config.min_depth < 0 || config.max_depth < config.min_depth)
    throw std::invalid_argument("depth range must satisfy 0 <= min_depth <= max_depth");
  const auto started = Clock::now();
  const auto elapsed_seconds = [&] {
    return std::chrono::duration<double>(Clock::now() - started).count();
  };
  ExactMateSearchResult result;

  const std::string certificate = permanent_no_mate_reason(game, config.attacker);
  if (!certificate.empty()) {
    result.status = "permanent_no_mate";
    result.stop_reason = certificate;
    result.verified_no_mate_through_depth = config.max_depth;
    result.elapsed_ms = elapsed_seconds() * 1000.0;
    return result;
  }

  result.status = "no_mate_within_max_depth";
  result.stop_reason = "max_depth_reached";
  for (int depth = config.min_depth; depth <= config.max_depth; ++depth) {
    if (config.max_nodes != 0 && result.nodes >= config.max_nodes) {
      result.status = "unknown";
      result.stop_reason = "cumulative node limit exceeded";
      break;
    }
    const double remaining_time =
        config.time_limit_seconds > 0.0 ? config.time_limit_seconds - elapsed_seconds() : 0.0;
    if (config.time_limit_seconds > 0.0 && remaining_time <= 0.0) {
      result.status = "unknown";
      result.stop_reason = "cumulative time limit exceeded";
      break;
    }
    const uint64_t remaining_nodes = config.max_nodes == 0 ? 0 : config.max_nodes - result.nodes;
    RevealVerifiedSolver solver(config.attacker, depth, remaining_nodes, remaining_time, {},
                                false, 100000, 500000, UINT64_MAX, false, 0, true, true);
    solver.set_max_memo_states(config.max_memo_states);
    const RevealVerifiedSearchResult raw = solver.solve(game);
    result.nodes += raw.stats.nodes;
    if (raw.proven) {
      result.status = "mate";
      result.stop_reason = "mate_proven";
      result.mate_depth = depth;
      result.line = raw.line;
      break;
    }
    if (!raw.unknown_reason.empty()) {
      result.status = "unknown";
      result.stop_reason = raw.unknown_reason;
      break;
    }
    // This depth is conclusively refuted; only now is the next one sound.
    result.verified_no_mate_through_depth = depth;
  }

  if (!result.line.empty()) {
    result.has_winning_root_action = true;
    result.winning_root_action = result.line.front().action_code;
    if (game.current_player() == config.attacker)
      for (const Action &action : game.legal_actions())
        if (action.pack() == result.winning_root_action) {
          result.winning_root_action_id = ActionEncoderV4::encode(action, game);
          break;
        }
  }
  result.elapsed_ms = elapsed_seconds() * 1000.0;
  return result;
}

} // namespace csplendor

#endif // CSPLENDOR_EXACT_MATE_SEARCH_H
