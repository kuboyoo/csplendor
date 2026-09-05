#ifndef CSPLENDOR_GAME_H
#define CSPLENDOR_GAME_H

#include "action.h"
#include "board.h"
#include "move_generator.h"
#include "rule_query.h"
#include "rule_transition.h"
#ifdef CSPLENDOR_VERIFY_DELTA_UNDO
#include "undo_record.h"
#include <cassert>
#endif
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
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

  // Full user-visible copy, including the action and undo journals.
  Game clone() const { return *this; }

  // Current-state copy for search. Action and undo journals are excluded.
  Game clone_light() const {
    CSPLENDOR_PERF_INC(CloneLightCalls);
    return copy_current_state();
  }

  // Shuffled clone for MCTS determinization - randomizes hidden information
  // from the perspective of observer_player to combat "clairvoyance"
  Game shuffled_clone(uint8_t observer_player, uint64_t seed) const {
    CSPLENDOR_PERF_INC(DeterminizationCloneCalls);
    Game g = copy_current_state();
    g.board.randomize_hidden_information(observer_player, seed);
    return g;
  }

  Game shuffled_clone_portable(uint8_t observer_player, uint64_t seed) const {
    CSPLENDOR_PERF_INC(DeterminizationCloneCalls);
    Game g = copy_current_state();
    g.board.randomize_hidden_information_portable(observer_player, seed);
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
#ifdef CSPLENDOR_SINGLE_PASS_LEGAL_CODES
    std::array<uint64_t, MAX_MOVES> scratch;
    uint16_t count = 0;
    if (use_packed_code_sink()) {
      auto code_sink = [&scratch, &count](uint64_t code) {
        scratch[count++] = code;
        return true;
      };
      MoveGenerator::consume_all_codes_capped(board, simple_payment_mode,
                                              code_sink);
      return std::vector<uint64_t>(scratch.begin(), scratch.begin() + count);
    }
    auto sink = [&scratch, &count](const Action &action) {
      scratch[count++] = action.pack();
      return true;
    };
    MoveGenerator::consume_all_capped(board, simple_payment_mode, sink);
    return std::vector<uint64_t>(scratch.begin(), scratch.begin() + count);
#else
    std::vector<uint64_t> codes;
    codes.reserve(MoveGenerator::count_all_fixed(board, simple_payment_mode));
    if (use_packed_code_sink()) {
      auto code_sink = [&codes](uint64_t code) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
        const size_t capacity = codes.capacity();
#endif
        codes.push_back(code);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
        if (codes.capacity() != capacity)
          CSPLENDOR_PERF_INC(ActionVectorReallocations);
#endif
        return true;
      };
      MoveGenerator::consume_all_codes_capped(board, simple_payment_mode,
                                              code_sink);
      return codes;
    }
    auto sink = [&codes](const Action &action) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      const size_t capacity = codes.capacity();
#endif
      codes.push_back(action.pack());
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      if (codes.capacity() != capacity)
        CSPLENDOR_PERF_INC(ActionVectorReallocations);
#endif
      return true;
    };
    MoveGenerator::consume_all_capped(board, simple_payment_mode, sink);
    return codes;
