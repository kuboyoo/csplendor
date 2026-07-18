#ifndef CSPLENDOR_VISIBLE_ONLY_SOLVER_H
#define CSPLENDOR_VISIBLE_ONLY_SOLVER_H

#include "action.h"
#include "game.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct VisibleOnlySearchStats {
  uint64_t nodes = 0;
  uint64_t memo_hits = 0;
  uint64_t terminal_nodes = 0;
  uint64_t legal_moves = 0;
  double elapsed_ms = 0.0;
};

struct VisibleOnlyLineEntry {
  uint64_t action_code = 0;
  int winner = -1;
  std::string reason;
  size_t action_count = 0;
};

struct VisibleOnlySearchResult {
  int winner = -1;
  int forced_win_depth = -1;
  bool simple_payment_mode = false;
  std::string winner_reason = "unknown";
  std::string unknown_reason;
  size_t memoized_states = 0;
  VisibleOnlySearchStats stats;
  std::vector<VisibleOnlyLineEntry> line;
};

class VisibleOnlySolver {
public:
  VisibleOnlySolver(uint64_t max_nodes, double time_limit_seconds)
      : max_nodes_(max_nodes), time_limit_seconds_(time_limit_seconds) {}

  VisibleOnlySearchResult solve(const Game &input) {
    memo_.clear();
    stats_ = VisibleOnlySearchStats();
    start_time_ = Clock::now();

    Game game = input.clone_light();
    game.blank_refill_mode = true;
    for (int level = 0; level < 3; ++level)
      game.board.decks[level].clear();
    game.board.invalidate_hash();
    root_ = game.clone_light();

    VisibleOnlySearchResult result;
    result.simple_payment_mode = game.simple_payment_mode;
    try {
      const int attacker = game.current_player();
      for (int depth = 1; depth <= FORCED_WIN_MAX_DEPTH; ++depth) {
        forced_memo_.clear();
        std::unordered_set<DepthStateKey, DepthStateKeyHash> forced_path;
        if (forced_win(game, forced_path, known_card_count(game), attacker,
                       depth) == ForceStatus::PROVEN) {
          result.winner = attacker;
          result.forced_win_depth = depth;
          result.winner_reason = "forced_visible_only_win";
          result.line = forced_principal_line(attacker, depth);
          stats_.elapsed_ms = elapsed_ms();
          result.memoized_states = forced_memo_.size();
          result.stats = stats_;
          return result;
        }
      }

      std::unordered_set<StateKey, StateKeyHash> path;
      result.winner =
          winner_from_score(minimax(game, path, known_card_count(game), -2, 2));
      auto it = memo_.find(state_key(game));
      result.winner_reason =
          it == memo_.end() ? terminal_reason(game) : it->second.reason;
      result.line = principal_line();
    } catch (const SearchLimitExceeded &exc) {
      result.winner = -1;
      result.winner_reason = exc.what();
      result.unknown_reason = exc.what();
    }

    stats_.elapsed_ms = elapsed_ms();
    result.memoized_states = memo_.size();
    result.stats = stats_;
    return result;
  }

private:
  using Clock = std::chrono::steady_clock;

  struct SearchLimitExceeded : public std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  struct StateKey {
    uint64_t board_hash = 0;
    uint8_t points0 = 0;
    uint8_t points1 = 0;
    uint8_t purchased0 = 0;
    uint8_t purchased1 = 0;
    bool final_round = false;
    int8_t winner = -1;

    bool operator==(const StateKey &other) const {
      return board_hash == other.board_hash && points0 == other.points0 &&
             points1 == other.points1 && purchased0 == other.purchased0 &&
             purchased1 == other.purchased1 &&
             final_round == other.final_round && winner == other.winner;
    }
  };

  struct StateKeyHash {
    size_t operator()(const StateKey &key) const {
      uint64_t meta = static_cast<uint64_t>(key.points0);
      meta |= static_cast<uint64_t>(key.points1) << 8;
      meta |= static_cast<uint64_t>(key.purchased0) << 16;
      meta |= static_cast<uint64_t>(key.purchased1) << 24;
      meta |= static_cast<uint64_t>(key.final_round ? 1 : 0) << 32;
      meta |= static_cast<uint64_t>(static_cast<uint8_t>(key.winner + 2))
              << 33;
      return std::hash<uint64_t>{}(key.board_hash ^
                                   (meta * 0x9e3779b97f4a7c15ULL));
    }
  };

