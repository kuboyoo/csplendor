#ifndef CSPLENDOR_MOVE_GENERATOR_H
#define CSPLENDOR_MOVE_GENERATOR_H

#include "action.h"
#include "board.h"
#include <array>
#include <vector>

class MoveGenerator {
public:
  // Fixed-size version - no heap allocations
  static MoveList generate_all_fixed(const Board &board, bool simple_payment_mode = false) {
    MoveList result;
    if (board.current_player >= Board::NUM_PLAYERS)
      return result;

    if (board.waiting_noble) {
      generate_noble_visit_choices_fixed(board, result);
      return result;
    }

    MoveList actions;
    generate_take_different_fixed(board, actions);
    generate_take_same_fixed(board, actions);
    generate_reserve_visible_fixed(board, actions);
    generate_reserve_deck_fixed(board, actions);
    generate_purchase_fixed(board, actions, simple_payment_mode);

    // Expand actions with all possible gem returns if needed
    for (size_t i = 0; i < actions.size(); ++i) {
      expand_with_returns_fixed(board, actions[i], result);
    }
    return result;
  }

  // Legacy vector version for compatibility
  static std::vector<Action> generate_all(const Board &board, bool simple_payment_mode = false) {
    MoveList fixed = generate_all_fixed(board, simple_payment_mode);
    return std::vector<Action>(fixed.begin(), fixed.end());
  }

  static std::vector<Action> generate_base(const Board &board, bool simple_payment_mode = false) {
    std::vector<Action> actions;
    if (board.current_player >= Board::NUM_PLAYERS)
      return actions;

    generate_take_different(board, actions);
    generate_take_same(board, actions);
    generate_reserve_visible(board, actions);
    generate_reserve_deck(board, actions);
    generate_purchase(board, actions, simple_payment_mode);
    return actions;
  }

  static std::vector<uint8_t> get_eligible_nobles(const Board &board,
                                                  int player_idx) {
    if (player_idx < 0 || player_idx >= Board::NUM_PLAYERS)
      return {};

    const auto &p = board.players[player_idx];
    std::vector<uint8_t> eligible;

    // Use cached eligibility mask for fast lookup
    uint16_t mask = p.noble_eligibility_mask;

    // Check only nobles on board that player is eligible for
    for (size_t i = 0; i < board.nobles.size(); ++i) {
      uint8_t noble_id = board.nobles[i];
      if (is_valid_noble_id(noble_id) &&
          (mask & (uint16_t(1) << noble_id))) {
        eligible.push_back(noble_id);
      }
    }
    return eligible;
  }

private:
  static void generate_take_different(const Board &b,
                                      std::vector<Action> &out) {
    std::vector<int> available_colors;
    for (int i = 0; i < 5; ++i)
      if (b.bank[i] > 0)
        available_colors.push_back(i);

    if (available_colors.size() >= 3) {
      // Combinations of 3
      for (size_t i = 0; i < available_colors.size(); ++i) {
        for (size_t j = i + 1; j < available_colors.size(); ++j) {
          for (size_t k = j + 1; k < available_colors.size(); ++k) {
            Action a;
            a.type = TAKE_DIFFERENT;
            a.take[available_colors[i]] = 1;
            a.take[available_colors[j]] = 1;
            a.take[available_colors[k]] = 1;
            out.push_back(a);
          }
        }
      }
    } else if (available_colors.size() > 0) {
      // Take all available if less than 3
      Action a;
      a.type = TAKE_DIFFERENT;
      for (int c : available_colors)
        a.take[c] = 1;
      out.push_back(a);
    }
  }

  static void generate_take_same(const Board &b, std::vector<Action> &out) {
    for (int i = 0; i < 5; ++i) {
      if (b.bank[i] >= 4) {
        Action a;
        a.type = TAKE_SAME;
        a.take[i] = 2;
        out.push_back(a);
      }
    }
  }