#endif
  }

  uint64_t legal_action_code_at(uint16_t index) const {
    uint16_t current = 0;
    uint64_t code = 0;
    if (use_packed_code_sink()) {
      auto code_sink = [&current, &code, index](uint64_t candidate) {
        if (current++ != index)
          return true;
        code = candidate;
        return false;
      };
      MoveGenerator::consume_all_codes_capped(board, simple_payment_mode,
                                              code_sink);
      return code;
    }
    auto sink = [&current, &code, index](const Action &action) {
      if (current++ != index)
        return true;
      code = action.pack();
      return false;
    };
    MoveGenerator::consume_all_capped(board, simple_payment_mode, sink);
    return code;
  }

  bool apply_action_code(uint64_t code, bool record_history = true) {
    return apply(Action::unpack(code), record_history);
  }

  bool apply_action_code_trusted(uint64_t code, bool record_history = false) {
    return apply_trusted(Action::unpack(code), record_history);
  }

  bool apply_legal_action_index(uint16_t index, bool record_history = false) {
    uint16_t current = 0;
    Action selected;
    bool found = false;
    auto sink = [&current, &selected, &found, index](const Action &action) {
      if (current++ != index)
        return true;
      selected = action;
      found = true;
      return false;
    };
    MoveGenerator::consume_all_capped(board, simple_payment_mode, sink);
    if (!found)
      return false;
    return apply_trusted(selected, record_history);
  }

  bool apply_random_action(uint64_t random_value, bool record_history = false) {
    uint16_t count = MoveGenerator::count_all_fixed(board, simple_payment_mode);
    if (count == 0)
      return false;

    uint16_t target = static_cast<uint16_t>(random_value % count);
    uint16_t current = 0;
    Action selected;
    bool found = false;
    auto sink = [&current, &selected, &found, target](const Action &action) {
      if (current++ != target)
        return true;
      selected = action;
      found = true;
      return false;
    };
    MoveGenerator::consume_all_capped(board, simple_payment_mode, sink);
    return found && apply_trusted(selected, record_history);
  }

  bool requires_forced_pass() const {
    return MoveGenerator::requires_forced_pass(board, simple_payment_mode);
  }

  bool apply_forced_pass(bool record_history = true) {
    Action pass;
    pass.type = PASS;
    return apply(pass, record_history);
  }

  bool undo() {
    if (board_history.empty())
      return false;
    board = std::move(board_history.back());
    board_history.pop_back();
    if (!history.empty())
      history.pop_back();
    return true;
  }

  bool is_legal(const Action &action) const {
    // apply() uses semantic validation: fields irrelevant to an action type
    // do not change its meaning. Keep this query on that exact contract.
    return can_apply(action);
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

  bool use_packed_code_sink() const noexcept {
    if (!csplendor::move_generation_detail::packed_code_sink_enabled ||
        board.current_player >= Board::NUM_PLAYERS)
      return false;

    // Saturated reserve slots remove up to fifteen cheap base actions and are
    // common in low-mobility late positions. The legacy Action sink has less
    // fixed overhead there; all rule constraints and output order remain in
    // the shared generator implementation.
    return board.players[board.current_player].reserved_count <
           Board::MAX_RESERVED;
  }

  Game copy_current_state() const {
    Game copy(NoInit{});
    CSPLENDOR_PERF_INC(BoardSnapshotCopies);
    copy.board = board;
    copy.simple_payment_mode = simple_payment_mode;
    copy.blank_refill_mode = blank_refill_mode;
    return copy;
  }

  bool apply_unchecked(const Action &action, bool record_history) {
#ifdef CSPLENDOR_INCREMENTAL_EXACT_HASH
    if (board.hash_valid)
      return apply_unchecked_with_mutator<true>(action, record_history);
#endif
    return apply_unchecked_with_mutator<false>(action, record_history);
  }

  template <bool MaintainExactHash>
  bool apply_unchecked_with_mutator(const Action &action, bool record_history) {
    Board previous;
    if (record_history) {
      CSPLENDOR_PERF_INC(BoardSnapshotCopies);
      previous = board;
    }
#ifdef CSPLENDOR_VERIFY_DELTA_UNDO
    csplendor::detail::UndoRecord delta_previous;
    if (record_history)
      delta_previous = csplendor::detail::UndoRecord::capture(board);
#endif

    // A pass is built on a copy because its stalemate probe may reject an
    // arbitrary editor state.  Prime the copy before opening its transaction
    // so an already-valid exact hash remains available for delta maintenance.
    if (action.type == PASS) {
      CSPLENDOR_PERF_INC(BoardSnapshotCopies);
      Board next = board;
      Board::RuleMutator<MaintainExactHash> mutation(next);
      csplendor::detail::end_turn(next, mutation);
      // If neither player can act, no future state change is possible.
      // Resolve the stalemate as a draw instead of allowing an infinite pass
      // cycle. A final-round result produced by end_turn() takes precedence.
      if (!next.is_game_over() &&
          MoveGenerator::requires_forced_pass(next, simple_payment_mode))
        mutation.set_winner(-2);
      mutation.commit();
      board = std::move(next);
      if (record_history) {
#ifdef CSPLENDOR_VERIFY_DELTA_UNDO
        assert(delta_previous.restores_snapshot(board, previous));
#endif
        board_history.push_back(std::move(previous));
        history.push_back(action);
      }
      return true;
    }

    // Enter the successful-publication mutation boundary once for the whole
    // action. An early return leaves the exact cache invalid.
    Board::RuleMutator<MaintainExactHash> mutation(board);

    bool applied = false;
    switch (action.type) {
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      applied = apply_take_gems(action, mutation);
      break;
    case RESERVE_VISIBLE:
      applied = apply_reserve_visible(action, mutation);
      break;
    case RESERVE_DECK:
      applied = apply_reserve_deck(action, mutation);
      break;
    case PURCHASE:
      applied = apply_purchase(action, mutation);
      break;
    case VISIT_NOBLE:
      applied = apply_noble_visit(action, mutation);
      if (!applied)
        return false;
      mutation.set_waiting_noble(false);
      csplendor::detail::end_turn(board, mutation);
      mutation.commit();
      if (record_history) {
#ifdef CSPLENDOR_VERIFY_DELTA_UNDO
        assert(delta_previous.restores_snapshot(board, previous));
#endif
        board_history.push_back(std::move(previous));
        history.push_back(action);
      }
      return true;
    case PASS:
      return false;
    default:
      return false;
    }

    if (!applied)
      return false;

    // Standard turn processing (Take Gems, Reserve, Purchase), including
    // automatic or deferred noble visits.
    csplendor::detail::finish_standard_action(board, mutation);
    mutation.commit();

    if (record_history) {
#ifdef CSPLENDOR_VERIFY_DELTA_UNDO
      assert(delta_previous.restores_snapshot(board, previous));
#endif
      board_history.push_back(std::move(previous));
      history.push_back(action);
    }
    return true;
  }

  bool valid_current_player() const {
    return board.current_player < Board::NUM_PLAYERS;
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

    return csplendor::rules::validate_token_return(next_gems, a.return_gems);
  }

  bool can_apply_reserve_visible(const Action &a) const {
    if (!valid_current_player() || !is_valid_card_id(a.card_id))
      return false;

    const auto &p = board.players[board.current_player];
    if (!p.can_reserve())
      return false;

    if (!csplendor::rules::find_visible_card_source(board, a.card_id))
      return false;

    std::array<uint8_t, 6> next_gems = p.gems;
    if (board.bank[GOLD] > 0)
      next_gems[GOLD]++;
    return csplendor::rules::validate_token_return(next_gems, a.return_gems);
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
    return csplendor::rules::validate_token_return(next_gems, a.return_gems);
  }

  bool can_apply_purchase(const Action &a) const {
    if (!valid_current_player() || !is_valid_card_id(a.card_id) ||
        !csplendor::rules::has_no_token_return(a))
      return false;

    const auto &p = board.players[board.current_player];

    const bool source_found =
        a.from_reserved
            ? csplendor::rules::find_reserved_card_source(p, a.card_id) >= 0
            : static_cast<bool>(
                  csplendor::rules::find_visible_card_source(board, a.card_id));
    if (!source_found)
      return false;

    return csplendor::rules::validate_purchase_payment(
        p, get_card(a.card_id), a.gold_as);
  }

  bool can_apply_noble_visit(const Action &a) const {
    if (!valid_current_player() || a.type != VISIT_NOBLE ||
        !is_valid_noble_id(a.noble_choice))
      return false;

    auto eligible =
        csplendor::rules::eligible_nobles(board, board.current_player);
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
    case PASS:
      return MoveGenerator::requires_forced_pass(board, simple_payment_mode);
    default:
      return false;
    }
  }

  template <typename Mutator>
  bool apply_take_gems(const Action &a, Mutator &mutation) {
    const int player_index = board.current_player;
    auto &p = board.players[player_index];
    for (int i = 0; i < 5; ++i) {
      mutation.set_player_gem(player_index, i,
                              static_cast<uint8_t>(p.gems[i] + a.take[i]));
      mutation.set_bank(i, static_cast<uint8_t>(board.bank[i] - a.take[i]));
    }
    apply_gem_return(a, mutation);
    return true;
  }

  template <typename Mutator>
  bool apply_reserve_visible(const Action &a, Mutator &mutation) {
    // Find and remove from board
    const auto source =
        csplendor::rules::find_visible_card_source(board, a.card_id);
    if (!source)
      return false;
    const int found_level = source.level;
    const int found_slot = source.slot;

    csplendor::detail::reserve_card_unchecked(board, mutation, a.card_id,
                                              false);
    if (!board.decks[found_level].empty()) {
      // Blank refill mode: consume the card but keep the slot unknown.
      if (blank_refill_mode) {
        mutation.pop_deck(found_level);
        mutation.set_visible(found_level, found_slot, -1);
      } else {
        const uint8_t refill = mutation.pop_deck(found_level);
        mutation.set_visible(found_level, found_slot,
                             static_cast<int8_t>(refill));
      }
    } else {
      mutation.set_visible(found_level, found_slot, -1);
    }

    csplendor::detail::grant_reserve_gold(board, mutation);
    CSPLENDOR_ROLLBACK_FAULT(SourceRemoval);
    apply_gem_return(a, mutation);
    return true;
  }

  template <typename Mutator>
  bool apply_reserve_deck(const Action &a, Mutator &mutation) {
    if (a.deck_level < 0 || a.deck_level >= 3 ||
        board.decks[a.deck_level].empty())
      return false;

    const uint8_t card_id = mutation.pop_deck(a.deck_level);
    csplendor::detail::reserve_card_unchecked(board, mutation, card_id, true);
    CSPLENDOR_ROLLBACK_FAULT(SourceRemoval);
    csplendor::detail::grant_reserve_gold(board, mutation);
    apply_gem_return(a, mutation);
    return true;
  }

  template <typename Mutator>
  bool apply_purchase(const Action &a, Mutator &mutation) {
    auto &p = board.players[board.current_player];
    const auto &card = get_card(a.card_id);

    int reserved_slot = -1;
    csplendor::rules::VisibleCardSource visible_source;
    if (a.from_reserved) {
      reserved_slot =
          csplendor::rules::find_reserved_card_source(p, a.card_id);
      if (reserved_slot == -1)
        return false;
    } else {
      visible_source =
          csplendor::rules::find_visible_card_source(board, a.card_id);
      if (!visible_source)
        return false;
    }

    // Payment and card gains are common to visible, reserved, and oracle
    // purchases. Validation remains at the caller boundary for trusted moves.
    csplendor::detail::purchase_card<false>(board, mutation, card, a.gold_as);

    // Remove from source
    if (a.from_reserved) {
      for (int j = reserved_slot; j < 2; ++j) {
        mutation.set_player_reserved(board.current_player, j,
                                     p.reserved[j + 1]);
        mutation.set_player_reserved_hidden(board.current_player, j,
                                            p.reserved_is_hidden[j + 1]);
      }
      mutation.set_player_reserved(board.current_player, 2, -1);
      mutation.set_player_reserved_hidden(board.current_player, 2, false);
      mutation.set_player_reserved_count(
          board.current_player, static_cast<uint8_t>(p.reserved_count - 1));
    } else {
      const int visible_level = visible_source.level;
      const int visible_slot = visible_source.slot;
      if (!board.decks[visible_level].empty()) {
        // Blank refill mode: consume the card but keep the slot unknown.
        if (blank_refill_mode) {
          mutation.pop_deck(visible_level);
          mutation.set_visible(visible_level, visible_slot, -1);
        } else {
          const uint8_t refill = mutation.pop_deck(visible_level);
          mutation.set_visible(visible_level, visible_slot,
                               static_cast<int8_t>(refill));
        }
      } else {
        mutation.set_visible(visible_level, visible_slot, -1);
      }
    }
    CSPLENDOR_ROLLBACK_FAULT(SourceRemoval);
    return true;
  }

  template <typename Mutator>
  void apply_gem_return(const Action &a, Mutator &mutation) {
    csplendor::detail::return_gems_unchecked(board, mutation, a.return_gems);
  }

  template <typename Mutator>
  bool apply_noble_visit(const Action &a, Mutator &mutation) {
    auto eligible =
        csplendor::rules::eligible_nobles(board, board.current_player);
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

    csplendor::detail::acquire_noble_unchecked(board, mutation, noble_id);
    return true;
  }
};

#endif // CSPLENDOR_GAME_H
