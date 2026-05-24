#ifndef CSPLENDOR_ACTION_ENCODER_H
#define CSPLENDOR_ACTION_ENCODER_H

#include "action.h"
#include "game.h"
#include <array>
#include <cstdint>

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
  static std::array<uint8_t, BASE_ACTION_COUNT> get_action_mask(const Game &game) {
    std::array<uint8_t, BASE_ACTION_COUNT> mask = {0};

    // Get legal actions from the game
    const auto &legal_actions = game.legal_actions();

    // Encode each legal action and mark in mask
    for (const Action &action : legal_actions) {
      int idx = encode(action, game);
      if (idx >= 0 && idx < BASE_ACTION_COUNT) {
        mask[idx] = 1;
      }
    }

    return mask;
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
    const auto &legal_actions = game.legal_actions();

    // Find all actions that encode to this index
    Action best_action;
    bool found = false;
    int best_gold_used = 999;
    int best_gems_returned = 999;

    for (const Action &action : legal_actions) {
      if (encode(action, game) == index) {
        if (!found) {
          best_action = action;
          found = true;

          // Calculate score for this action
          int gold_used = 0;
          int gems_returned = 0;
          for (int i = 0; i < 5; ++i) {
            gold_used += action.gold_as[i];
          }
          for (int i = 0; i < 6; ++i) {
            gems_returned += action.return_gems[i];
          }
          best_gold_used = gold_used;
          best_gems_returned = gems_returned;
        } else {
          // Compare with current best (minimize gold, then minimize returns)
          int gold_used = 0;
          int gems_returned = 0;
          for (int i = 0; i < 5; ++i) {
            gold_used += action.gold_as[i];
          }
          for (int i = 0; i < 6; ++i) {
            gems_returned += action.return_gems[i];
          }

          if (gold_used < best_gold_used ||
              (gold_used == best_gold_used &&
               gems_returned < best_gems_returned)) {
            best_action = action;
            best_gold_used = gold_used;
            best_gems_returned = gems_returned;
          }
        }
      }
    }

    return best_action;
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
    std::array<uint8_t, BASE_ACTION_COUNT> mask = {0};
    std::array<float, BASE_ACTION_COUNT> scores = {0};

    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    const auto &legal_actions = game.legal_actions();

    for (const Action &action : legal_actions) {
      int idx = encode(action, game);
      if (idx < 0 || idx >= BASE_ACTION_COUNT)
        continue;

      mask[idx] = 1;

      // Assign heuristic scores based on action type
      switch (action.type) {
      case PURCHASE: {
        // Get card and calculate CP
        const Card &card = get_card(action.card_id);
        float cp = calculate_card_cp(card, player);
        scores[idx] = 1.0f + cp * 2.0f;
        break;
      }
      case RESERVE_VISIBLE:
      case RESERVE_DECK:
        scores[idx] = 0.5f;
        break;
      case TAKE_DIFFERENT:
      case TAKE_SAME:
        scores[idx] = 0.2f;
        break;
      case VISIT_NOBLE:
        scores[idx] = 5.0f; // Noble visits are always good
        break;
      default:
        scores[idx] = 0.1f;
        break;
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
};

#endif // CSPLENDOR_ACTION_ENCODER_H
