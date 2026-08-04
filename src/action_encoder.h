#ifndef CSPLENDOR_ACTION_ENCODER_H
#define CSPLENDOR_ACTION_ENCODER_H

#include "action.h"
#include "action_encoder_common.h"
#include "game.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

/**
 * C++ implementation of ActionEncoder.
 * Encodes/decodes Splendor actions to/from integer indices [0, 47].
 *
 * Action mapping:
 * - TAKE_DIFFERENT: 0-9   (10 patterns: C(5,3) combinations)
 * - TAKE_SAME:      10-14 (5 patterns: one per color)
 * - RESERVE_VISIBLE: 15-26 (12 patterns: 3 levels * 4 slots)
 * - RESERVE_DECK:   27-29 (3 patterns: one per level)
 * - PURCHASE_VISIBLE: 30-41 (12 patterns: 3 levels * 4 slots)
 * - PURCHASE_RESERVED: 42-44 (3 patterns: one per reserved slot)
 * - VISIT_NOBLE:    45-47 (3 patterns: one per noble slot)
 */
class ActionEncoderCpp {
public:
  static constexpr int BASE_ACTION_COUNT = 48;

  // Pre-computed C(5,3) combinations for TAKE_DIFFERENT
  // (0,1,2), (0,1,3), (0,1,4), (0,2,3), (0,2,4), (0,3,4), (1,2,3), (1,2,4),
  // (1,3,4), (2,3,4)
  static constexpr std::array<std::array<int, 3>, 10> TAKE_DIFF_COMBINATIONS = {
      {{{0, 1, 2}},
       {{0, 1, 3}},
       {{0, 1, 4}},
       {{0, 2, 3}},
       {{0, 2, 4}},
       {{0, 3, 4}},
       {{1, 2, 3}},
       {{1, 2, 4}},
       {{1, 3, 4}},
       {{2, 3, 4}}}};

  /**
   * Encode an action to an index [0, 47].
   * Returns -1 if the action cannot be encoded.
   */
  static int encode(const Action &action, const Game &game) {
    const Board &board = game.board;

    switch (action.type) {
    case TAKE_DIFFERENT: {
      // Find which colors were taken
      std::array<int, 3> colors = {-1, -1, -1};
      int color_count = 0;
      for (int i = 0; i < 5 && color_count < 3; ++i) {
        if (action.take[i] > 0) {
          colors[color_count++] = i;
        }
      }

      if (color_count == 3) {
        // Find matching combination
        for (int idx = 0; idx < 10; ++idx) {
          if (colors[0] == TAKE_DIFF_COMBINATIONS[idx][0] &&
              colors[1] == TAKE_DIFF_COMBINATIONS[idx][1] &&
              colors[2] == TAKE_DIFF_COMBINATIONS[idx][2]) {
            return idx;
          }
        }
      } else {
        // Less than 3 colors taken (bank shortage)
        // Find first combination that contains all taken colors
        for (int idx = 0; idx < 10; ++idx) {
          bool all_match = true;
          for (int c = 0; c < color_count; ++c) {
            bool found = false;
            for (int j = 0; j < 3; ++j) {
              if (colors[c] == TAKE_DIFF_COMBINATIONS[idx][j]) {
                found = true;
                break;
              }
            }
            if (!found) {
              all_match = false;
              break;
            }
          }
          if (all_match) {
            return idx;
          }
        }
      }
      return -1;
    }

    case TAKE_SAME: {
      for (int i = 0; i < 5; ++i) {
        if (action.take[i] == 2) {
          return 10 + i;
        }
      }
      return -1;
    }

    case RESERVE_VISIBLE: {
      for (int level = 0; level < 3; ++level) {
        for (int slot = 0; slot < 4; ++slot) {
          if (board.visible[level][slot] == action.card_id) {
            return 15 + level * 4 + slot;
          }
        }
      }
      return -1;
    }

    case RESERVE_DECK: {
      if (action.deck_level < 0 || action.deck_level >= 3)
        return -1;
      return 27 + action.deck_level;
    }

    case PURCHASE: {
      if (action.from_reserved) {
        // Find index in player's reserved cards
        const PlayerState &player = board.players[board.current_player];
        for (int i = 0; i < 3; ++i) {
          if (player.reserved[i] == action.card_id) {
            return 42 + i;
          }
        }
        return -1;
      } else {
        // Visible on board
        for (int level = 0; level < 3; ++level) {
          for (int slot = 0; slot < 4; ++slot) {
            if (board.visible[level][slot] == action.card_id) {
              return 30 + level * 4 + slot;
            }
          }
        }
        return -1;
      }
    }

    case VISIT_NOBLE: {
      // Map noble_choice (noble ID) to position in nobles list
      int8_t noble_id = action.noble_choice;
      for (size_t i = 0; i < board.nobles.size() && i < 3; ++i) {
        if (board.nobles[i] == noble_id) {
          return 45 + static_cast<int>(i);
        }
      }
      return -1;
    }

    default:
      return -1;
    }
  }

