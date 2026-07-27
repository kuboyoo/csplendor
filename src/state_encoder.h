#ifndef CSPLENDOR_STATE_ENCODER_H
#define CSPLENDOR_STATE_ENCODER_H

#include "board.h"
#include "card_data.h"
#include "game.h"
#include "noble_data.h"
#include <array>
#include <cstdint>

// Feature dimensions
static constexpr size_t CARD_FEATURE_SIZE = 8;   // points, cost[5], bonus, level
static constexpr size_t NOBLE_FEATURE_SIZE = 6;  // points, requirement[5]
static constexpr size_t PLAYER_FEATURE_SIZE = 36; // gems(6) + bonuses(5) + points(1) + reserved(3*8)
static constexpr size_t TOTAL_FEATURES = 196;    // 6 + 36 + 36 + 96 + 3 + 18 + 1
static constexpr size_t PUBLIC_CARD_LEVEL_FEATURE_SIZE = 39;
static constexpr size_t PUBLIC_CARD_FEATURE_SIZE =
    3 * PUBLIC_CARD_LEVEL_FEATURE_SIZE;

/**
 * C++ implementation of StateFeaturizer for encoding game state.
 * Supports observer-aware encoding to hide opponent's hidden reserved cards.
 */
class StateEncoder {
public:
  /**
   * Encode a game state into a feature vector.
   *
   * @param game The game state to encode
   * @param observer The player whose perspective to use (-1 for full info)
   * @return 196-element feature array
   */
  static std::array<float, TOTAL_FEATURES> encode(const Game &game,
                                                   int8_t observer = -1) {
    return encode_board(game.board, observer);
  }

