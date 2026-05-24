#ifndef CSPLENDOR_GAME_H
#define CSPLENDOR_GAME_H

#include "action.h"
#include "board.h"
#include "move_generator.h"
#include <algorithm>
#include <array>
#include <memory>
#include <vector>

class Game {
  struct NoInit {};

public:
  Board board;
  std::vector<Action> history;
  std::vector<Board> board_history; // Simple undo support
  bool simple_payment_mode = false; // When true, only generate minimal gold payment patterns
  bool blank_refill_mode = false;   // When true, deck refill consumes top card but keeps board slot blank

  explicit Game(uint64_t seed = 0) { board.init(seed); }

  Game clone() const {
    Game g = *this;
    return g;
  }

  // Lightweight clone for MCTS - does not copy history to save memory
  Game clone_light() const {
    Game g(NoInit{});
    g.board = board;
    g.simple_payment_mode = simple_payment_mode;
    g.blank_refill_mode = blank_refill_mode;
    // Intentionally not copying history and board_history
    // This is safe for MCTS where we only need the current state
    return g;
  }

  // Shuffled clone for MCTS determinization - randomizes hidden information
  // from the perspective of observer_player to combat "clairvoyance"
  Game shuffled_clone(uint8_t observer_player, uint64_t seed) const {
    Game g(NoInit{});
    g.board = board;
    g.simple_payment_mode = simple_payment_mode;
    g.blank_refill_mode = blank_refill_mode;
    g.board.randomize_hidden_information(observer_player, seed);
    return g;
  }

  bool apply(const Action &action, bool record_history = true) {
    if (!can_apply(action))
      return false;
    return apply_unchecked(action, record_history);
  }

  bool apply_trusted(const Action &action, bool record_history = false) {
    if (!valid_current_player() || board.is_game_over())
      return false;
    return apply_unchecked(action, record_history);
  }

  uint16_t legal_action_count() const {
    return MoveGenerator::count_all_fixed(board, simple_payment_mode);
  }

  std::vector<uint64_t> legal_action_codes() const {
    MoveList fixed = MoveGenerator::generate_all_fixed(board, simple_payment_mode);
    std::vector<uint64_t> codes;
    codes.reserve(fixed.size());
    for (const Action &action : fixed)
      codes.push_back(action.pack());
    return codes;
  }

  uint64_t legal_action_code_at(uint16_t index) const {
    MoveList fixed = MoveGenerator::generate_all_fixed(board, simple_payment_mode);
    if (index >= fixed.size())
      return 0;
    return fixed[index].pack();
  }

  bool apply_action_code(uint64_t code, bool record_history = true) {
    return apply(Action::unpack(code), record_history);
  }

  bool apply_action_code_trusted(uint64_t code, bool record_history = false) {
    return apply_trusted(Action::unpack(code), record_history);
  }

  bool apply_legal_action_index(uint16_t index, bool record_history = false) {
    MoveList fixed = MoveGenerator::generate_all_fixed(board, simple_payment_mode);
    if (index >= fixed.size())
      return false;
    return apply_trusted(fixed[index], record_history);
  }

  bool apply_random_action(uint64_t random_value, bool record_history = false) {
    MoveList fixed = MoveGenerator::generate_all_fixed(board, simple_payment_mode);
    if (fixed.empty())
      return false;
    return apply_trusted(fixed[random_value % fixed.size()], record_history);
  }

  bool undo() {
    if (board_history.empty())
      return false;
    board = board_history.back();
    board_history.pop_back();
    if (!history.empty())
      history.pop_back();
    return true;
  }

  bool is_legal(const Action &action) const {
    MoveList legals =
        MoveGenerator::generate_all_fixed(board, simple_payment_mode);
    for (const auto &l : legals) {
      if (l == action)
        return true;
    }
    return false;
  }

  std::vector<Action> legal_actions() const {
    return MoveGenerator::generate_all(board, simple_payment_mode);
  }

  std::vector<Action> base_actions() const {
    return MoveGenerator::generate_base(board, simple_payment_mode);
  }

  void set_simple_payment_mode(bool mode) { simple_payment_mode = mode; }
  bool get_simple_payment_mode() const { return simple_payment_mode; }
  void set_blank_refill_mode(bool mode) { blank_refill_mode = mode; }
  bool get_blank_refill_mode() const { return blank_refill_mode; }

  std::array<int, 2> scores() const {
    return {(int)board.players[0].points, (int)board.players[1].points};
  }

