#ifndef CSPLENDOR_MOVE_GENERATOR_H
#define CSPLENDOR_MOVE_GENERATOR_H

#include "action.h"
#include "board.h"
#include "rule_query.h"
#include <array>
#include <vector>

class Game;
class ActionEncoderCpp;
class ActionEncoderV2;
class ActionEncoderV3;

namespace csplendor::move_generation_detail {

// Keep OFF/ON benchmark builds code-identical so unrelated header-inline hot
// paths are not moved solely by the experiment axis.
#ifdef CSPLENDOR_CLOSED_FORM_RETURN_COUNT
inline const volatile bool closed_form_return_count_enabled = true;
#else
inline const volatile bool closed_form_return_count_enabled = false;
#endif

inline uint16_t
count_small_token_returns(const std::array<uint8_t, 6> &available, int excess,
                          uint16_t limit) {
  if (limit == 0)
    return 0;
  if (excess == 0)
    return 1;

  uint16_t n1 = 0;
  uint16_t n2 = 0;
  uint16_t n3 = 0;
  for (uint8_t amount : available) {
    n1 += amount >= 1;
    n2 += amount >= 2;
    n3 += amount >= 3;
  }

  uint16_t count = 0;
  if (excess == 1) {
    count = n1;
  } else if (excess == 2) {
    count = static_cast<uint16_t>(n1 * (n1 - 1) / 2 + n2);
  } else if (excess == 3) {
    count = static_cast<uint16_t>(n1 * (n1 - 1) * (n1 - 2) / 6 + n2 * (n1 - 1) +
                                  n3);
  }
  return std::min(count, limit);
}

} // namespace csplendor::move_generation_detail

class MoveGenerator {
public:
  using EligibleNobles = csplendor::rules::EligibleNobles;

  // Fixed-size version - no heap allocations
  static MoveList generate_all_fixed(const Board &board,
                                     bool simple_payment_mode = false) {
    MoveList result;
    auto sink = [&result](const Action &action) {
      result.push_back(action);
      return true;
    };
    consume_all_capped(board, simple_payment_mode, sink);
    return result;
  }

  static uint16_t count_all_fixed(const Board &board,
                                  bool simple_payment_mode = false) {
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return 0;
    if (board.waiting_noble)
      return get_eligible_nobles_fixed(board, board.current_player).count;

    validate_purchase_source_ids(board);

    uint16_t count = 0;
    uint16_t base_count = 0;
    auto sink = [&board, &count, &base_count](const Action &action) {
      if (base_count >= MAX_MOVES)
        return false;
      ++base_count;
      uint16_t remaining = static_cast<uint16_t>(MAX_MOVES - count);
      count += count_with_returns(board, action, remaining);
      return count < MAX_MOVES && base_count < MAX_MOVES;
    };
    emit_base_actions(board, simple_payment_mode, sink);
    // PASS is a forced rule transition. It is emitted only when the player
    // has no ordinary action, so the fixed 48-slot policy mapping remains
    // unchanged while the public legal-action APIs stay total.
    return base_count == 0 ? 1 : count;
  }

  static bool requires_forced_pass(const Board &board,
                                   bool simple_payment_mode = false) {
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over() ||
        board.waiting_noble)
      return false;

    validate_purchase_source_ids(board);
    bool has_ordinary_action = false;
    auto sink = [&has_ordinary_action](const Action &) {
      has_ordinary_action = true;
      return false;
    };
    emit_base_actions(board, simple_payment_mode, sink);
    return !has_ordinary_action;
  }

  // Legacy vector version for compatibility
  static std::vector<Action> generate_all(const Board &board,
                                          bool simple_payment_mode = false) {
    std::vector<Action> result;
    result.reserve(count_all_fixed(board, simple_payment_mode));
    auto sink = [&result](const Action &action) {
      result.push_back(action);
      return true;
    };
    consume_all_capped(board, simple_payment_mode, sink);
    return result;
  }

  static std::vector<Action> generate_base(const Board &board,
                                           bool simple_payment_mode = false) {
    std::vector<Action> actions;
    if (board.current_player >= Board::NUM_PLAYERS)
      return actions;

    auto sink = [&actions](const Action &action) {
      actions.push_back(action);
      return true;
    };
    emit_base_actions(board, simple_payment_mode, sink);
    if (actions.empty() && !board.is_game_over() && !board.waiting_noble) {
      Action pass;
      pass.type = PASS;
      actions.push_back(pass);
    }
    return actions;
  }

  static std::vector<uint8_t> get_eligible_nobles(const Board &board,
                                                  int player_idx) {
    EligibleNobles fixed = get_eligible_nobles_fixed(board, player_idx);
    return std::vector<uint8_t>(fixed.begin(), fixed.end());
  }

  static EligibleNobles get_eligible_nobles_fixed(const Board &board,
                                                  int player_idx) {
    return csplendor::rules::eligible_nobles(board, player_idx);
  }