  /**
   * Get a boolean mask of size 48 where 1 means legal.
   * This is the critical function that was causing GIL contention
   * when implemented in Python.
   */
  static uint64_t get_action_mask_bits(const Game &game) {
    const Board &board = game.board;
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return 0;

    if (board.waiting_noble) {
      if (has_duplicate_nobles(board))
        return get_action_mask_reference_bits(game);
      return get_noble_mask_bits(board);
    }

    // Keep the public editor-state exception contract. The direct path does
    // not otherwise need to dereference every purchase source.
    MoveGenerator::validate_purchase_source_ids(board);
    if (requires_reference_scan(game))
      return get_action_mask_reference_bits(game);
    return get_action_mask_bits_direct(game);
  }

  // MCTS hot path. The caller owns a canonical rule-engine state, so public
  // editor compatibility checks (duplicate sources and MAX_MOVES fallback)
  // are intentionally omitted.
  static uint64_t get_action_mask_bits_trusted(const Game &game) {
    const Board &board = game.board;
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return 0;
    if (board.waiting_noble)
      return get_noble_mask_bits(board);
    return get_action_mask_bits_direct(game);
  }

  static std::array<uint8_t, BASE_ACTION_COUNT>
  get_action_mask(const Game &game) {
    return mask_bits_to_array(get_action_mask_bits(game));
  }

  /**
   * Decode an index [0, 47] to an Action.
   * Returns a valid action if found, or a default Action if the index
   * doesn't correspond to any legal action.
   *
   * For PURCHASE actions with multiple payment options, selects the best
   * payment method (minimizing gold usage).
   */
  static Action decode(int index, const Game &game) {
    const Board &board = game.board;
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return Action{};

    if (board.waiting_noble) {
      if (has_duplicate_nobles(board))
        return decode_reference(index, game);
      const uint64_t mask = get_noble_mask_bits(board);
      if (!mask_contains(mask, index))
        return Action{};
      Action action;
      action.type = VISIT_NOBLE;
      action.noble_choice =
          static_cast<int8_t>(board.nobles[static_cast<size_t>(index - 45)]);
      return action;
    }

    MoveGenerator::validate_purchase_source_ids(board);
    if (requires_reference_scan(game))
      return decode_reference(index, game);

    const uint64_t mask = get_action_mask_bits_direct(game);
    if (!mask_contains(mask, index))
      return Action{};

    return decode_direct(index, game);
  }

  // Canonical counterpart to get_action_mask_bits_trusted(). Selection has
  // already proved legality, so this constructs only the requested action.
  static Action decode_trusted(int index, const Game &game) {
    const Board &board = game.board;
    if (board.current_player >= Board::NUM_PLAYERS || board.is_game_over())
      return Action{};
    if (board.waiting_noble) {
      const int slot = index - 45;
      if (slot < 0 || slot >= 3 ||
          static_cast<size_t>(slot) >= board.nobles.size())
        return Action{};
      Action action;
      action.type = VISIT_NOBLE;
      action.noble_choice = board.nobles[static_cast<size_t>(slot)];
      return action;
    }
    return decode_direct(index, game);
  }

