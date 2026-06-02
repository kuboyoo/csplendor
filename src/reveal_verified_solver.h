#ifndef CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
#define CSPLENDOR_REVEAL_VERIFIED_SOLVER_H

#include "action.h"
#include "game.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct RevealVerifiedSearchStats {
  uint64_t nodes = 0;
  uint64_t memo_hits = 0;
  uint64_t terminal_nodes = 0;
  uint64_t legal_moves = 0;
  uint64_t reveal_branches = 0;
  uint64_t final_round_reveal_collapses = 0;
  uint64_t final_round_score_prunes = 0;
  uint64_t final_round_direct_resolutions = 0;
  uint64_t oracle_purchase_actions = 0;
  uint64_t oracle_reserve_actions = 0;
  uint64_t deck_reserve_candidates = 0;
  uint64_t deck_reserve_branches = 0;
  double elapsed_ms = 0.0;
};

struct RevealVerifiedLineEntry {
  uint64_t action_code = 0;
  int reveal_card = -1;
  size_t action_count = 0;
};

struct RevealVerifiedProofEdge {
  uint64_t action_code = 0;
  int reveal_card = -1;
  int oracle_card = -1;
  bool oracle_reserve = false;
  int oracle_return_color = -1;
  std::array<uint8_t, 5> oracle_gold_as = {0};
  size_t child = 0;
};

struct RevealVerifiedProofNode {
  size_t id = 0;
  int player = -1;
  int depth = 0;
  std::string kind = "state";
  std::string resolution;
  std::vector<RevealVerifiedProofEdge> children;
};

struct RevealVerifiedProofDag {
  bool requested = false;
  bool complete = false;
  std::string omitted_reason;
  size_t root = 0;
  std::vector<RevealVerifiedProofNode> nodes;
};

struct RevealVerifiedSearchResult {
  bool proven = false;
  int attacker = -1;
  int depth = -1;
  std::string reason = "unknown";
  std::string unknown_reason;
  size_t memoized_states = 0;
  RevealVerifiedSearchStats stats;
  std::vector<RevealVerifiedLineEntry> line;
  RevealVerifiedProofDag proof_dag;
};

class RevealVerifiedSolver {
public:
  RevealVerifiedSolver(int attacker, int depth, uint64_t max_nodes,
                       double time_limit_seconds,
                       std::vector<uint64_t> preferred_attacker_actions = {},
                       bool include_proof_dag = false,
                       size_t proof_dag_node_limit = 100000,
                       size_t proof_dag_edge_limit = 500000,
                       uint64_t required_root_action = UINT64_MAX)
      : attacker_(attacker), depth_(depth), max_nodes_(max_nodes),
        time_limit_seconds_(time_limit_seconds),
        preferred_attacker_actions_(std::move(preferred_attacker_actions)),
        include_proof_dag_(include_proof_dag),
        proof_dag_node_limit_(proof_dag_node_limit),
        proof_dag_edge_limit_(proof_dag_edge_limit),
        required_root_action_(required_root_action) {}