  struct Entry {
    enum class Bound : uint8_t { EXACT, LOWER, UPPER };

    int score = 0;
    Bound bound = Bound::EXACT;
    std::string reason;
    uint64_t action_code = 0;
    bool has_action = false;
    size_t action_count = 0;
  };

  enum class ForceStatus : uint8_t { PROVEN, REFUTED, UNKNOWN };

  struct DepthStateKey {
    StateKey state;
    int depth = 0;

    bool operator==(const DepthStateKey &other) const {
      return state == other.state && depth == other.depth;
    }
  };

  struct DepthStateKeyHash {
    size_t operator()(const DepthStateKey &key) const {
      return StateKeyHash{}(key.state) ^
             (std::hash<int>{}(key.depth) * 0x9e3779b9U);
    }
  };

  struct ForceEntry {
    ForceStatus status = ForceStatus::UNKNOWN;
    uint64_t action_code = 0;
    bool has_action = false;
    size_t action_count = 0;
  };

  struct OrderedAction {
    int rank = 9;
    int neg_points = 0;
    uint64_t code = 0;

    bool operator<(const OrderedAction &other) const {
      if (rank != other.rank)
        return rank < other.rank;
      if (neg_points != other.neg_points)
        return neg_points < other.neg_points;
      return code < other.code;
    }
  };

  uint64_t max_nodes_ = 0;
  double time_limit_seconds_ = 0.0;
  Clock::time_point start_time_;
  VisibleOnlySearchStats stats_;
  std::unordered_map<StateKey, Entry, StateKeyHash> memo_;
  std::unordered_map<DepthStateKey, ForceEntry, DepthStateKeyHash> forced_memo_;
  Game root_{0};
  static constexpr int FORCED_WIN_MAX_DEPTH = 8;
  static constexpr size_t ATTACKER_TAKE_LIMIT = 6;
  static constexpr size_t ATTACKER_RESERVE_LIMIT = 3;

  ForceStatus
  forced_win(Game &game,
             std::unordered_set<DepthStateKey, DepthStateKeyHash> &path,
             int known_cards, int attacker, int depth) {
    check_limits();
    ++stats_.nodes;

    if (game.is_game_over()) {
      ++stats_.terminal_nodes;
      return game.winner() == attacker ? ForceStatus::PROVEN
                                       : ForceStatus::REFUTED;
    }
    if (known_cards == 0) {
      ++stats_.terminal_nodes;
      return ForceStatus::REFUTED;
    }
    if (game.current_player() == attacker && depth <= 0)
      return ForceStatus::REFUTED;

    const DepthStateKey key{state_key(game), depth};
    auto memo_it = forced_memo_.find(key);
    if (memo_it != forced_memo_.end()) {
      ++stats_.memo_hits;
      return memo_it->second.status;
    }
    if (path.find(key) != path.end()) {
      ++stats_.terminal_nodes;
      return ForceStatus::UNKNOWN;
    }

    std::vector<OrderedAction> actions = representative_actions(game);
    if (game.current_player() == attacker)
      actions = forced_attacker_actions(game, actions);
    stats_.legal_moves += actions.size();
    if (actions.empty()) {
      ++stats_.terminal_nodes;
      forced_memo_[key] = ForceEntry{ForceStatus::REFUTED, 0, false, 0};
      return ForceStatus::REFUTED;
    }

    const int current_player = game.current_player();
    path.insert(key);
    bool has_unknown = false;
    uint64_t first_action = 0;
    bool has_first_action = false;
    for (const OrderedAction &ordered : actions) {
      const Action action = Action::unpack(ordered.code);
      const Board previous = game.board;
      if (!game.apply_action_code_trusted(ordered.code, false))
        continue;
      const int next_depth =
          depth - static_cast<int>(current_player == attacker &&
                                   game.current_player() != current_player);
      const ForceStatus child =
          forced_win(game, path,
                     known_cards - static_cast<int>(action.type == PURCHASE),
                     attacker, next_depth);
      game.board = previous;

      if (!has_first_action) {
        first_action = ordered.code;
        has_first_action = true;
      }
      if (current_player == attacker && child == ForceStatus::PROVEN) {
        path.erase(key);
        forced_memo_[key] =
            ForceEntry{ForceStatus::PROVEN, ordered.code, true, actions.size()};
        return ForceStatus::PROVEN;
      }
      if (current_player != attacker && child == ForceStatus::REFUTED) {
        path.erase(key);
        forced_memo_[key] =
            ForceEntry{ForceStatus::REFUTED, ordered.code, true, actions.size()};
        return ForceStatus::REFUTED;
      }
      has_unknown = has_unknown || child == ForceStatus::UNKNOWN;
    }
    path.erase(key);

    if (has_unknown)
      return ForceStatus::UNKNOWN;
    const ForceStatus status = current_player == attacker
                                   ? ForceStatus::REFUTED
                                   : ForceStatus::PROVEN;
    forced_memo_[key] =
        ForceEntry{status, first_action, has_first_action, actions.size()};
    return status;
  }