  static void generate_reserve_visible(const Board &b,
                                       std::vector<Action> &out) {
    if (!b.players[b.current_player].can_reserve())
      return;
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (b.visible[l][s] != -1) {
          Action a;
          a.type = RESERVE_VISIBLE;
          a.card_id = b.visible[l][s];
          out.push_back(a);
        }
      }
    }
  }

  static void generate_reserve_deck(const Board &b, std::vector<Action> &out) {
    if (!b.players[b.current_player].can_reserve())
      return;
    for (int l = 0; l < 3; ++l) {
      if (!b.decks[l].empty()) {
        Action a;
        a.type = RESERVE_DECK;
        a.deck_level = l;
        out.push_back(a);
      }
    }
  }

  static void generate_purchase(const Board &b, std::vector<Action> &out, bool simple_payment_mode = false) {
    const auto &p = b.players[b.current_player];

    // From board
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (b.visible[l][s] != -1) {
          generate_purchase_options(p, b.visible[l][s], false, out, simple_payment_mode);
        }
      }
    }

    // From reserved
    for (int i = 0; i < 3; ++i) {
      if (p.reserved[i] != -1) {
        generate_purchase_options(p, p.reserved[i], true, out, simple_payment_mode);
      }
    }
  }

  // Generate all valid payment combinations for a single card
  static void generate_purchase_options(const PlayerState &p, int8_t card_id,
                                        bool from_reserved,
                                        std::vector<Action> &out,
                                        bool simple_payment_mode = false) {
    const auto &card = get_card(card_id);

    // Calculate effective cost (cost - bonuses)
    std::array<int, 5> effective_cost;
    for (int i = 0; i < 5; ++i) {
      effective_cost[i] = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
    }

    // Calculate minimum gold needed using packed representation
    int min_gold = cli::ResourceBundle::needed_gold(
        card.packed_cost, p.packed_bonuses, p.packed_gems);

    if (min_gold > p.gems[GOLD]) {
      return; // Can't afford this card at all
    }

    // Generate all valid gold_as combinations
    std::array<uint8_t, 5> gold_as = {0, 0, 0, 0, 0};
    generate_gold_as_combinations(p, effective_cost, 0, 0, gold_as, card_id,
                                  from_reserved, out, simple_payment_mode);
  }

  // Recursively generate all valid gold_as combinations
  static void generate_gold_as_combinations(
      const PlayerState &p, const std::array<int, 5> &effective_cost,
      int color_idx, int gold_used, std::array<uint8_t, 5> gold_as,
      int8_t card_id, bool from_reserved, std::vector<Action> &out,
      bool simple_payment_mode = false) {
    if (color_idx == 5) {
      // All colors processed, create an action
      Action a;
      a.type = PURCHASE;
      a.card_id = card_id;
      a.from_reserved = from_reserved;
      a.gold_as = gold_as;
      out.push_back(a);
      return;
    }

    int cost = effective_cost[color_idx];
    int player_gems = p.gems[color_idx];
    int remaining_gold = p.gems[GOLD] - gold_used;

    // Calculate min and max gold usage for this color
    // min: if player has enough gems, 0. Otherwise, cost - gems.
    // max: minimum of (cost, remaining_gold)
    int min_gold_for_color = std::max(0, cost - player_gems);
    int max_gold_for_color = std::min(cost, remaining_gold);

    if (min_gold_for_color > max_gold_for_color) {
      return; // Invalid branch, can't afford
    }

    if (simple_payment_mode) {
      // Simple mode: only use minimum gold (maximize gem usage)
      gold_as[color_idx] = min_gold_for_color;
      generate_gold_as_combinations(p, effective_cost, color_idx + 1,
                                    gold_used + min_gold_for_color, gold_as, card_id,
                                    from_reserved, out, simple_payment_mode);
    } else {
      // Full mode: generate all valid payment combinations
      for (int g = min_gold_for_color; g <= max_gold_for_color; ++g) {
        gold_as[color_idx] = g;
        generate_gold_as_combinations(p, effective_cost, color_idx + 1,
                                      gold_used + g, gold_as, card_id,
                                      from_reserved, out, simple_payment_mode);
      }
    }
  }

  static void expand_with_returns(const Board &b, Action action,
                                  std::vector<Action> &out) {
    const auto &p = b.players[b.current_player];
    uint8_t next_gems[6];
    for (int i = 0; i < 6; ++i)
      next_gems[i] = p.gems[i];

    // Simulate gem changes
    if (action.type == TAKE_DIFFERENT || action.type == TAKE_SAME) {
      for (int i = 0; i < 5; ++i)
        next_gems[i] += action.take[i];
    } else if (action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK) {
      if (b.bank[GOLD] > 0)
        next_gems[GOLD]++;
    } else if (action.type == PURCHASE) {
      const auto &card = get_card(action.card_id);
      int gold_used = 0;
      for (int i = 0; i < 5; ++i) {
        int cost = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
        int from_gems = std::min((int)next_gems[i], cost);
        next_gems[i] -= from_gems;
        gold_used += (cost - from_gems);
      }
      next_gems[GOLD] -= gold_used;
    }

    int total = 0;
    for (int i = 0; i < 6; ++i)
      total += next_gems[i];

    int excess = std::max(0, total - Board::MAX_TOKENS);

    if (excess > 0) {
      auto return_options = generate_return_combinations(next_gems, excess);
      for (const auto &opt : return_options) {
        Action a_ret = action;
        for (int i = 0; i < 6; ++i) {
          a_ret.return_gems[i] = opt[i];
        }
        out.push_back(a_ret);
      }
    } else {
      out.push_back(action);
    }
  }

  static void generate_noble_visit_choices(const Board &b,
                                           std::vector<Action> &out) {
    auto eligible = get_eligible_nobles(b, b.current_player);
    for (uint8_t noble_id : eligible) {
      Action a;
      a.type = VISIT_NOBLE;
      a.noble_choice = noble_id;
      out.push_back(a);
    }
  }

  static std::vector<std::array<uint8_t, 6>>
  generate_return_combinations(const uint8_t current[6], int count) {
    std::vector<std::array<uint8_t, 6>> result;
    std::array<uint8_t, 6> current_return = {0, 0, 0, 0, 0, 0};
    recursive_return(current, count, 0, current_return, result);
    return result;
  }

  static void recursive_return(const uint8_t available[6], int remaining,
                               int color_idx,
                               std::array<uint8_t, 6> current_return,
                               std::vector<std::array<uint8_t, 6>> &result) {
    if (remaining == 0) {
      result.push_back(current_return);
      return;
    }
    if (color_idx == 6)
      return;

    for (int i = 0; i <= std::min(remaining, (int)available[color_idx]); ++i) {
      current_return[color_idx] = i;
      recursive_return(available, remaining - i, color_idx + 1, current_return,
                       result);
    }
  }

  // ========== Fixed-size versions (no heap allocations) ==========

  static void generate_take_different_fixed(const Board &b, MoveList &out) {
    std::array<int, 5> available_colors;
    int num_available = 0;
    for (int i = 0; i < 5; ++i)
      if (b.bank[i] > 0)
        available_colors[num_available++] = i;

    if (num_available >= 3) {
      for (int i = 0; i < num_available; ++i) {
        for (int j = i + 1; j < num_available; ++j) {
          for (int k = j + 1; k < num_available; ++k) {
            Action a;
            a.type = TAKE_DIFFERENT;
            a.take[available_colors[i]] = 1;
            a.take[available_colors[j]] = 1;
            a.take[available_colors[k]] = 1;
            out.push_back(a);
          }
        }
      }
    } else if (num_available > 0) {
      Action a;
      a.type = TAKE_DIFFERENT;
      for (int i = 0; i < num_available; ++i)
        a.take[available_colors[i]] = 1;
      out.push_back(a);
    }
  }

  static void generate_take_same_fixed(const Board &b, MoveList &out) {
    for (int i = 0; i < 5; ++i) {
      if (b.bank[i] >= 4) {
        Action a;
        a.type = TAKE_SAME;
        a.take[i] = 2;
        out.push_back(a);
      }
    }
  }

  static void generate_reserve_visible_fixed(const Board &b, MoveList &out) {
    if (!b.players[b.current_player].can_reserve())
      return;
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (b.visible[l][s] != -1) {
          Action a;
          a.type = RESERVE_VISIBLE;
          a.card_id = b.visible[l][s];
          out.push_back(a);
        }
      }
    }
  }

  static void generate_reserve_deck_fixed(const Board &b, MoveList &out) {
    if (!b.players[b.current_player].can_reserve())
      return;
    for (int l = 0; l < 3; ++l) {
      if (!b.decks[l].empty()) {
        Action a;
        a.type = RESERVE_DECK;
        a.deck_level = l;
        out.push_back(a);
      }
    }
  }

  static void generate_purchase_fixed(const Board &b, MoveList &out, bool simple_payment_mode = false) {
    const auto &p = b.players[b.current_player];

    // From board
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (b.visible[l][s] != -1) {
          generate_purchase_options_fixed(p, b.visible[l][s], false, out, simple_payment_mode);
        }
      }
    }

    // From reserved
    for (int i = 0; i < 3; ++i) {
      if (p.reserved[i] != -1) {
        generate_purchase_options_fixed(p, p.reserved[i], true, out, simple_payment_mode);
      }
    }
  }

  static void generate_purchase_options_fixed(const PlayerState &p,
                                              int8_t card_id, bool from_reserved,
                                              MoveList &out, bool simple_payment_mode = false) {
    const auto &card = get_card(card_id);

    std::array<int, 5> effective_cost;
    for (int i = 0; i < 5; ++i) {
      effective_cost[i] = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
    }

    int min_gold = cli::ResourceBundle::needed_gold(card.packed_cost,
                                                    p.packed_bonuses, p.packed_gems);

    if (min_gold > p.gems[GOLD]) {
      return;
    }

    std::array<uint8_t, 5> gold_as = {0, 0, 0, 0, 0};
    generate_gold_as_combinations_fixed(p, effective_cost, 0, 0, gold_as,
                                        card_id, from_reserved, out, simple_payment_mode);
  }

  static void generate_gold_as_combinations_fixed(
      const PlayerState &p, const std::array<int, 5> &effective_cost,
      int color_idx, int gold_used, std::array<uint8_t, 5> gold_as,
      int8_t card_id, bool from_reserved, MoveList &out, bool simple_payment_mode = false) {
    if (color_idx == 5) {
      Action a;
      a.type = PURCHASE;
      a.card_id = card_id;
      a.from_reserved = from_reserved;
      a.gold_as = gold_as;
      out.push_back(a);
      return;
    }

    int cost = effective_cost[color_idx];
    int player_gems = p.gems[color_idx];
    int remaining_gold = p.gems[GOLD] - gold_used;

    int min_gold_for_color = std::max(0, cost - player_gems);
    int max_gold_for_color = std::min(cost, remaining_gold);

    if (min_gold_for_color > max_gold_for_color) {
      return;
    }

    if (simple_payment_mode) {
      // Simple mode: only use minimum gold (maximize gem usage)
      gold_as[color_idx] = min_gold_for_color;
      generate_gold_as_combinations_fixed(p, effective_cost, color_idx + 1,
                                          gold_used + min_gold_for_color, gold_as, card_id,
                                          from_reserved, out, simple_payment_mode);
    } else {
      // Full mode: generate all valid payment combinations
      for (int g = min_gold_for_color; g <= max_gold_for_color; ++g) {
        gold_as[color_idx] = g;
        generate_gold_as_combinations_fixed(p, effective_cost, color_idx + 1,
                                            gold_used + g, gold_as, card_id,
                                            from_reserved, out, simple_payment_mode);
      }
    }
  }

  static void expand_with_returns_fixed(const Board &b, Action action,
                                        MoveList &out) {
    const auto &p = b.players[b.current_player];
    uint8_t next_gems[6];
    for (int i = 0; i < 6; ++i)
      next_gems[i] = p.gems[i];

    if (action.type == TAKE_DIFFERENT || action.type == TAKE_SAME) {
      for (int i = 0; i < 5; ++i)
        next_gems[i] += action.take[i];
    } else if (action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK) {
      if (b.bank[GOLD] > 0)
        next_gems[GOLD]++;
    } else if (action.type == PURCHASE) {
      const auto &card = get_card(action.card_id);
      int gold_used = 0;
      for (int i = 0; i < 5; ++i) {
        int cost = std::max(0, (int)card.cost[i] - (int)p.bonuses[i]);
        int from_gems = std::min((int)next_gems[i], cost);
        next_gems[i] -= from_gems;
        gold_used += (cost - from_gems);
      }
      next_gems[GOLD] -= gold_used;
    }

    int total = 0;
    for (int i = 0; i < 6; ++i)
      total += next_gems[i];

    int excess = std::max(0, total - Board::MAX_TOKENS);

    if (excess > 0) {
      // Generate return combinations directly into MoveList
      std::array<uint8_t, 6> current_return = {0, 0, 0, 0, 0, 0};
      recursive_return_fixed(next_gems, excess, 0, current_return, action, out);
    } else {
      out.push_back(action);
    }
  }

  static void recursive_return_fixed(const uint8_t available[6], int remaining,
                                     int color_idx,
                                     std::array<uint8_t, 6> current_return,
                                     Action base_action, MoveList &out) {
    if (remaining == 0) {
      Action a_ret = base_action;
      a_ret.return_gems = current_return;
      out.push_back(a_ret);
      return;
    }
    if (color_idx == 6)
      return;

    for (int i = 0; i <= std::min(remaining, (int)available[color_idx]); ++i) {
      current_return[color_idx] = i;
      recursive_return_fixed(available, remaining - i, color_idx + 1,
                             current_return, base_action, out);
    }
  }

  static void generate_noble_visit_choices_fixed(const Board &b, MoveList &out) {
    auto eligible = get_eligible_nobles(b, b.current_player);
    for (uint8_t noble_id : eligible) {
      Action a;
      a.type = VISIT_NOBLE;
      a.noble_choice = noble_id;
      out.push_back(a);
    }
  }
};

#endif // CSPLENDOR_MOVE_GENERATOR_H