  RevealVerifiedSearchResult solve(const Game &input) {
    memo_.clear();
    stats_ = RevealVerifiedSearchStats();
    start_time_ = Clock::now();

    Game game = input.clone_light();
    // Keep refill timing and level as blank slots. The defender may use any
    // initially hidden card of a blank level, which dominates every concrete
    // reveal sequence without enumerating deck permutations.
    game.blank_refill_mode = true;
    root_ = game.clone_light();
    remember_initial_known_cards(game);
    remember_initial_hidden_cards(game);

    RevealVerifiedSearchResult result;
    result.attacker = attacker_;
    result.depth = depth_;
    try {
      std::unordered_set<DepthStateKey, DepthStateKeyHash> path;
      const ForceStatus status = forced_win(game, path, depth_);
      result.proven = status == ForceStatus::PROVEN;
      result.reason = result.proven ? "all_reveals_verified"
                                    : "candidate_mate_not_verified";
      result.line = principal_line();
      if (result.proven && include_proof_dag_)
        result.proof_dag = build_proof_dag();
    } catch (const SearchLimitExceeded &exc) {
      result.reason = exc.what();
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

  struct ProofDagBuildAborted : public std::runtime_error {
    using std::runtime_error::runtime_error;
  };

  enum class ForceStatus : uint8_t { PROVEN, REFUTED, UNKNOWN };

  struct StateKey {
    uint64_t board_hash = 0;
    uint64_t unseen_low = 0;
    uint64_t unseen_high = 0;
    uint64_t acquired_hidden_low = 0;
    uint64_t acquired_hidden_high = 0;
    uint8_t points0 = 0;
    uint8_t points1 = 0;
    uint8_t purchased0 = 0;
    uint8_t purchased1 = 0;
    uint8_t reserved0 = 0;
    uint8_t reserved1 = 0;
    bool final_round = false;
    int8_t winner = -1;

    bool operator==(const StateKey &other) const {
      return board_hash == other.board_hash && unseen_low == other.unseen_low &&
             unseen_high == other.unseen_high &&
             acquired_hidden_low == other.acquired_hidden_low &&
             acquired_hidden_high == other.acquired_hidden_high &&
             points0 == other.points0 &&
             points1 == other.points1 && purchased0 == other.purchased0 &&
             purchased1 == other.purchased1 && reserved0 == other.reserved0 &&
             reserved1 == other.reserved1 &&
             final_round == other.final_round && winner == other.winner;
    }
  };

  struct StateKeyHash {
    size_t operator()(const StateKey &key) const {
      uint64_t meta = static_cast<uint64_t>(key.points0);
      meta |= static_cast<uint64_t>(key.points1) << 8;
      meta |= static_cast<uint64_t>(key.purchased0) << 16;
      meta |= static_cast<uint64_t>(key.purchased1) << 24;
      meta |= static_cast<uint64_t>(key.reserved0) << 32;
      meta |= static_cast<uint64_t>(key.reserved1) << 40;
      meta |= static_cast<uint64_t>(key.final_round ? 1 : 0) << 48;
      meta |= static_cast<uint64_t>(static_cast<uint8_t>(key.winner + 2))
              << 49;
      const uint64_t mixed =
          key.board_hash ^ (key.unseen_low * 0x9e3779b97f4a7c15ULL) ^
          (key.unseen_high * 0xc2b2ae3d27d4eb4fULL) ^
          (key.acquired_hidden_low * 0x27d4eb2f165667c5ULL) ^
          (key.acquired_hidden_high * 0x94d049bb133111ebULL) ^
          (meta * 0x165667b19e3779f9ULL);
      return std::hash<uint64_t>{}(mixed);
    }
  };

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

  struct Entry {
    ForceStatus status = ForceStatus::UNKNOWN;
    uint64_t action_code = 0;
    int reveal_card = -1;
    bool has_action = false;
    size_t action_count = 0;
    bool replayable = true;
  };

  struct OrderedAction {
    int rank = 9;
    int neg_points = 0;
    uint64_t code = 0;
    int oracle_card = -1;
    bool oracle_reserve = false;
    int oracle_return_color = -1;
    std::array<uint8_t, 5> oracle_gold_as = {0};

    bool operator<(const OrderedAction &other) const {
      if (rank != other.rank)
        return rank < other.rank;
      if (neg_points != other.neg_points)
        return neg_points < other.neg_points;
      if (code != other.code)
        return code < other.code;
      if (oracle_card != other.oracle_card)
        return oracle_card < other.oracle_card;
      if (oracle_reserve != other.oracle_reserve)
        return oracle_reserve < other.oracle_reserve;
      if (oracle_return_color != other.oracle_return_color)
        return oracle_return_color < other.oracle_return_color;
      return oracle_gold_as < other.oracle_gold_as;
    }
  };

  int attacker_ = 0;
  int depth_ = 0;
  uint64_t max_nodes_ = 0;
  double time_limit_seconds_ = 0.0;
  Clock::time_point start_time_;
  RevealVerifiedSearchStats stats_;
  std::unordered_map<DepthStateKey, Entry, DepthStateKeyHash> memo_;
  Game root_{0};
  uint64_t initial_known_low_ = 0;
  uint64_t initial_known_high_ = 0;
  uint64_t initial_hidden_low_ = 0;
  uint64_t initial_hidden_high_ = 0;
  std::vector<uint64_t> preferred_attacker_actions_;
  bool include_proof_dag_ = false;
  size_t proof_dag_node_limit_ = 100000;
  size_t proof_dag_edge_limit_ = 500000;
  uint64_t required_root_action_ = UINT64_MAX;
  size_t proof_dag_edges_ = 0;
  std::unordered_map<DepthStateKey, size_t, DepthStateKeyHash> proof_node_ids_;
  std::vector<RevealVerifiedProofNode> proof_nodes_;
  static constexpr size_t ATTACKER_TAKE_LIMIT = 6;
  static constexpr size_t ATTACKER_RESERVE_LIMIT = 3;

  ForceStatus
  forced_win(Game &game,
             std::unordered_set<DepthStateKey, DepthStateKeyHash> &path,
             int depth) {
    check_limits();
    ++stats_.nodes;

    if (game.is_game_over()) {
      ++stats_.terminal_nodes;
      return game.winner() == attacker_ ? ForceStatus::PROVEN
                                        : ForceStatus::REFUTED;
    }
    if (game.current_player() == attacker_ && depth <= 0)
      return ForceStatus::REFUTED;

    const DepthStateKey key{state_key(game), depth};
    auto memo_it = memo_.find(key);
    if (memo_it != memo_.end()) {
      ++stats_.memo_hits;
      return memo_it->second.status;
    }
    if (path.find(key) != path.end()) {
      ++stats_.terminal_nodes;
      return ForceStatus::UNKNOWN;
    }
    if (can_resolve_final_round(game)) {
      const Entry resolved = resolve_final_round(game);
      memo_[key] = resolved;
      return resolved.status;
    }

    std::vector<OrderedAction> actions = ordered_actions(game);
    if (game.current_player() == attacker_)
      actions = forced_attacker_actions(game, actions, depth, path.empty());
    stats_.legal_moves += actions.size();
    if (actions.empty()) {
      ++stats_.terminal_nodes;
      memo_[key] = Entry{ForceStatus::REFUTED, 0, -1, false, 0};
      return ForceStatus::REFUTED;
    }

    const int current_player = game.current_player();
    path.insert(key);
    bool has_unknown = false;
    Entry representative;
    representative.action_count = actions.size();

    for (const OrderedAction &ordered : actions) {
      bool action_unknown = false;
      bool action_refuted = false;
      int representative_reveal = -1;
      bool has_representative_reveal = false;
      const bool completed = for_each_outcome(
          game, ordered,
          [&](int reveal_card) {
            const int next_depth =
                depth - static_cast<int>(current_player == attacker_ &&
                                         game.current_player() !=
                                             current_player);
            const ForceStatus child = forced_win(game, path, next_depth);
            if (!has_representative_reveal) {
              representative_reveal = reveal_card;
              has_representative_reveal = true;
            }
            if (child == ForceStatus::REFUTED) {
              action_refuted = true;
              return false;
            }
            action_unknown = action_unknown || child == ForceStatus::UNKNOWN;
            return true;
          });
      if (!completed && !action_refuted)
        action_unknown = true;

      if (!representative.has_action) {
        representative = Entry{ForceStatus::UNKNOWN, ordered.code,
                               representative_reveal, true, actions.size(),
                               is_replayable(ordered)};
      }
      if (current_player == attacker_ && !action_refuted && !action_unknown) {
        path.erase(key);
        memo_[key] = Entry{ForceStatus::PROVEN, ordered.code,
                           representative_reveal, true, actions.size(),
                           is_replayable(ordered)};
        return ForceStatus::PROVEN;
      }
      if (current_player != attacker_ && action_refuted) {
        path.erase(key);
        memo_[key] = Entry{ForceStatus::REFUTED, ordered.code,
                           representative_reveal, true, actions.size(),
                           is_replayable(ordered)};
        return ForceStatus::REFUTED;
      }
      has_unknown = has_unknown || action_unknown;
    }
    path.erase(key);

    if (has_unknown)
      return ForceStatus::UNKNOWN;
    const ForceStatus status = current_player == attacker_
                                   ? ForceStatus::REFUTED
                                   : ForceStatus::PROVEN;
    representative.status = status;
    memo_[key] = representative;
    return status;
  }

  template <typename Visitor>
  bool for_each_outcome(Game &game, const OrderedAction &ordered,
                        Visitor visitor) {
    if (ordered.oracle_card >= 0 || ordered.oracle_reserve) {
      const Board previous = game.board;
      if (!apply_oracle_action(game, ordered))
        return false;
      const bool keep_going = visitor(-1);
      game.board = previous;
      return keep_going;
    }

    const Action action = Action::unpack(ordered.code);
    if (action.type == RESERVE_DECK)
      return for_each_deck_reserve_outcome(game, action, visitor);

    const Board previous = game.board;
    if (!game.apply_trusted(action, false))
      return false;
    const bool keep_going = visitor(-1);
    game.board = previous;
    return keep_going;
  }

  static bool is_replayable(const OrderedAction &ordered) {
    return ordered.oracle_card < 0 && !ordered.oracle_reserve;
  }

  template <typename Visitor>
  bool for_each_deck_reserve_outcome(Game &game, const Action &action,
                                     Visitor visitor) {
    const std::vector<int> cards = deck_reserve_cards(game, action.deck_level);
    stats_.deck_reserve_candidates += cards.size();
    if (cards.empty())
      return false;
    const Board previous = game.board;
    for (int card_id : cards) {
      game.board = previous;
      if (!apply_deck_reserve_outcome(game, action, card_id))
        continue;
      ++stats_.deck_reserve_branches;
      if (!visitor(card_id)) {
        game.board = previous;
        return false;
      }
    }
    game.board = previous;
    return true;
  }

  std::vector<int> deck_reserve_cards(const Game &game, int level) const {
    std::vector<int> cards;
    if (level < 0 || level >= 3 || game.board.decks[level].empty())
      return cards;
    // Blank refills erase card identity, so conservatively branch over every
    // unclaimed initial hidden card of the requested level.
    for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
      const Card &card = get_card(card_id);
      if (card.level - 1 == level && is_initial_hidden_card(card_id) &&
          !has_hidden_card_claimed(game.board, card_id))
        cards.push_back(card_id);
    }
    return cards;
  }

  static bool apply_deck_reserve_outcome(Game &game, const Action &action,
                                         int card_id) {
    Board &board = game.board;
    if (board.is_game_over() || board.waiting_noble ||
        board.current_player >= Board::NUM_PLAYERS ||
        action.type != RESERVE_DECK ||
        action.deck_level < 0 || action.deck_level >= 3 ||
        board.decks[action.deck_level].empty())
      return false;
    PlayerState &player = board.players[board.current_player];
    if (!player.can_reserve() || !is_valid_card_id(card_id) ||
        get_card(card_id).level - 1 != action.deck_level)
      return false;

    board.invalidate_hash();
    board.decks[action.deck_level].pop_back();
    player.reserved_is_hidden[player.reserved_count] = true;
    player.reserved[player.reserved_count++] = card_id;
    if (board.bank[GOLD] > 0) {
      --board.bank[GOLD];
      ++player.gems[GOLD];
    }
    for (int color = 0; color < 6; ++color) {
      if (action.return_gems[color] > player.gems[color])
        return false;
      player.gems[color] -= action.return_gems[color];
      board.bank[color] += action.return_gems[color];
    }
    player.sync_packed();
    end_oracle_turn(board);
    return true;
  }

  static int visible_refill_level(const Action &action) {
    if (action.type == RESERVE_VISIBLE)
      return get_card(action.card_id).level - 1;
    if (action.type == PURCHASE && !action.from_reserved)
      return get_card(action.card_id).level - 1;
    return -1;
  }

  static bool can_resolve_final_round(const Game &game) {
    return game.board.final_round && !game.board.waiting_noble &&
           game.current_player() == 1;
  }

  Entry resolve_final_round(Game &game) {
    ForceStatus direct_status = ForceStatus::UNKNOWN;
    if (resolve_final_round_direct(game, direct_status)) {
      ++stats_.final_round_direct_resolutions;
      if (direct_status == ForceStatus::PROVEN)
        ++stats_.final_round_score_prunes;
      return Entry{direct_status, 0, -1, false, 0};
    }
    if (max_final_round_score(game) < game.board.players[0].points) {
      ++stats_.final_round_score_prunes;
      return Entry{attacker_ == 0 ? ForceStatus::PROVEN : ForceStatus::REFUTED,
                   0, -1, false, 0};
    }

    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions = ordered_actions(game);
    stats_.legal_moves += actions.size();
    if (actions.empty())
      return Entry{ForceStatus::REFUTED, 0, -1, false, 0};

    bool has_unknown = false;
    Entry representative;
    representative.action_count = actions.size();
    for (const OrderedAction &ordered : actions) {
      const Action action = Action::unpack(ordered.code);
      const Board previous = game.board;
      const int level = visible_refill_level(action);
      const int reveal_card =
          level >= 0 && !game.board.decks[level].empty()
              ? static_cast<int>(game.board.decks[level].back())
              : -1;
      if (level >= 0 && !game.board.decks[level].empty())
        stats_.final_round_reveal_collapses +=
            game.board.decks[level].size() - 1;
      if (!game.apply_trusted(action, false))
        continue;
      const ForceStatus child = game.board.waiting_noble
                                    ? resolve_final_round_noble(game)
                                    : terminal_status(game);
      game.board = previous;

      if (!representative.has_action) {
        representative =
            Entry{child, ordered.code, reveal_card, true, actions.size(),
                  is_replayable(ordered)};
      }
      if (current_player == attacker_ && child == ForceStatus::PROVEN)
        return Entry{ForceStatus::PROVEN, ordered.code, reveal_card, true,
                     actions.size(), is_replayable(ordered)};
      if (current_player != attacker_ && child == ForceStatus::REFUTED)
        return Entry{ForceStatus::REFUTED, ordered.code, reveal_card, true,
                     actions.size(), is_replayable(ordered)};
      has_unknown = has_unknown || child == ForceStatus::UNKNOWN;
    }

    representative.status =
        has_unknown ? ForceStatus::UNKNOWN
                    : current_player == attacker_ ? ForceStatus::REFUTED
                                                  : ForceStatus::PROVEN;
    return representative;
  }

  ForceStatus resolve_final_round_noble(Game &game) {
    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions = ordered_actions(game);
    stats_.legal_moves += actions.size();
    bool has_unknown = false;
    for (const OrderedAction &ordered : actions) {
      const Board previous = game.board;
      if (!game.apply_action_code_trusted(ordered.code, false))
        continue;
      const ForceStatus child = terminal_status(game);
      game.board = previous;
      if (current_player == attacker_ && child == ForceStatus::PROVEN)
        return ForceStatus::PROVEN;
      if (current_player != attacker_ && child == ForceStatus::REFUTED)
        return ForceStatus::REFUTED;
      has_unknown = has_unknown || child == ForceStatus::UNKNOWN;
    }
    return has_unknown ? ForceStatus::UNKNOWN
                       : current_player == attacker_ ? ForceStatus::REFUTED
                                                     : ForceStatus::PROVEN;
  }

  ForceStatus terminal_status(const Game &game) {
    if (!game.is_game_over())
      return ForceStatus::UNKNOWN;
    ++stats_.terminal_nodes;
    return game.winner() == attacker_ ? ForceStatus::PROVEN
                                      : ForceStatus::REFUTED;
  }

  static int max_final_round_score(const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[1];
    int max_score =
        static_cast<int>(player.points) + max_noble_points(board, player.bonuses);
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        max_score = std::max(max_score,
                             score_after_purchase(board, player,
                                                  board.visible[level][slot]));
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      max_score = std::max(
          max_score, score_after_purchase(board, player, player.reserved[slot]));
    return max_score;
  }

  bool resolve_final_round_direct(const Game &game, ForceStatus &status) const {
    const Board &board = game.board;
    const PlayerState &player = board.players[1];
    const bool allow_oracle = attacker_ != 1;
    bool has_action = false;
    bool has_player0_only_outcomes = true;
    bool has_player1_win = false;
    const auto consider = [&](int points, int purchased_count) {
      has_action = true;
      const int winner = final_round_winner(board, points, purchased_count);
      has_player0_only_outcomes = has_player0_only_outcomes && winner == 0;
      has_player1_win = has_player1_win || winner == 1;
    };

    if (has_nonpurchase_action(board, allow_oracle)) {
      consider(static_cast<int>(player.points) +
                   max_noble_points(board, player.bonuses),
               player.purchased_count);
    }
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        consider_purchase(board, player, board.visible[level][slot], consider);
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      consider_purchase(board, player, player.reserved[slot], consider);
    if (allow_oracle) {
      for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
        if (is_initial_hidden_card(card_id) &&
            !has_hidden_card_claimed(board, card_id) &&
            has_blank_slot_at_level(board, get_card(card_id).level - 1))
          consider_purchase(board, player, card_id, consider);
      }
    }

    if (!has_action)
      return false;
    status = attacker_ == 0 ? (has_player0_only_outcomes ? ForceStatus::PROVEN
                                                        : ForceStatus::REFUTED)
                            : (has_player1_win ? ForceStatus::PROVEN
                                               : ForceStatus::REFUTED);
    return true;
  }