  int minimax(Game &game, std::unordered_set<StateKey, StateKeyHash> &path,
              int known_cards, int alpha, int beta) {
    check_limits();
    ++stats_.nodes;

    if (game.is_game_over()) {
      ++stats_.terminal_nodes;
      return score_from_winner(game.winner());
    }
    if (known_cards == 0) {
      ++stats_.terminal_nodes;
      return score_from_winner(adjudicate_by_score(game));
    }

    const StateKey key = state_key(game);
    auto memo_it = memo_.find(key);
    if (memo_it != memo_.end()) {
      ++stats_.memo_hits;
      if (memo_it->second.bound == Entry::Bound::EXACT)
        return memo_it->second.score;
      if (memo_it->second.bound == Entry::Bound::LOWER)
        alpha = std::max(alpha, memo_it->second.score);
      else
        beta = std::min(beta, memo_it->second.score);
      if (alpha >= beta)
        return memo_it->second.score;
    }
    if (path.find(key) != path.end()) {
      ++stats_.terminal_nodes;
      return score_from_winner(adjudicate_by_score(game));
    }

    std::vector<OrderedAction> actions = representative_actions(game);
    stats_.legal_moves += actions.size();
    if (actions.empty()) {
      ++stats_.terminal_nodes;
      const int score = score_from_winner(adjudicate_by_score(game));
      memo_[key] =
          Entry{score, Entry::Bound::EXACT,
                "no_visible_only_action_adjudicated_by_score", 0, false, 0};
      return score;
    }

    const int current_player = game.current_player();
    const int original_alpha = alpha;
    const int original_beta = beta;
    path.insert(key);
    int best_score = current_player == 0 ? -2 : 2;
    uint64_t best_action = 0;
    bool has_best_action = false;

    for (const OrderedAction &ordered : actions) {
      const Action action = Action::unpack(ordered.code);
      const Board previous = game.board;
      if (!game.apply_action_code_trusted(ordered.code, false))
        continue;
      const int child_score =
          minimax(game, path,
                  known_cards - static_cast<int>(action.type == PURCHASE),
                  alpha, beta);
      game.board = previous;

      if ((current_player == 0 && child_score > best_score) ||
          (current_player == 1 && child_score < best_score)) {
        best_score = child_score;
        best_action = ordered.code;
        has_best_action = true;
      }
      if (current_player == 0)
        alpha = std::max(alpha, best_score);
      else
        beta = std::min(beta, best_score);
      if (alpha >= beta)
        break;
    }
    path.erase(key);

    Entry::Bound bound = Entry::Bound::EXACT;
    if (best_score <= original_alpha)
      bound = Entry::Bound::UPPER;
    else if (best_score >= original_beta)
      bound = Entry::Bound::LOWER;
    memo_[key] =
        Entry{best_score, bound, score_reason(best_score, current_player),
              best_action, has_best_action, actions.size()};
    return best_score;
  }

  std::vector<OrderedAction> representative_actions(Game &game) {
    std::unordered_map<StateKey, OrderedAction, StateKeyHash> representatives;
    for (uint64_t code : game.legal_action_codes()) {
      const Action action = Action::unpack(code);
      if (action.type == RESERVE_DECK)
        continue;
      const Board previous = game.board;
      if (!game.apply_action_code_trusted(code, false))
        continue;
      const StateKey child_key = state_key(game);
      game.board = previous;

      const OrderedAction candidate = ordered_action(action, code);
      auto it = representatives.find(child_key);
      if (it == representatives.end() || candidate < it->second)
        representatives[child_key] = candidate;
    }

    std::vector<OrderedAction> actions;
    actions.reserve(representatives.size());
    for (const auto &item : representatives)
      actions.push_back(item.second);
    std::sort(actions.begin(), actions.end());
    return actions;
  }

