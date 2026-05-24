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
          for (int f = 0; f < CARD_FEATURE_SIZE; ++f) {
            features[idx++] = 0.0f;
          }
        } else if (is_hidden) {
          // Hidden card: only encode tier/level
          const Card &card = get_card(card_id);
          for (int f = 0; f < CARD_FEATURE_SIZE - 1; ++f) {
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
        for (int f = 0; f < NOBLE_FEATURE_SIZE; ++f) {
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

private:
  static void encode_card(int8_t card_id, std::array<float, TOTAL_FEATURES> &features,
                          size_t &idx) {
    if (card_id == -1) {
      for (int f = 0; f < CARD_FEATURE_SIZE; ++f) {
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