  bool is_game_over() const { return board.winner != -1; }
  int winner() const { return board.winner; }
  int current_player() const { return board.current_player; }
  int turn() const { return board.turn; }

private:
  explicit Game(NoInit) {}

  bool apply_unchecked(const Action &action, bool record_history) {
    Board previous;
    if (record_history)
      previous = board;

    // Invalidate hash since state will change
    board.invalidate_hash();

    bool applied = false;
    switch (action.type) {
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      applied = apply_take_gems(action);
      break;
    case RESERVE_VISIBLE:
      applied = apply_reserve_visible(action);
      break;
    case RESERVE_DECK:
      applied = apply_reserve_deck(action);
      break;
    case PURCHASE:
      applied = apply_purchase(action);
      break;
    case VISIT_NOBLE:
      applied = apply_noble_visit(action);
      if (!applied)
        return false;
      board.waiting_noble = false;
      end_turn();
      if (record_history) {
        board_history.push_back(previous);
        history.push_back(action);
      }
      return true;
    default:
      return false;
    }

    if (!applied)
      return false;

    // Standard turn processing (Take Gems, Reserve, Purchase)
    // After standard action, check if noble visits are triggered.
    auto eligible =
        MoveGenerator::get_eligible_nobles_fixed(board, board.current_player);

    if (eligible.size() > 1) {
      // Multiple nobles: wait for manual selection.
      board.waiting_noble = true;
    } else {
      // 1 or 0 nobles: automatic application or none.
      if (eligible.size() == 1) {
        if (!apply_noble_visit(action))
          return false;
      }
      end_turn();
    }

    if (record_history) {
      board_history.push_back(previous);
      history.push_back(action);
    }
    return true;
  }

  bool valid_current_player() const {
    return board.current_player < Board::NUM_PLAYERS;
  }

  bool has_no_return(const Action &a) const {
    for (int i = 0; i < 6; ++i) {
      if (a.return_gems[i] != 0)
        return false;
    }
    return true;
  }

  bool validate_returns(const std::array<uint8_t, 6> &next_gems,
                        const Action &a) const {
    int total = 0;
    int returned = 0;
    for (int i = 0; i < 6; ++i) {
      if (a.return_gems[i] > next_gems[i])
        return false;
      total += next_gems[i];
      returned += a.return_gems[i];
    }

    int excess = std::max(0, total - Board::MAX_TOKENS);
    return returned == excess;
  }

  bool can_apply_take(const Action &a) const {
    if (!valid_current_player())
      return false;

    const auto &p = board.players[board.current_player];
    std::array<uint8_t, 6> next_gems = p.gems;
    int taken = 0;
    int available_colors = 0;
    int same_color = -1;

    for (int i = 0; i < 5; ++i) {
      if (board.bank[i] > 0)
        available_colors++;
      if (a.take[i] > board.bank[i])
        return false;
      if (a.type == TAKE_DIFFERENT && a.take[i] > 1)
        return false;
      if (a.type == TAKE_SAME && a.take[i] != 0 && a.take[i] != 2)
        return false;
      if (a.take[i] == 2)
        same_color = i;
      next_gems[i] += a.take[i];
      taken += a.take[i];
    }

    if (a.type == TAKE_DIFFERENT) {
      if (taken <= 0 || taken > 3)
        return false;
      if (available_colors >= 3 && taken != 3)
        return false;
      if (available_colors < 3 && taken != available_colors)
        return false;
    } else if (taken != 2) {
      return false;
    } else if (same_color < 0 || board.bank[same_color] < 4) {
      return false;
    }

    return validate_returns(next_gems, a);
  }

  bool can_apply_reserve_visible(const Action &a) const {
    if (!valid_current_player() || !is_valid_card_id(a.card_id))
      return false;

    const auto &p = board.players[board.current_player];
    if (!p.can_reserve())
      return false;

    bool found = false;
    for (int l = 0; l < 3 && !found; ++l)
      for (int s = 0; s < 4; ++s)
        found = found || board.visible[l][s] == a.card_id;
    if (!found)
      return false;

    std::array<uint8_t, 6> next_gems = p.gems;
    if (board.bank[GOLD] > 0)
      next_gems[GOLD]++;
    return validate_returns(next_gems, a);
  }