  static std::vector<OrderedAction>
  forced_attacker_actions(const Game &game,
                          const std::vector<OrderedAction> &actions) {
    std::vector<OrderedAction> purchases;
    std::vector<std::pair<int, OrderedAction>> takes;
    std::vector<OrderedAction> reserves;
    std::vector<OrderedAction> passthrough;
    for (const OrderedAction &ordered : actions) {
      const Action action = Action::unpack(ordered.code);
      switch (action.type) {
      case PURCHASE:
      case VISIT_NOBLE:
        purchases.push_back(ordered);
        break;
      case TAKE_DIFFERENT:
      case TAKE_SAME:
        takes.push_back({attacker_take_score(game, action), ordered});
        break;
      case RESERVE_VISIBLE:
        reserves.push_back(ordered);
        break;
      default:
        passthrough.push_back(ordered);
        break;
      }
    }

    std::sort(takes.begin(), takes.end(),
              [](const auto &left, const auto &right) {
                if (left.first != right.first)
                  return left.first > right.first;
                return left.second < right.second;
              });
    std::sort(reserves.begin(), reserves.end());

    std::vector<OrderedAction> filtered;
    filtered.insert(filtered.end(), purchases.begin(), purchases.end());
    for (size_t index = 0;
         index < takes.size() && index < ATTACKER_TAKE_LIMIT; ++index)
      filtered.push_back(takes[index].second);
    for (size_t index = 0;
         index < reserves.size() && index < ATTACKER_RESERVE_LIMIT; ++index)
      filtered.push_back(reserves[index]);
    filtered.insert(filtered.end(), passthrough.begin(), passthrough.end());
    return filtered;
  }