  /**
   * Encode a board state into a feature vector.
   *
   * @param board The board state to encode
   * @param observer The player whose perspective to use (-1 for full info)
   * @return 196-element feature array
   */
  static std::array<float, TOTAL_FEATURES> encode_board(const Board &board,
                                                         int8_t observer = -1) {
    std::array<float, TOTAL_FEATURES> features = {0};
    size_t idx = 0;

    // 1. Bank gems (6 features)
    for (int i = 0; i < 6; ++i) {
      features[idx++] = static_cast<float>(board.bank[i]) / 7.0f;
    }

    // 2. Player features (36 features each, 2 players)
    for (int p = 0; p < 2; ++p) {
      const auto &player = board.players[p];

      // Gems (6)
      for (int i = 0; i < 6; ++i) {
        features[idx++] = static_cast<float>(player.gems[i]) / 10.0f;
      }

      // Bonuses (5)
      for (int i = 0; i < 5; ++i) {
        features[idx++] = static_cast<float>(player.bonuses[i]) / 10.0f;
      }

      // Points (1)
      features[idx++] = static_cast<float>(player.points) / 15.0f;

      // Reserved cards (3 cards * 8 features = 24)
      for (int r = 0; r < 3; ++r) {
        int8_t card_id = player.reserved[r];

        // Check if this card is hidden from the observer
        bool is_hidden = (observer != -1 && p != observer &&
                          player.reserved_is_hidden[r]);

        if (card_id == -1) {
          // No card in this slot
          for (size_t f = 0; f < CARD_FEATURE_SIZE; ++f) {
            features[idx++] = 0.0f;
          }
        } else if (is_hidden) {
          // Hidden card: only encode tier/level
          const Card &card = get_card(card_id);
          for (size_t f = 0; f < CARD_FEATURE_SIZE - 1; ++f) {
            features[idx++] = 0.0f; // Hide all details except level
          }
          features[idx++] = static_cast<float>(card.level) / 3.0f;
        } else {
          // Visible card: encode full details
          encode_card(card_id, features, idx);
        }
      }
    }

    // 3. Visible cards (12 cards * 8 features = 96)
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        int8_t card_id = board.visible[level][slot];
        encode_card(card_id, features, idx);
      }
    }

    // 4. Deck counts (3)
    for (int level = 0; level < 3; ++level) {
      features[idx++] = static_cast<float>(board.decks[level].size()) / 40.0f;
    }

    // 5. Nobles (3 nobles * 6 features = 18)
    for (size_t i = 0; i < 3; ++i) {
      if (i < board.nobles.size()) {
        encode_noble(board.nobles[i], features, idx);
      } else {
        for (size_t f = 0; f < NOBLE_FEATURE_SIZE; ++f) {
          features[idx++] = 0.0f;
        }
      }
    }

    // 6. Current player (1)
    features[idx++] = static_cast<float>(board.current_player);

    return features;
  }

  /**
   * Encode with player perspective swap (for canonical form).
   * When player == 1, swaps player 0 and player 1 features.
   */
  static std::array<float, TOTAL_FEATURES>
  encode_canonical(const Game &game, int player, int8_t observer = -1) {
    auto features = encode(game, observer);

    if (player == 1) {
      // Swap player 0 (indices 6:42) and player 1 (indices 42:78) features
      for (size_t i = 0; i < PLAYER_FEATURE_SIZE; ++i) {
        std::swap(features[6 + i], features[42 + i]);
      }
      // Flip current player indicator
      features[195] = 1.0f - features[195];
    }

    return features;
  }

  /**
   * Encode observer-safe posterior summaries for future card reveals.
   *
   * The unknown pool for a tier is the physical deck plus the opponent's
   * hidden reservations in that tier. Their allocation is private, but the
   * union is derivable from public card history and is determinization-stable.
   *
   * Each tier contributes:
   *   pool/hidden/deck fractions (3), bonus probabilities (5),
   *   point probabilities 0..5 (6), expected printed costs (5), and,
   *   for the perspective player then the opponent, probabilities that the
   *   next one/three reveals have payment distance <= 0..3 plus expected
   *   distance and point efficiency (10 each).
   */
  static std::array<float, PUBLIC_CARD_FEATURE_SIZE>
  encode_public_card_statistics(const Game &game, int player,
                                uint8_t observer) {
    if (player < 0 || player >= Board::NUM_PLAYERS)
      throw std::invalid_argument("player must identify a player");
    if (observer >= Board::NUM_PLAYERS)
      throw std::invalid_argument("observer must identify a player");

    constexpr std::array<int, 3> LEVEL_CARD_COUNTS = {40, 30, 20};
    std::array<float, PUBLIC_CARD_FEATURE_SIZE> features = {0.0f};
    size_t feature_index = 0;

    for (int level_index = 0; level_index < 3; ++level_index) {
      const auto &deck = game.board.decks[level_index];
      const auto pool =
          game.board.observable_card_pool(observer, level_index + 1);
      const size_t pool_size = pool.size();
      const size_t hidden_count = pool_size - deck.size();

      const float denominator =
          pool_size == 0 ? 1.0f : static_cast<float>(pool_size);
      const float level_count =
          static_cast<float>(LEVEL_CARD_COUNTS[level_index]);
      features[feature_index++] = static_cast<float>(pool_size) / level_count;
      features[feature_index++] =
          static_cast<float>(hidden_count) / Board::MAX_RESERVED;
      features[feature_index++] =
          static_cast<float>(deck.size()) / level_count;

      std::array<int, 5> bonus_counts = {0};
      std::array<int, 6> point_counts = {0};
      std::array<int, 5> cost_sums = {0};
      for (size_t index = 0; index < pool_size; ++index) {
        const Card &card = get_card(pool[index]);
        if (card.bonus < bonus_counts.size())
          ++bonus_counts[card.bonus];
        const int point_bucket = std::min(5, static_cast<int>(card.points));
        ++point_counts[point_bucket];
        for (int color = 0; color < 5; ++color)
          cost_sums[color] += card.cost[color];
      }
      for (int count : bonus_counts)
        features[feature_index++] = static_cast<float>(count) / denominator;
      for (int count : point_counts)
        features[feature_index++] = static_cast<float>(count) / denominator;
      for (int sum : cost_sums)
        features[feature_index++] =
            static_cast<float>(sum) / (denominator * 7.0f);

      for (int offset = 0; offset < Board::NUM_PLAYERS; ++offset) {
        const PlayerState &target_player =
            game.board.players[offset == 0 ? player : 1 - player];
        std::array<int, 4> reachable_counts = {0};
        float distance_sum = 0.0f;
        float efficiency_sum = 0.0f;

        for (size_t index = 0; index < pool_size; ++index) {
          const Card &card = get_card(pool[index]);
          int colored_shortfall = 0;
          int effective_cost = 0;
          for (int color = 0; color < 5; ++color) {
            const int discounted =
                std::max(0, static_cast<int>(card.cost[color]) -
                                static_cast<int>(target_player.bonuses[color]));
            effective_cost += discounted;
            colored_shortfall +=
                std::max(0, discounted -
                                static_cast<int>(target_player.gems[color]));
          }
          const int distance =
              std::max(0, colored_shortfall -
                              static_cast<int>(target_player.gems[GOLD]));
          distance_sum += static_cast<float>(distance);
          const float efficiency =
              effective_cost == 0
                  ? static_cast<float>(card.points)
                  : static_cast<float>(card.points) / effective_cost;
          efficiency_sum += std::min(5.0f, efficiency);
          for (int threshold = 0; threshold <= 3; ++threshold) {
            if (distance <= threshold)
              ++reachable_counts[threshold];
          }
        }

        const int reveal_count =
            std::min(3, static_cast<int>(deck.size()));
        for (int threshold = 0; threshold <= 3; ++threshold) {
          features[feature_index++] =
              static_cast<float>(reachable_counts[threshold]) / denominator;
        }
        for (int threshold = 0; threshold <= 3; ++threshold) {
          features[feature_index++] =
              probability_at_least_one(pool_size,
                                       reachable_counts[threshold],
                                       reveal_count);
        }
        features[feature_index++] =
            distance_sum / (denominator * 15.0f);
        features[feature_index++] =
            efficiency_sum / (denominator * 5.0f);
      }
    }

    if (feature_index != features.size())
      throw std::logic_error("public card feature size mismatch");
    return features;
  }