  template <typename Visitor>
  static void consider_purchase(const Board &board, const PlayerState &player,
                                int card_id, Visitor visitor) {
    if (!is_valid_card_id(card_id))
      return;
    const Card &card = get_card(card_id);
    if (!player.can_afford(card))
      return;
    std::array<uint8_t, 5> bonuses = player.bonuses;
    ++bonuses[card.bonus];
    visitor(static_cast<int>(player.points) + card.points +
                max_noble_points(board, bonuses),
            player.purchased_count + 1);
  }

  static bool has_nonpurchase_action(const Board &board, bool allow_oracle) {
    if (allow_oracle && has_blank_slot(board))
      return true;
    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] > 0)
        return true;
    }
    if (!board.players[1].can_reserve())
      return false;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
        if (is_valid_card_id(board.visible[level][slot]))
          return true;
      }
    }
    return false;
  }

  static bool has_blank_slot(const Board &board) {
    for (int level = 0; level < 3; ++level) {
      if (has_blank_slot_at_level(board, level))
        return true;
    }
    return false;
  }

  static bool has_blank_slot_at_level(const Board &board, int level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      if (!is_valid_card_id(board.visible[level][slot]))
        return true;
    }
    return false;
  }

  static int final_round_winner(const Board &board, int player1_points,
                                int player1_purchased_count) {
    const PlayerState &player0 = board.players[0];
    if (player0.points > player1_points)
      return 0;
    if (player1_points > player0.points)
      return 1;
    if (player0.purchased_count < player1_purchased_count)
      return 0;
    if (player1_purchased_count < player0.purchased_count)
      return 1;
    return -2;
  }

  static int score_after_purchase(const Board &board, const PlayerState &player,
                                  int card_id) {
    if (!is_valid_card_id(card_id))
      return player.points;
    const Card &card = get_card(card_id);
    if (!player.can_afford(card))
      return player.points;
    std::array<uint8_t, 5> bonuses = player.bonuses;
    ++bonuses[card.bonus];
    return static_cast<int>(player.points) + card.points +
           max_noble_points(board, bonuses);
  }

  static int max_noble_points(const Board &board,
                              const std::array<uint8_t, 5> &bonuses) {
    int points = 0;
    for (uint8_t noble_id : board.nobles) {
      const Noble &noble = get_noble(noble_id);
      bool eligible = true;
      for (int color = 0; color < 5; ++color)
        eligible = eligible && bonuses[color] >= noble.requirement[color];
      if (eligible)
        points = std::max(points, static_cast<int>(noble.points));
    }
    return points;
  }

  std::vector<OrderedAction> ordered_actions(const Game &game) {
    std::vector<OrderedAction> actions = ordinary_ordered_actions(game);
    if (game.current_player() != attacker_ && !game.board.waiting_noble)
      add_oracle_actions(game, actions);
    std::sort(actions.begin(), actions.end());
    return actions;
  }

  static std::vector<OrderedAction> ordinary_ordered_actions(const Game &game) {
    std::vector<OrderedAction> actions;
    for (uint64_t code : game.legal_action_codes()) {
      const Action action = Action::unpack(code);
      actions.push_back(ordered_action(action, code));
    }
    std::sort(actions.begin(), actions.end());
    return actions;
  }

  void add_oracle_actions(const Game &game,
                          std::vector<OrderedAction> &actions) {
    const PlayerState &player = game.board.players[game.current_player()];
    for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
      if (!is_initial_hidden_card(card_id) ||
          has_hidden_card_claimed(game.board, card_id) ||
          !has_blank_slot_at_level(game.board, get_card(card_id).level - 1))
        continue;
      const Card &card = get_card(card_id);
      if (!player.can_afford(card))
        continue;
      std::array<int, 5> effective_cost = {0};
      for (int color = 0; color < 5; ++color) {
        effective_cost[color] =
            std::max(0, static_cast<int>(card.cost[color]) -
                            static_cast<int>(player.bonuses[color]));
      }
      add_oracle_purchase_actions(player, card, effective_cost, 0, 0, {},
                                  actions);
    }
    if (player.can_reserve() && has_blank_slot(game.board)) {
      const int total_gems = player.total_gems();
      if (game.board.bank[GOLD] == 0 || total_gems < Board::MAX_TOKENS) {
        add_oracle_reserve_action(-1, actions);
      } else {
        for (int color = 0; color < 6; ++color) {
          if (player.gems[color] > 0 || color == GOLD)
            add_oracle_reserve_action(color, actions);
        }
      }
    }
  }

  void add_oracle_reserve_action(int return_color,
                                 std::vector<OrderedAction> &actions) {
    OrderedAction action;
    action.rank = 1;
    action.oracle_reserve = true;
    action.oracle_return_color = return_color;
    actions.push_back(action);
    ++stats_.oracle_reserve_actions;
  }

  void add_oracle_purchase_actions(
      const PlayerState &player, const Card &card,
      const std::array<int, 5> &effective_cost, int color, int gold_used,
      std::array<uint8_t, 5> gold_as, std::vector<OrderedAction> &actions) {
    if (color == 5) {
      OrderedAction action;
      action.rank = 0;
      action.neg_points = -static_cast<int>(card.points);
      action.oracle_card = card.id;
      action.oracle_gold_as = gold_as;
      actions.push_back(action);
      ++stats_.oracle_purchase_actions;
      return;
    }
    const int min_gold =
        std::max(0, effective_cost[color] - static_cast<int>(player.gems[color]));
    const int max_gold =
        std::min(effective_cost[color],
                 static_cast<int>(player.gems[GOLD]) - gold_used);
    for (int amount = min_gold; amount <= max_gold; ++amount) {
      gold_as[color] = amount;
      add_oracle_purchase_actions(player, card, effective_cost, color + 1,
                                  gold_used + amount, gold_as, actions);
    }
  }

  static bool apply_oracle_action(Game &game, const OrderedAction &ordered) {
    Board &board = game.board;
    if (board.is_game_over() || board.waiting_noble ||
        board.current_player >= Board::NUM_PLAYERS)
      return false;
    board.invalidate_hash();
    PlayerState &player = board.players[board.current_player];
    if (ordered.oracle_card >= 0) {
      const Card &card = get_card(ordered.oracle_card);
      if (!player.can_afford(card))
        return false;
      int gold_used = 0;
      for (int color = 0; color < 5; ++color) {
        const int cost =
            std::max(0, static_cast<int>(card.cost[color]) -
                            static_cast<int>(player.bonuses[color]));
        const int from_gold = ordered.oracle_gold_as[color];
        const int from_gems = cost - from_gold;
        if (from_gold > cost || from_gems > player.gems[color])
          return false;
        player.gems[color] -= from_gems;
        board.bank[color] += from_gems;
        gold_used += from_gold;
      }
      if (gold_used > player.gems[GOLD])
        return false;
      player.gems[GOLD] -= gold_used;
      board.bank[GOLD] += gold_used;
      player.purchased_cards.push_back(card.id);
      ++player.purchased_count;
      ++player.bonuses[card.bonus];
      player.points += card.points;
      player.sync_packed();
    } else if (ordered.oracle_reserve) {
      if (!player.can_reserve())
        return false;
      // The identity stays unknown, but the reserved card still occupies a
      // real slot and blocks further reserve actions once the limit is hit.
      player.reserved_is_hidden[player.reserved_count] = true;
      player.reserved[player.reserved_count++] = -1;
      if (board.bank[GOLD] > 0) {
        --board.bank[GOLD];
        ++player.gems[GOLD];
        if (ordered.oracle_return_color >= 0) {
          if (player.gems[ordered.oracle_return_color] == 0)
            return false;
          --player.gems[ordered.oracle_return_color];
          ++board.bank[ordered.oracle_return_color];
        }
      }
      player.sync_packed();
    } else {
      return false;
    }

    const auto eligible =
        MoveGenerator::get_eligible_nobles_fixed(board, board.current_player);
    if (eligible.size() > 1) {
      board.waiting_noble = true;
      return true;
    }
    if (eligible.size() == 1)
      acquire_noble(board, eligible[0]);
    end_oracle_turn(board);
    return true;
  }

  static void acquire_noble(Board &board, int noble_id) {
    PlayerState &player = board.players[board.current_player];
    player.points += get_noble(noble_id).points;
    player.acquired_nobles.push_back(noble_id);
    board.nobles.remove(noble_id);
  }

  static void end_oracle_turn(Board &board) {
    if (!board.final_round && board.players[board.current_player].points >= 15)
      board.final_round = true;
    board.current_player = 1 - board.current_player;
    if (board.current_player != 0)
      return;
    ++board.turn;
    if (!board.final_round)
      return;
    const int points0 = board.players[0].points;
    const int points1 = board.players[1].points;
    if (points0 != points1) {
      board.winner = points0 > points1 ? 0 : 1;
      return;
    }
    const int purchased0 = board.players[0].purchased_count;
    const int purchased1 = board.players[1].purchased_count;
    board.winner =
        purchased0 == purchased1 ? -2 : purchased0 < purchased1 ? 0 : 1;
  }

  static bool has_player_purchased_card(const PlayerState &player,
                                        int card_id) {
    return std::find(player.purchased_cards.begin(),
                     player.purchased_cards.end(),
                     static_cast<uint8_t>(card_id)) !=
           player.purchased_cards.end();
  }

  static bool has_hidden_card_acquired(const Board &board, int card_id) {
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      if (has_player_purchased_card(board.players[player], card_id))
        return true;
    }
    return false;
  }

  static bool has_hidden_card_claimed(const Board &board, int card_id) {
    if (has_hidden_card_acquired(board, card_id))
      return true;
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
        if (board.players[player].reserved[slot] == card_id)
          return true;
      }
    }
    return false;
  }

  std::vector<OrderedAction>
  forced_attacker_actions(const Game &game,
                          const std::vector<OrderedAction> &actions,
                          int depth, bool is_root = false) {
    if (is_root && required_root_action_ != UINT64_MAX) {
      std::vector<OrderedAction> required;
      std::copy_if(actions.begin(), actions.end(), std::back_inserter(required),
                   [&](const OrderedAction &action) {
                     return action.code == required_root_action_;
                   });
      return required;
    }

    std::vector<OrderedAction> purchases;
    std::vector<std::pair<int, OrderedAction>> takes;
    std::vector<OrderedAction> reserves;
    std::vector<OrderedAction> passthrough;
    for (const OrderedAction &ordered : actions) {
      const Action action = Action::unpack(ordered.code);
      switch (action.type) {
      case PURCHASE:
        if (!is_initial_known_card(action.card_id))
          break;
        purchases.push_back(ordered);
        break;
      case VISIT_NOBLE:
        purchases.push_back(ordered);
        break;
      case TAKE_DIFFERENT:
      case TAKE_SAME:
        takes.push_back({attacker_take_score(game, action), ordered});
        break;
      case RESERVE_VISIBLE:
        if (is_initial_known_card(action.card_id))
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
    prefer_candidate_action(filtered, depth);
    return filtered;
  }

  void prefer_candidate_action(std::vector<OrderedAction> &actions,
                               int depth) const {
    const int index = depth_ - depth;
    if (index < 0 ||
        index >= static_cast<int>(preferred_attacker_actions_.size()))
      return;
    const uint64_t preferred = preferred_attacker_actions_[index];
    const auto it =
        std::find_if(actions.begin(), actions.end(), [&](const auto &action) {
          return action.code == preferred;
        });
    if (it != actions.end())
      std::rotate(actions.begin(), it, it + 1);
  }

  void remember_initial_known_cards(const Game &game) {
    initial_known_low_ = 0;
    initial_known_high_ = 0;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        remember_initial_known_card(game.board.visible[level][slot]);
    }
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
        remember_initial_known_card(game.board.players[player].reserved[slot]);
    }
  }

  void remember_initial_known_card(int card_id) {
    if (!is_valid_card_id(card_id))
      return;
    if (card_id < 64)
      initial_known_low_ |= uint64_t{1} << card_id;
    else
      initial_known_high_ |= uint64_t{1} << (card_id - 64);
  }

  bool is_initial_known_card(int card_id) const {
    if (!is_valid_card_id(card_id))
      return false;
    if (card_id < 64)
      return (initial_known_low_ & (uint64_t{1} << card_id)) != 0;
    return (initial_known_high_ & (uint64_t{1} << (card_id - 64))) != 0;
  }

  void remember_initial_hidden_cards(const Game &game) {
    initial_hidden_low_ = 0;
    initial_hidden_high_ = 0;
    for (int level = 0; level < 3; ++level) {
      for (uint8_t card_id : game.board.decks[level]) {
        if (card_id < 64)
          initial_hidden_low_ |= uint64_t{1} << card_id;
        else
          initial_hidden_high_ |= uint64_t{1} << (card_id - 64);
      }
    }
  }

  bool is_initial_hidden_card(int card_id) const {
    if (!is_valid_card_id(card_id))
      return false;
    if (card_id < 64)
      return (initial_hidden_low_ & (uint64_t{1} << card_id)) != 0;
    return (initial_hidden_high_ & (uint64_t{1} << (card_id - 64))) != 0;
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
    return card.points * 1000 + (before_gap - after_gap) * 100 - after_gap * 10;
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

  StateKey state_key(const Game &game) const {
    uint64_t unseen_low = 0;
    uint64_t unseen_high = 0;
    for (int level = 0; level < 3; ++level) {
      for (uint8_t card_id : game.board.decks[level]) {
        if (card_id < 64)
          unseen_low |= uint64_t{1} << card_id;
        else
          unseen_high |= uint64_t{1} << (card_id - 64);
      }
    }
    uint64_t acquired_hidden_low = 0;
    uint64_t acquired_hidden_high = 0;
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      for (uint8_t card_id : game.board.players[player].purchased_cards) {
        if (!is_initial_hidden_card(card_id))
          continue;
        if (card_id < 64)
          acquired_hidden_low |= uint64_t{1} << card_id;
        else
          acquired_hidden_high |= uint64_t{1} << (card_id - 64);
      }
    }
    const Board &board = game.board;
    return StateKey{board.hash(),
                    unseen_low,
                    unseen_high,
                    acquired_hidden_low,
                    acquired_hidden_high,
                    board.players[0].points,
                    board.players[1].points,
                    board.players[0].purchased_count,
                    board.players[1].purchased_count,
                    board.players[0].reserved_count,
                    board.players[1].reserved_count,
                    board.final_round,
                    board.winner};
  }

  RevealVerifiedProofDag build_proof_dag() {
    RevealVerifiedProofDag dag;
    dag.requested = true;
    proof_node_ids_.clear();
    proof_nodes_.clear();
    proof_dag_edges_ = 0;
    const RevealVerifiedSearchStats search_stats = stats_;
    try {
      Game game = root_.clone_light();
      dag.root = build_proof_node(game, depth_);
      dag.complete = true;
      dag.nodes = std::move(proof_nodes_);
    } catch (const ProofDagBuildAborted &exc) {
      dag.omitted_reason = exc.what();
      proof_nodes_.clear();
      proof_node_ids_.clear();
    }
    stats_ = search_stats;
    return dag;
  }

  size_t build_proof_node(Game &game, int depth) {
    const DepthStateKey key{state_key(game), depth};
    auto known = proof_node_ids_.find(key);
    if (known != proof_node_ids_.end())
      return known->second;
    if (proof_dag_node_limit_ && proof_nodes_.size() >= proof_dag_node_limit_)
      throw ProofDagBuildAborted("proof DAG node limit exceeded");

    const size_t id = proof_nodes_.size();
    proof_node_ids_[key] = id;
    proof_nodes_.push_back(
        RevealVerifiedProofNode{id, game.current_player(), depth});
    if (game.is_game_over()) {
      proof_nodes_[id].kind = "terminal";
      proof_nodes_[id].resolution =
          game.winner() == attacker_ ? "attacker_win" : "non_attacker_win";
      return id;
    }

    auto memo_it = memo_.find(key);
    if (memo_it == memo_.end() || memo_it->second.status != ForceStatus::PROVEN) {
      throw ProofDagBuildAborted("proof DAG references unmaterialized subtree");
    }
    const Entry &entry = memo_it->second;
    if (!entry.has_action) {
      proof_nodes_[id].kind = "resolved";
      proof_nodes_[id].resolution = "final_round_proof_summary";
      return id;
    }

    std::vector<OrderedAction> actions = ordered_actions(game);
    if (game.current_player() == attacker_) {
      actions = forced_attacker_actions(game, actions, depth, id == 0);
      actions.erase(
          std::remove_if(actions.begin(), actions.end(),
                         [&](const OrderedAction &ordered) {
                           return ordered.code != entry.action_code;
                         }),
          actions.end());
    }
    const int current_player = game.current_player();
    for (const OrderedAction &ordered : actions) {
      for_each_outcome(game, ordered, [&](int reveal_card) {
        const int next_depth =
            depth - static_cast<int>(current_player == attacker_ &&
                                     game.current_player() != current_player);
        const size_t child = build_proof_node(game, next_depth);
        if (proof_dag_edge_limit_ && proof_dag_edges_ >= proof_dag_edge_limit_)
          throw ProofDagBuildAborted("proof DAG edge limit exceeded");
        proof_nodes_[id].children.push_back(RevealVerifiedProofEdge{
            ordered.code, reveal_card, ordered.oracle_card,
            ordered.oracle_reserve, ordered.oracle_return_color,
            ordered.oracle_gold_as, child});
        ++proof_dag_edges_;
        return true;
      });
    }
    return id;
  }

  std::vector<RevealVerifiedLineEntry> principal_line() const {
    Game game = root_.clone_light();
    std::vector<RevealVerifiedLineEntry> line;
    std::unordered_set<DepthStateKey, DepthStateKeyHash> seen;
    int depth = depth_;
    for (int ply = 0; ply < 200 && depth >= 0; ++ply) {
      if (game.is_game_over())
        break;
      const DepthStateKey key{state_key(game), depth};
      if (seen.find(key) != seen.end())
        break;
      seen.insert(key);
      auto it = memo_.find(key);
      if (it == memo_.end() || !it->second.has_action ||
          !it->second.replayable)
        break;
      const int current_player = game.current_player();
      const Action action = Action::unpack(it->second.action_code);
      const int reveal_card = it->second.reveal_card;
      const bool applied =
          action.type == RESERVE_DECK && reveal_card >= 0
              ? apply_deck_reserve_outcome(game, action, reveal_card)
              : game.apply_action_code_trusted(it->second.action_code, false);
      if (!applied)
        break;
      line.push_back(RevealVerifiedLineEntry{
          it->second.action_code, reveal_card, it->second.action_count});
      depth -= static_cast<int>(current_player == attacker_ &&
                                game.current_player() != current_player);
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

#endif // CSPLENDOR_REVEAL_VERIFIED_SOLVER_H