  static int attacker_take_score(const Game &game, const Action &action) {
    const PlayerState &player = game.board.players[game.current_player()];
    std::array<int, 6> after_gems = {0};
    for (int color = 0; color < 6; ++color) {
      const int take = color < 5 ? action.take[color] : 0;
      after_gems[color] =
          static_cast<int>(player.gems[color]) + take - action.return_gems[color];
    }

    int best = 0;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
        const int card_id = game.board.visible[level][slot];
        if (is_valid_card_id(card_id))
          best = std::max(best, attacker_take_score_for_card(
                                    player, after_gems, get_card(card_id)));
      }
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
      const int card_id = player.reserved[slot];
      if (is_valid_card_id(card_id))
        best = std::max(
            best,
            attacker_take_score_for_card(player, after_gems, get_card(card_id)));
    }
    return best;
  }

  static int attacker_take_score_for_card(const PlayerState &player,
                                          const std::array<int, 6> &after_gems,
                                          const Card &card) {
    const int before_gap = card_payment_gap(player, player.gems, card);
    const int after_gap = card_payment_gap(player, after_gems, card);
    const int progress = before_gap - after_gap;
    return card.points * 1000 + progress * 100 - after_gap * 10;
  }

  template <typename Gems>
  static int card_payment_gap(const PlayerState &player, const Gems &gems,
                              const Card &card) {
    int shortage = 0;
    for (int color = 0; color < 5; ++color) {
      const int need =
          std::max(0, static_cast<int>(card.cost[color]) -
                          static_cast<int>(player.bonuses[color]));
      shortage += std::max(0, need - static_cast<int>(gems[color]));
    }
    return std::max(0, shortage - static_cast<int>(gems[GOLD]));
  }

  static OrderedAction ordered_action(const Action &action, uint64_t code) {
    int rank = 9;
    switch (action.type) {
    case PURCHASE:
    case VISIT_NOBLE:
      rank = 0;
      break;
    case RESERVE_VISIBLE:
      rank = 1;
      break;
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      rank = 2;
      break;
    default:
      break;
    }
    int points = 0;
    if (is_valid_card_id(action.card_id))
      points = get_card(action.card_id).points;
    return OrderedAction{rank, -points, code};
  }

  static int known_card_count(const Game &game) {
    int count = 0;
    for (int level = 0; level < 3; ++level)
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        count += static_cast<int>(game.board.visible[level][slot] >= 0);
    for (int player = 0; player < Board::NUM_PLAYERS; ++player)
      for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
        count += static_cast<int>(game.board.players[player].reserved[slot] >=
                                  0);
    return count;
  }

  static int adjudicate_by_score(const Game &game) {
    const PlayerState &player0 = game.board.players[0];
    const PlayerState &player1 = game.board.players[1];
    if (player0.points > player1.points)
      return 0;
    if (player1.points > player0.points)
      return 1;
    if (player0.purchased_count < player1.purchased_count)
      return 0;
    if (player1.purchased_count < player0.purchased_count)
      return 1;
    return -2;
  }

  static int score_from_winner(int winner) {
    if (winner == 0)
      return 1;
    if (winner == 1)
      return -1;
    return 0;
  }

  static int winner_from_score(int score) {
    if (score > 0)
      return 0;
    if (score < 0)
      return 1;
    return -2;
  }

  static std::string score_reason(int score, int current_player) {
    if (winner_from_score(score) == current_player)
      return "current_player_can_force_visible_only_win";
    if (score == 0)
      return "current_player_can_force_visible_only_draw";
    return "all_visible_only_responses_lose_for_current_player";
  }

  static StateKey state_key(const Game &game) {
    const Board &board = game.board;
    return StateKey{board.hash(),
                    board.players[0].points,
                    board.players[1].points,
                    board.players[0].purchased_count,
                    board.players[1].purchased_count,
                    board.final_round,
                    board.winner};
  }

  static std::string terminal_reason(const Game &game) {
    if (game.is_game_over())
      return "game_over";
    if (known_card_count(game) == 0)
      return "visible_cards_exhausted_adjudicated_by_score";
    return "unknown";
  }

  std::vector<VisibleOnlyLineEntry> forced_principal_line(int attacker,
                                                          int depth) const {
    Game game = root_.clone_light();
    std::vector<VisibleOnlyLineEntry> line;
    std::unordered_set<DepthStateKey, DepthStateKeyHash> seen;
    for (int ply = 0; ply < 200 && depth >= 0; ++ply) {
      if (game.is_game_over() || known_card_count(game) == 0)
        break;
      const DepthStateKey key{state_key(game), depth};
      if (seen.find(key) != seen.end())
        break;
      seen.insert(key);
      auto it = forced_memo_.find(key);
      if (it == forced_memo_.end() || !it->second.has_action)
        break;
      const int current_player = game.current_player();
      if (!game.apply_action_code_trusted(it->second.action_code, false))
        break;
      line.push_back(VisibleOnlyLineEntry{it->second.action_code,
                                          attacker,
                                          "forced_visible_only_win",
                                          it->second.action_count});
      depth -= static_cast<int>(current_player == attacker &&
                                game.current_player() != current_player);
    }
    return line;
  }

  std::vector<VisibleOnlyLineEntry> principal_line() const {
    Game game = root_.clone_light();
    std::vector<VisibleOnlyLineEntry> line;
    std::unordered_set<StateKey, StateKeyHash> seen;
    for (int ply = 0; ply < 200; ++ply) {
      if (game.is_game_over() || known_card_count(game) == 0)
        break;
      const StateKey key = state_key(game);
      if (seen.find(key) != seen.end())
        break;
      seen.insert(key);
      auto it = memo_.find(key);
      if (it == memo_.end() || !it->second.has_action)
        break;
      if (!game.apply_action_code_trusted(it->second.action_code, false))
        break;
      line.push_back(VisibleOnlyLineEntry{it->second.action_code,
                                          winner_from_score(it->second.score),
                                          it->second.reason,
                                          it->second.action_count});
    }
    return line;
  }

  void check_limits() const {
    if (max_nodes_ && stats_.nodes >= max_nodes_)
      throw SearchLimitExceeded("node limit exceeded");
    if (time_limit_seconds_ > 0.0 &&
        elapsed_ms() >= time_limit_seconds_ * 1000.0)
      throw SearchLimitExceeded("time limit exceeded");
  }

  double elapsed_ms() const {
    return std::chrono::duration<double, std::milli>(Clock::now() - start_time_)
        .count();
  }
};

#endif // CSPLENDOR_VISIBLE_ONLY_SOLVER_H