private:
  static float probability_at_least_one(size_t population, size_t successes,
                                        int draws) {
    if (population == 0 || successes == 0 || draws <= 0)
      return 0.0f;
    if (successes >= population)
      return 1.0f;
    draws = std::min(draws, static_cast<int>(population));
    double none = 1.0;
    for (int draw = 0; draw < draws; ++draw) {
      const size_t failures = population - successes;
      if (static_cast<size_t>(draw) >= failures)
        return 1.0f;
      none *= static_cast<double>(failures - draw) /
              static_cast<double>(population - draw);
    }
    return static_cast<float>(1.0 - none);
  }

  static void encode_card(int8_t card_id, std::array<float, TOTAL_FEATURES> &features,
                          size_t &idx) {
    if (card_id == -1) {
      for (size_t f = 0; f < CARD_FEATURE_SIZE; ++f) {
        features[idx++] = 0.0f;
      }
      return;
    }

    const Card &card = get_card(card_id);

    // points
    features[idx++] = static_cast<float>(card.points) / 5.0f;

    // cost (5 colors)
    for (int i = 0; i < 5; ++i) {
      features[idx++] = static_cast<float>(card.cost[i]) / 7.0f;
    }

    // bonus type
    features[idx++] = static_cast<float>(card.bonus) / 5.0f;

    // level
    features[idx++] = static_cast<float>(card.level) / 3.0f;
  }

  static void encode_noble(uint8_t noble_id,
                           std::array<float, TOTAL_FEATURES> &features,
                           size_t &idx) {
    const Noble &noble = get_noble(noble_id);

    // points
    features[idx++] = static_cast<float>(noble.points) / 3.0f;

    // requirements (5 colors)
    for (int i = 0; i < 5; ++i) {
      features[idx++] = static_cast<float>(noble.requirement[i]) / 4.0f;
    }
  }
};

#endif // CSPLENDOR_STATE_ENCODER_H