private:
  friend class Game;
  friend class ActionEncoderCpp;
  friend class ActionEncoderV2;
  friend class ActionEncoderV3;

  // All existing full-action APIs retain both legacy limits: at most the
  // first MAX_MOVES base actions are expanded and at most the first MAX_MOVES
  // final actions are observable. The emitter beneath this wrapper is
  // uncapped so generate_base() keeps its existing vector semantics.
  template <typename Sink>
  static bool consume_all_capped(const Board &board, bool simple_payment_mode,
                                 Sink &sink) {
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return true;

    uint16_t emitted_count = 0;
    auto final_sink = [&sink, &emitted_count](const Action &action) {
      if (emitted_count >= MAX_MOVES)
        return false;
      ++emitted_count;
      return sink(action) && emitted_count < MAX_MOVES;
    };

    if (board.waiting_noble)
      return emit_noble_visit_choices(board, final_sink);

    validate_purchase_source_ids(board);

    uint16_t base_count = 0;
    auto base_sink = [&board, &base_count, &final_sink](const Action &action) {
      if (base_count >= MAX_MOVES)
        return false;
      ++base_count;
      if (!emit_with_returns(board, action, final_sink))
        return false;
      return base_count < MAX_MOVES;
    };
    const bool completed =
        emit_base_actions(board, simple_payment_mode, base_sink);
    if (completed && base_count == 0) {
      Action pass;
      pass.type = PASS;
      return final_sink(pass);
    }
    return completed;
  }

  static void validate_purchase_source_ids(const Board &board) {
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        int8_t card_id = board.visible[level][slot];
        if (card_id != -1 && !is_valid_card_id(card_id))
          (void)get_card(card_id);
      }
    }

    const auto &player = board.players[board.current_player];
    for (int slot = 0; slot < 3; ++slot) {
      int8_t card_id = player.reserved[slot];
      if (card_id != -1 && !is_valid_card_id(card_id))
        (void)get_card(card_id);
    }
  }

  template <typename Sink>
  static bool emit_base_actions(const Board &board, bool simple_payment_mode,
                                Sink &sink) {
    return emit_take_different(board, sink) && emit_take_same(board, sink) &&
           emit_reserve_visible(board, sink) &&
           emit_reserve_deck(board, sink) &&
           emit_purchase(board, simple_payment_mode, sink);
  }

  template <typename Sink>
  static bool emit_take_different(const Board &board, Sink &sink) {
    std::array<int, 5> available_colors;
    int num_available = 0;
    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] > 0)
        available_colors[num_available++] = color;
    }

    if (num_available >= 3) {
      for (int i = 0; i < num_available; ++i) {
        for (int j = i + 1; j < num_available; ++j) {
          for (int k = j + 1; k < num_available; ++k) {
            Action action;
            action.type = TAKE_DIFFERENT;
            action.take[available_colors[i]] = 1;
            action.take[available_colors[j]] = 1;
            action.take[available_colors[k]] = 1;
            if (!sink(action))
              return false;
          }
        }
      }
    } else if (num_available > 0) {
      Action action;
      action.type = TAKE_DIFFERENT;
      for (int i = 0; i < num_available; ++i)
        action.take[available_colors[i]] = 1;
      if (!sink(action))
        return false;
    }
    return true;
  }

  template <typename Sink>
  static bool emit_take_same(const Board &board, Sink &sink) {
    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] >= 4) {
        Action action;
        action.type = TAKE_SAME;
        action.take[color] = 2;
        if (!sink(action))
          return false;
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_reserve_visible(const Board &board, Sink &sink) {
    if (!board.players[board.current_player].can_reserve())
      return true;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        if (board.visible[level][slot] != -1) {
          Action action;
          action.type = RESERVE_VISIBLE;
          action.card_id = board.visible[level][slot];
          if (!sink(action))
            return false;
        }
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_reserve_deck(const Board &board, Sink &sink) {
    if (!board.players[board.current_player].can_reserve())
      return true;
    for (int level = 0; level < 3; ++level) {
      if (!board.decks[level].empty()) {
        Action action;
        action.type = RESERVE_DECK;
        action.deck_level = static_cast<int8_t>(level);
        if (!sink(action))
          return false;
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_purchase(const Board &board, bool simple_payment_mode,
                            Sink &sink) {
    const auto &player = board.players[board.current_player];

    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        if (board.visible[level][slot] != -1 &&
            !emit_purchase_options(player, board.visible[level][slot], false,
                                   simple_payment_mode, sink)) {
          return false;
        }
      }
    }

    for (int slot = 0; slot < 3; ++slot) {
      if (player.reserved[slot] != -1 &&
          !emit_purchase_options(player, player.reserved[slot], true,
                                 simple_payment_mode, sink)) {
        return false;
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_purchase_options(const PlayerState &player, int8_t card_id,
                                    bool from_reserved,
                                    bool simple_payment_mode, Sink &sink) {
    const auto &card = get_card(card_id);
    const auto effective_cost =
        csplendor::rules::effective_card_cost(player, card);

    int min_gold = cli::ResourceBundle::needed_gold(
        card.packed_cost, player.packed_bonuses, player.packed_gems);
    if (min_gold > player.gems[GOLD])
      return true;

    std::array<uint8_t, 5> gold_as = {0, 0, 0, 0, 0};
    return emit_gold_as_combinations(player, effective_cost, 0, 0, gold_as,
                                     card_id, from_reserved,
                                     simple_payment_mode, sink);
  }

  template <typename Sink>
  static bool emit_gold_as_combinations(
      const PlayerState &player, const std::array<int, 5> &effective_cost,
      int color_idx, int gold_used, std::array<uint8_t, 5> gold_as,
      int8_t card_id, bool from_reserved, bool simple_payment_mode,
      Sink &sink) {
    if (color_idx == 5) {
      Action action;
      action.type = PURCHASE;
      action.card_id = card_id;
      action.from_reserved = from_reserved;
      action.gold_as = gold_as;
      return sink(action);
    }

    int cost = effective_cost[color_idx];
    int player_gems = player.gems[color_idx];
    int remaining_gold = player.gems[GOLD] - gold_used;
    int min_gold_for_color = std::max(0, cost - player_gems);
    int max_gold_for_color = std::min(cost, remaining_gold);
    if (min_gold_for_color > max_gold_for_color)
      return true;

    if (simple_payment_mode) {
      gold_as[color_idx] = static_cast<uint8_t>(min_gold_for_color);
      return emit_gold_as_combinations(
          player, effective_cost, color_idx + 1, gold_used + min_gold_for_color,
          gold_as, card_id, from_reserved, simple_payment_mode, sink);
    }

    for (int gold = min_gold_for_color; gold <= max_gold_for_color; ++gold) {
      gold_as[color_idx] = static_cast<uint8_t>(gold);
      if (!emit_gold_as_combinations(
              player, effective_cost, color_idx + 1, gold_used + gold, gold_as,
              card_id, from_reserved, simple_payment_mode, sink)) {
        return false;
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_with_returns(const Board &board, const Action &action,
                                Sink &sink) {
    // Purchases can only spend gems; unlike take/reserve actions they never
    // permit an explicit token return.  This also keeps generator/apply
    // parity for public editor states that start above the ten-token limit.
    if (action.type == PURCHASE)
      return sink(action);

    const auto next_gems =
        csplendor::rules::gems_after_token_action(board, action);
    const int excess = csplendor::rules::required_token_return(next_gems);
    if (excess <= 0)
      return sink(action);

    std::array<uint8_t, 6> current_return = {0, 0, 0, 0, 0, 0};
    return emit_return_combinations(next_gems, excess, 0, current_return,
                                    action, sink);
  }

  static uint16_t count_with_returns(const Board &board, const Action &action,
                                     uint16_t limit) {
    if (action.type == PURCHASE)
      return std::min<uint16_t>(1, limit);

    const auto next_gems =
        csplendor::rules::gems_after_token_action(board, action);
    const int excess = csplendor::rules::required_token_return(next_gems);
    if (excess <= 0)
      return std::min<uint16_t>(1, limit);
    if (excess <= 3 &&
        csplendor::move_generation_detail::closed_form_return_count_enabled) {
      return csplendor::move_generation_detail::count_small_token_returns(
          next_gems, excess, limit);
    }
    return count_return_combinations(next_gems, excess, 0, limit);
  }

  static uint16_t
  count_return_combinations(const std::array<uint8_t, 6> &available,
                            int remaining, int color_idx, uint16_t limit) {
    if (limit == 0)
      return 0;
    if (remaining == 0)
      return 1;
    if (color_idx == 6)
      return 0;

    uint16_t count = 0;
    int max_return =
        std::min(remaining, static_cast<int>(available[color_idx]));
    for (int amount = 0; amount <= max_return; ++amount) {
      uint16_t branch_limit = static_cast<uint16_t>(limit - count);
      count += count_return_combinations(available, remaining - amount,
                                         color_idx + 1, branch_limit);
      if (count >= limit)
        return limit;
    }
    return count;
  }

  template <typename Sink>
  static bool emit_return_combinations(const std::array<uint8_t, 6> &available,
                                       int remaining, int color_idx,
                                       std::array<uint8_t, 6> current_return,
                                       const Action &base_action, Sink &sink) {
    if (remaining == 0) {
      Action action = base_action;
      action.return_gems = current_return;
      return sink(action);
    }
    if (color_idx == 6)
      return true;

    int max_return =
        std::min(remaining, static_cast<int>(available[color_idx]));
    for (int amount = 0; amount <= max_return; ++amount) {
      current_return[color_idx] = static_cast<uint8_t>(amount);
      if (!emit_return_combinations(available, remaining - amount,
                                    color_idx + 1, current_return, base_action,
                                    sink)) {
        return false;
      }
    }
    return true;
  }

  template <typename Sink>
  static bool emit_noble_visit_choices(const Board &board, Sink &sink) {
    EligibleNobles eligible =
        get_eligible_nobles_fixed(board, board.current_player);
    for (size_t i = 0; i < eligible.size(); ++i) {
      Action action;
      action.type = VISIT_NOBLE;
      action.noble_choice = eligible[i];
      if (!sink(action))
        return false;
    }
    return true;
  }
};

#endif // CSPLENDOR_MOVE_GENERATOR_H