  /**
   * Calculate Cost Performance (CP) score for a card given player state.
   * Higher score = better card to purchase.
   *
   * Formula: CP = (points * 5 + 1) / (0.5 * total_cost + 2 * shortage + 1)
   * where shortage = gems needed beyond current holdings
   */
  static float calculate_card_cp(const Card &card, const PlayerState &player) {
    float points = static_cast<float>(card.points);
    float total_cost = 0.0f;
    float shortage = 0.0f;

    for (int i = 0; i < 5; ++i) {
      total_cost += card.cost[i];
      int price = std::max(0, static_cast<int>(card.cost[i]) -
                                  static_cast<int>(player.bonuses[i]));
      int missing = std::max(0, price - static_cast<int>(player.gems[i]));
      shortage += missing;
    }

    if (total_cost == 0.0f) {
      return 100.0f; // Free card is amazing
    }

    // If we can afford it now (shortage = 0), higher priority
    return (points * 5.0f + 1.0f) / (0.5f * total_cost + 2.0f * shortage + 1.0f);
  }

  /**
   * Get action mask with heuristic scores.
   * Returns both a validity mask and heuristic scores for each action.
   *
   * Scores guide MCTS towards better moves:
   * - PURCHASE: Based on Cost Performance (CP) of the card
   * - RESERVE: Moderate score (0.5)
   * - TAKE_*: Lower score (0.2)
   *
   * The scores are NOT normalized - caller should normalize if needed.
   */
  static std::pair<std::array<uint8_t, BASE_ACTION_COUNT>,
                   std::array<float, BASE_ACTION_COUNT>>
  get_action_mask_with_scores(const Game &game) {
    std::array<uint8_t, BASE_ACTION_COUNT> mask = get_action_mask(game);
    std::array<float, BASE_ACTION_COUNT> scores = {0};

    const Board &board = game.board;
    if (board.current_player >= Board::NUM_PLAYERS)
      return {mask, scores};
    const PlayerState &player = board.players[board.current_player];
    for (int index = 0; index < BASE_ACTION_COUNT; ++index) {
      if (!mask[index])
        continue;
      if (index < 15) {
        scores[index] = 0.2f;
      } else if (index < 30) {
        scores[index] = 0.5f;
      } else if (index < 42) {
        const int slot = index - 30;
        const Card &card = get_card(board.visible[slot / 4][slot % 4]);
        scores[index] = 1.0f + calculate_card_cp(card, player) * 2.0f;
      } else if (index < 45) {
        const Card &card = get_card(player.reserved[index - 42]);
        scores[index] = 1.0f + calculate_card_cp(card, player) * 2.0f;
      } else {
        scores[index] = 5.0f;
      }
    }

    return {mask, scores};
  }

  /**
   * Get normalized heuristic policy from action scores.
   * Returns a probability distribution over legal actions.
   */
  static std::array<float, BASE_ACTION_COUNT>
  get_heuristic_policy(const Game &game) {
    auto [mask, scores] = get_action_mask_with_scores(game);

    // Normalize scores to get probabilities
    float sum = 0.0f;
    for (int i = 0; i < BASE_ACTION_COUNT; ++i) {
      if (mask[i]) {
        sum += scores[i];
      }
    }

    std::array<float, BASE_ACTION_COUNT> policy = {0};
    if (sum > 1e-6f) {
      for (int i = 0; i < BASE_ACTION_COUNT; ++i) {
        if (mask[i]) {
          policy[i] = scores[i] / sum;
        }
      }
    } else {
      // Uniform fallback
      int count = 0;
      for (int i = 0; i < BASE_ACTION_COUNT; ++i) {
        if (mask[i])
          count++;
      }
      if (count > 0) {
        float uniform = 1.0f / count;
        for (int i = 0; i < BASE_ACTION_COUNT; ++i) {
          if (mask[i])
            policy[i] = uniform;
        }
      }
    }

    return policy;
  }

#ifdef CSPLENDOR_ACTION_ENCODER_REFERENCE
  static std::array<uint8_t, BASE_ACTION_COUNT>
  get_action_mask_reference_for_testing(const Game &game) {
    return mask_bits_to_array(get_action_mask_reference_bits(game));
  }