  bool can_apply_reserve_deck(const Action &a) const {
    if (!valid_current_player() || a.deck_level < 0 || a.deck_level >= 3)
      return false;

    const auto &p = board.players[board.current_player];
    if (!p.can_reserve() || board.decks[a.deck_level].empty())
      return false;

    std::array<uint8_t, 6> next_gems = p.gems;
    if (board.bank[GOLD] > 0)
      next_gems[GOLD]++;
    return validate_returns(next_gems, a);
  }

  bool can_apply_purchase(const Action &a) const {
    if (!valid_current_player() || !is_valid_card_id(a.card_id) ||
        !has_no_return(a))
      return false;

    const auto &p = board.players[board.current_player];

    bool source_found = false;
    if (a.from_reserved) {
      for (int i = 0; i < 3; ++i)
        source_found = source_found || p.reserved[i] == a.card_id;
    } else {
      for (int l = 0; l < 3 && !source_found; ++l)
        for (int s = 0; s < 4; ++s)
          source_found = source_found || board.visible[l][s] == a.card_id;
    }
    if (!source_found)
      return false;

    const auto &card = get_card(a.card_id);
    int gold_used = 0;
    for (int i = 0; i < 5; ++i) {
      int cost = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
      int from_gold = a.gold_as[i];
      if (from_gold > cost)
        return false;
      int from_gems = cost - from_gold;
      if (from_gems > p.gems[i])
        return false;
      gold_used += from_gold;
    }
    return gold_used <= p.gems[GOLD];
  }

  bool can_apply_noble_visit(const Action &a) const {
    if (!valid_current_player() || a.type != VISIT_NOBLE ||
        !is_valid_noble_id(a.noble_choice))
      return false;

    auto eligible =
        MoveGenerator::get_eligible_nobles_fixed(board, board.current_player);
    for (uint8_t noble_id : eligible) {
      if (noble_id == a.noble_choice)
        return true;
    }
    return false;
  }

  bool can_apply(const Action &a) const {
    if (!valid_current_player() || board.is_game_over())
      return false;

    if (board.waiting_noble)
      return can_apply_noble_visit(a);

    if (a.type == VISIT_NOBLE)
      return false;

    switch (a.type) {
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      return can_apply_take(a);
    case RESERVE_VISIBLE:
      return can_apply_reserve_visible(a);
    case RESERVE_DECK:
      return can_apply_reserve_deck(a);
    case PURCHASE:
      return can_apply_purchase(a);
    default:
      return false;
    }
  }

  bool apply_take_gems(const Action &a) {
    auto &p = board.players[board.current_player];
    for (int i = 0; i < 5; ++i) {
      p.gems[i] += a.take[i];
      board.bank[i] -= a.take[i];
    }
    p.sync_packed();
    apply_gem_return(a);
    return true;
  }

  bool apply_reserve_visible(const Action &a) {
    auto &p = board.players[board.current_player];

    // Find and remove from board
    int found_level = -1;
    int found_slot = -1;
    for (int l = 0; l < 3 && found_level == -1; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (board.visible[l][s] == a.card_id) {
          found_level = l;
          found_slot = s;
          break;
        }
      }
    }
    if (found_level == -1)
      return false;

    p.reserved_is_hidden[p.reserved_count] = false;
    p.reserved[p.reserved_count++] = a.card_id;
    if (!board.decks[found_level].empty()) {
      // Blank refill mode: consume the card but keep the slot unknown.
      if (blank_refill_mode) {
        board.decks[found_level].pop_back();
        board.visible[found_level][found_slot] = -1;
      } else {
        board.visible[found_level][found_slot] = board.decks[found_level].back();
        board.decks[found_level].pop_back();
      }
    } else {
      board.visible[found_level][found_slot] = -1;
    }

    if (board.bank[GOLD] > 0) {
      p.gems[GOLD]++;
      board.bank[GOLD]--;
    }
    p.sync_packed();
    apply_gem_return(a);
    return true;
  }

  bool apply_reserve_deck(const Action &a) {
    if (a.deck_level < 0 || a.deck_level >= 3 ||
        board.decks[a.deck_level].empty())
      return false;

    auto &p = board.players[board.current_player];
    uint8_t card_id = board.decks[a.deck_level].back();
    board.decks[a.deck_level].pop_back();
    p.reserved_is_hidden[p.reserved_count] = true;
    p.reserved[p.reserved_count++] = card_id;

    if (board.bank[GOLD] > 0) {
      p.gems[GOLD]++;
      board.bank[GOLD]--;
    }
    apply_gem_return(a);
    return true;
  }

  bool apply_purchase(const Action &a) {
    auto &p = board.players[board.current_player];
    const auto &card = get_card(a.card_id);

    int reserved_slot = -1;
    int visible_level = -1;
    int visible_slot = -1;
    if (a.from_reserved) {
      for (int i = 0; i < 3; ++i) {
        if (p.reserved[i] == a.card_id) {
          reserved_slot = i;
          break;
        }
      }
      if (reserved_slot == -1)
        return false;
    } else {
      for (int l = 0; l < 3 && visible_level == -1; ++l) {
        for (int s = 0; s < 4; ++s) {
          if (board.visible[l][s] == a.card_id) {
            visible_level = l;
            visible_slot = s;
            break;
          }
        }
      }
      if (visible_level == -1)
        return false;
    }

    // Payment using action.gold_as to determine gold usage
    int gold_used = 0;
    for (int i = 0; i < 5; ++i) {
      int cost = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
      // Use gold_as[i] gold tokens for this color
      int from_gold = a.gold_as[i];
      int from_gems = cost - from_gold;
      p.gems[i] -= from_gems;
      board.bank[i] += from_gems;
      gold_used += from_gold;
    }
    p.gems[GOLD] -= gold_used;
    board.bank[GOLD] += gold_used;

    // Gain card
    p.purchased_cards.push_back(a.card_id);
    p.purchased_count++;
    p.bonuses[card.bonus]++;
    p.points += card.points;
    p.sync_packed();

    // Remove from source
    if (a.from_reserved) {
      for (int j = reserved_slot; j < 2; ++j) {
        p.reserved[j] = p.reserved[j + 1];
        p.reserved_is_hidden[j] = p.reserved_is_hidden[j + 1];
      }
      p.reserved[2] = -1;
      p.reserved_is_hidden[2] = false;
      p.reserved_count--;
    } else {
      if (!board.decks[visible_level].empty()) {
        // Blank refill mode: consume the card but keep the slot unknown.
        if (blank_refill_mode) {
          board.decks[visible_level].pop_back();
          board.visible[visible_level][visible_slot] = -1;
        } else {
          board.visible[visible_level][visible_slot] =
              board.decks[visible_level].back();
          board.decks[visible_level].pop_back();
        }
      } else {
        board.visible[visible_level][visible_slot] = -1;
      }
    }
    return true;
  }

  void apply_gem_return(const Action &a) {
    auto &p = board.players[board.current_player];
    for (int i = 0; i < 6; ++i) {
      p.gems[i] -= a.return_gems[i];
      board.bank[i] += a.return_gems[i];
    }
    p.sync_packed();
  }

  bool apply_noble_visit(const Action &a) {
    auto &p = board.players[board.current_player];
    auto eligible =
        MoveGenerator::get_eligible_nobles_fixed(board, board.current_player);
    if (eligible.empty())
      return a.type != VISIT_NOBLE;

    uint8_t noble_id;
    if (a.type == VISIT_NOBLE) {
      bool found = false;
      for (uint8_t eligible_id : eligible) {
        if (eligible_id == a.noble_choice) {
          found = true;
          break;
        }
      }
      if (!found)
        return false;
      noble_id = static_cast<uint8_t>(a.noble_choice);
    } else if (eligible.size() == 1) {
      noble_id = eligible[0];
    } else {
      // This should not be hit if logic in apply() is correct
      noble_id = eligible[0];
    }

    p.points += get_noble(noble_id).points;
    p.acquired_nobles.push_back(noble_id); // Track acquired noble
    board.nobles.remove(noble_id); // Uses FixedStack::remove
    return true;
  }

  void end_turn() {
    if (!board.final_round &&
        board.players[board.current_player].points >= 15) {
      board.final_round = true;
    }

    board.current_player = 1 - board.current_player;

    if (board.current_player == 0) {
      board.turn++;
      if (board.final_round) {
        check_game_end();
      }
    }
  }

  void check_game_end() {
    int p0 = board.players[0].points;
    int p1 = board.players[1].points;

    if (p0 > p1) {
      board.winner = 0;
    } else if (p1 > p0) {
      board.winner = 1;
    } else {
      int c0 = board.players[0].purchased_count;
      int c1 = board.players[1].purchased_count;
      if (c0 < c1)
        board.winner = 0;
      else if (c1 < c0)
        board.winner = 1;
      else
        board.winner = -2; // Draw
    }
  }
};

#endif // CSPLENDOR_GAME_H