  static Action decode_reference_for_testing(int index, const Game &game) {
    return decode_reference(index, game);
  }
#endif

private:
  static constexpr uint64_t ACTION_MASK_LIMIT =
      (uint64_t{1} << BASE_ACTION_COUNT) - 1;

  static bool mask_contains(uint64_t mask, int index) {
    return index >= 0 && index < BASE_ACTION_COUNT &&
           (mask & (uint64_t{1} << index)) != 0;
  }

  static std::array<uint8_t, BASE_ACTION_COUNT>
  mask_bits_to_array(uint64_t bits) {
    std::array<uint8_t, BASE_ACTION_COUNT> mask = {0};
    bits &= ACTION_MASK_LIMIT;
    for (int index = 0; index < BASE_ACTION_COUNT; ++index)
      mask[index] = static_cast<uint8_t>((bits >> index) & uint64_t{1});
    return mask;
  }

  static uint64_t get_action_mask_reference_bits(const Game &game) {
    uint64_t mask = 0;
    auto sink = [&game, &mask](const Action &action) {
      const int index = encode(action, game);
      if (index >= 0 && index < BASE_ACTION_COUNT)
        mask |= uint64_t{1} << index;
      return true;
    };
    MoveGenerator::consume_all_capped(game.board, game.simple_payment_mode,
                                      sink);
    return mask;
  }

  static Action decode_reference(int index, const Game &game) {
    const MoveList legal_actions =
        MoveGenerator::generate_all_fixed(game.board, game.simple_payment_mode);
    Action best_action;
    bool found = false;
    int best_gold_used = std::numeric_limits<int>::max();
    int best_gems_returned = std::numeric_limits<int>::max();
    for (const Action &action : legal_actions) {
      if (encode(action, game) != index)
        continue;
      int gold_used = 0;
      int gems_returned = 0;
      for (uint8_t amount : action.gold_as)
        gold_used += amount;
      for (uint8_t amount : action.return_gems)
        gems_returned += amount;
      if (!found || gold_used < best_gold_used ||
          (gold_used == best_gold_used && gems_returned < best_gems_returned)) {
        best_action = action;
        found = true;
        best_gold_used = gold_used;
        best_gems_returned = gems_returned;
      }
    }
    return best_action;
  }

  static bool has_duplicate_nobles(const Board &board) {
    for (size_t left = 0; left < board.nobles.size(); ++left) {
      for (size_t right = left + 1; right < board.nobles.size(); ++right) {
        if (board.nobles[left] == board.nobles[right])
          return true;
      }
    }
    return false;
  }

  static uint64_t get_noble_mask_bits(const Board &board) {
    const PlayerState &player = board.players[board.current_player];
    uint64_t mask = 0;
    for (size_t slot = 0; slot < board.nobles.size() && slot < 3; ++slot) {
      const uint8_t noble_id = board.nobles[slot];
      if (is_valid_noble_id(noble_id) &&
          (player.noble_eligibility_mask & (uint16_t{1} << noble_id)))
        mask |= uint64_t{1} << (45 + slot);
    }
    return mask;
  }

  static int payment_option_count(const PlayerState &player, const Card &card,
                                  bool simple_payment_mode,
                                  int limit = static_cast<int>(MAX_MOVES)) {
    const int gold = player.gems[GOLD];
    std::array<int, Board::MAX_TOKENS + 1> counts{};
    std::array<int, Board::MAX_TOKENS + 1> next{};
    counts[0] = 1;
    for (int color = 0; color < 5; ++color) {
      next.fill(0);
      const int cost = std::max(0, static_cast<int>(card.cost[color]) -
                                       static_cast<int>(player.bonuses[color]));
      const int minimum =
          std::max(0, cost - static_cast<int>(player.gems[color]));
      const int maximum = simple_payment_mode ? minimum : cost;
      for (int used = 0; used <= gold; ++used) {
        if (counts[used] == 0)
          continue;
        for (int amount = minimum; amount <= maximum && used + amount <= gold;
             ++amount) {
          next[used + amount] =
              std::min(limit, next[used + amount] + counts[used]);
        }
      }
      counts = next;
    }
    int total = 0;
    for (int used = 0; used <= gold; ++used)
      total = std::min(limit, total + counts[used]);
    return total;
  }

  static int return_pattern_upper_bound(int excess) {
    static constexpr std::array<int, 4> count = {1, 6, 21, 56};
    return count[static_cast<size_t>(std::clamp(excess, 0, 3))];
  }

  static bool requires_reference_scan(const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    const int tokens = player.total_gems();
    if (tokens > Board::MAX_TOKENS || player.gems[GOLD] > Board::MAX_TOKENS)
      return true;

    std::array<bool, CARD_COUNT> visible_seen{};
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        const int8_t card_id = board.visible[level][slot];
        if (card_id < 0)
          continue;
        if (visible_seen[static_cast<size_t>(card_id)])
          return true;
        visible_seen[static_cast<size_t>(card_id)] = true;
      }
    }
    std::array<bool, CARD_COUNT> reserved_seen{};
    for (int slot = 0; slot < 3; ++slot) {
      const int8_t card_id = player.reserved[slot];
      if (card_id < 0)
        continue;
      if (reserved_seen[static_cast<size_t>(card_id)])
        return true;
      reserved_seen[static_cast<size_t>(card_id)] = true;
    }

    int available_colors = 0;
    int same_colors = 0;
    for (int color = 0; color < 5; ++color) {
      available_colors += board.bank[color] > 0 ? 1 : 0;
      same_colors += board.bank[color] >= 4 ? 1 : 0;
    }
    const int different_actions =
        available_colors >= 3 ? (available_colors * (available_colors - 1) *
                                 (available_colors - 2)) /
                                    6
                              : (available_colors > 0 ? 1 : 0);
    const int different_taken = available_colors >= 3 ? 3 : available_colors;
    uint32_t action_upper =
        static_cast<uint32_t>(different_actions) *
        return_pattern_upper_bound(
            std::max(0, tokens + different_taken - Board::MAX_TOKENS));
    action_upper +=
        static_cast<uint32_t>(same_colors) *
        return_pattern_upper_bound(std::max(0, tokens + 2 - Board::MAX_TOKENS));

    if (player.can_reserve()) {
      int reserve_sources = 0;
      for (const auto &level : board.visible)
        for (int8_t card_id : level)
          reserve_sources += card_id >= 0 ? 1 : 0;
      for (const auto &deck : board.decks)
        reserve_sources += !deck.empty() ? 1 : 0;
      const int reserve_tokens = tokens + (board.bank[GOLD] > 0 ? 1 : 0);
      action_upper += static_cast<uint32_t>(reserve_sources) *
                      return_pattern_upper_bound(
                          std::max(0, reserve_tokens - Board::MAX_TOKENS));
    }

    auto add_purchase_options = [&](int8_t card_id) {
      if (card_id < 0 || action_upper >= MAX_MOVES)
        return;
      const Card &card = get_card(card_id);
      if (!player.can_afford(card))
        return;
      action_upper += static_cast<uint32_t>(payment_option_count(
          player, card, game.simple_payment_mode,
          static_cast<int>(MAX_MOVES -
                           std::min<uint32_t>(action_upper, MAX_MOVES))));
    };
    for (const auto &level : board.visible)
      for (int8_t card_id : level)
        add_purchase_options(card_id);
    for (int8_t card_id : player.reserved)
      add_purchase_options(card_id);
    return action_upper >= MAX_MOVES;
  }

  static uint64_t get_action_mask_bits_direct(const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    uint64_t mask = 0;

    std::array<uint8_t, 5> shortage_take{};
    int available_colors = 0;
    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] > 0) {
        shortage_take[color] = 1;
        ++available_colors;
      }
    }
    if (available_colors >= 3) {
      for (int index = 0; index < 10; ++index) {
        const auto &colors = TAKE_DIFF_COMBINATIONS[index];
        if (board.bank[colors[0]] > 0 && board.bank[colors[1]] > 0 &&
            board.bank[colors[2]] > 0)
          mask |= uint64_t{1} << index;
      }
    } else if (available_colors > 0) {
      const int index =
          action_encoder_detail::TakeDifferentCodec::find_index(shortage_take);
      if (index >= 0)
        mask |= uint64_t{1} << index;
    }

    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] >= 4)
        mask |= uint64_t{1} << (10 + color);
    }

    if (player.can_reserve()) {
      for (int level = 0; level < 3; ++level) {
        for (int slot = 0; slot < 4; ++slot) {
          if (board.visible[level][slot] >= 0)
            mask |= uint64_t{1} << (15 + level * 4 + slot);
        }
        if (!board.decks[level].empty())
          mask |= uint64_t{1} << (27 + level);
      }
    }

    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        const int8_t card_id = board.visible[level][slot];
        if (card_id >= 0 && player.can_afford(get_card(card_id)))
          mask |= uint64_t{1} << (30 + level * 4 + slot);
      }
    }
    for (int slot = 0; slot < 3; ++slot) {
      const int8_t card_id = player.reserved[slot];
      if (card_id >= 0 && player.can_afford(get_card(card_id)))
        mask |= uint64_t{1} << (42 + slot);
    }
    return mask;
  }

  static Action decode_direct(int index, const Game &game) {
    if (index < 0 || index >= 45)
      return Action{};
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    Action action;
    if (index < 10) {
      action.type = TAKE_DIFFERENT;
      int available_count = 0;
      for (int color = 0; color < 5; ++color)
        available_count += board.bank[color] > 0 ? 1 : 0;
      if (available_count >= 3) {
        for (uint8_t color : TAKE_DIFF_COMBINATIONS[index])
          action.take[color] = 1;
      } else {
        for (int color = 0; color < 5; ++color)
          action.take[color] = board.bank[color] > 0 ? 1 : 0;
      }
    } else if (index < 15) {
      action.type = TAKE_SAME;
      action.take[static_cast<size_t>(index - 10)] = 2;
    } else if (index < 27) {
      const int slot = index - 15;
      action.type = RESERVE_VISIBLE;
      action.card_id = board.visible[slot / 4][slot % 4];
    } else if (index < 30) {
      action.type = RESERVE_DECK;
      action.deck_level = static_cast<int8_t>(index - 27);
    } else if (index < 42) {
      const int slot = index - 30;
      action.type = PURCHASE;
      action.card_id = board.visible[slot / 4][slot % 4];
      player.can_afford(get_card(action.card_id), &action.gold_as);
    } else {
      action.type = PURCHASE;
      action.from_reserved = true;
      action.card_id = player.reserved[static_cast<size_t>(index - 42)];
      player.can_afford(get_card(action.card_id), &action.gold_as);
    }
    set_first_canonical_return(board, action);
    return action;
  }

  static void set_first_canonical_return(const Board &board, Action &action) {
    if (action.type == PURCHASE || action.type == VISIT_NOBLE ||
        action.type == ACTION_TYPE_COUNT)
      return;
    const std::array<uint8_t, 6> available =
        MoveGenerator::gems_after_action(board, action);
    int total = 0;
    for (uint8_t amount : available)
      total += amount;
    int remaining = std::max(0, total - Board::MAX_TOKENS);
    for (int color = 0; color < 6 && remaining > 0; ++color) {
      int suffix_capacity = 0;
      for (int suffix = color + 1; suffix < 6; ++suffix)
        suffix_capacity += available[suffix];
      const int amount = std::max(0, remaining - suffix_capacity);
      action.return_gems[color] = static_cast<uint8_t>(amount);
      remaining -= amount;
    }
  }
};

#endif // CSPLENDOR_ACTION_ENCODER_H
