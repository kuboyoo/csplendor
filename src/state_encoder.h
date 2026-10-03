#ifndef CSPLENDOR_STATE_ENCODER_H
#define CSPLENDOR_STATE_ENCODER_H

#include "board.h"
#include "card_data.h"
#include "encoding_schema.h"
#include "game.h"
#include "noble_data.h"
#include <array>
#include <cstdint>

// Feature dimensions
static constexpr size_t CARD_FEATURE_SIZE =
    csplendor::encoding::StateFeatureV1::CARD_FEATURE_SIZE;
static constexpr size_t NOBLE_FEATURE_SIZE =
    csplendor::encoding::StateFeatureV1::NOBLE_FEATURE_SIZE;
static constexpr size_t PLAYER_FEATURE_SIZE =
    csplendor::encoding::StateFeatureV1::PLAYER_FEATURE_SIZE;
static constexpr size_t TOTAL_FEATURES =
    csplendor::encoding::StateFeatureV1::SIZE;
static constexpr size_t PUBLIC_CARD_LEVEL_FEATURE_SIZE =
    csplendor::encoding::StateFeatureV1::PUBLIC_CARD_LEVEL_FEATURE_SIZE;
static constexpr size_t PUBLIC_CARD_FEATURE_SIZE =
    csplendor::encoding::StateFeatureV1::PUBLIC_CARD_FEATURE_SIZE;

/**
 * C++ implementation of StateFeaturizer for encoding game state.
 * Supports observer-aware encoding to hide opponent's hidden reserved cards.
 */
namespace state_encoder_detail {
constexpr bool card_costs_fit_packed_sum() {
  for (const Card &card : CARDS)
    for (uint8_t cost : card.cost)
      if (cost > 15)
        return false;
  return true;
}
static_assert(card_costs_fit_packed_sum(),
              "packed_positive_sum assumes per-colour card costs below 16");
} // namespace state_encoder_detail

class StateEncoder {
public:
  using Schema = csplendor::encoding::StateFeatureV1;

  static constexpr uint32_t schema_version() noexcept {
    return Schema::VERSION;
  }

  static constexpr const char *schema_fingerprint() noexcept {
    return Schema::fingerprint();
  }

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
    size_t idx = Schema::OFFSET_BANK;

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
      // Swap the two player sections using the schema-owned offsets.
      for (size_t i = 0; i < PLAYER_FEATURE_SIZE; ++i) {
        std::swap(features[Schema::OFFSET_PLAYER_0 + i],
                  features[Schema::OFFSET_PLAYER_1 + i]);
      }
      // Flip current player indicator
      features[Schema::OFFSET_CURRENT_PLAYER] =
          1.0f - features[Schema::OFFSET_CURRENT_PLAYER];
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
      SortedCardPool pool;
      sorted_observable_card_pool(game.board, observer, level_index + 1, pool);
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

        // Pack from the arrays (not the cached packed fields) so editor
        // states with unsynchronised caches keep their exact semantics.
        const uint64_t packed_bonuses =
            cli::ResourceBundle::pack(target_player.bonuses);
        const uint64_t packed_gems = cli::ResourceBundle::pack(
            {target_player.gems[0], target_player.gems[1],
             target_player.gems[2], target_player.gems[3],
             target_player.gems[4]});
        for (size_t index = 0; index < pool_size; ++index) {
          const Card &card = get_card(pool[index]);
          // sum(max(0, cost - bonus)) and, because gems are non-negative,
          // sum(max(0, max(0, cost - bonus) - gems)) == sum(max(0, cost -
          // bonus - gems)).
          const int effective_cost =
              packed_positive_sum(card.packed_cost, packed_bonuses, 0);
          const int colored_shortfall = packed_positive_sum(
              card.packed_cost, packed_bonuses, packed_gems);
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

  using SortedCardPool = FixedStack<uint8_t, Board::MAX_DECK_SIZE +
                                                 Board::MAX_RESERVED>;

  // Same multiset and ascending order as Board::observable_card_pool(), built
  // without a heap allocation or comparison sort.  Valid card IDs are unique,
  // so a two-word bitset enumerates them in sorted order; editor states with
  // duplicate or out-of-range IDs keep the original sort-based path.
  static void sorted_observable_card_pool(const Board &board, uint8_t observer,
                                          int level, SortedCardPool &pool) {
    const auto &deck = board.decks[level - 1];
    const PlayerState &opponent = board.players[1 - observer];
    uint64_t bits[2] = {0, 0};
    size_t total = 0;
    bool canonical = deck.size() <= Board::MAX_DECK_SIZE;
    auto add = [&bits, &total, &canonical](int card_id) {
      ++total;
      if (card_id < 0 || card_id >= 128) {
        canonical = false;
        return;
      }
      const uint64_t bit = uint64_t{1} << (card_id & 63);
      uint64_t &word = bits[card_id >> 6];
      if (word & bit)
        canonical = false;
      word |= bit;
    };
    for (uint8_t card_id : deck)
      add(card_id);
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
      const int card_id = opponent.reserved[slot];
      if (opponent.reserved_is_hidden[slot] && is_valid_card_id(card_id) &&
          get_card(card_id).level == level)
        add(card_id);
    }

    pool.clear();
    if (!canonical || total > pool.capacity()) {
      const auto sorted = board.observable_card_pool(observer, level);
      if (sorted.size() > pool.capacity())
        throw std::logic_error("observable card pool exceeds capacity");
      for (uint8_t card_id : sorted)
        pool.push_back_unchecked(card_id);
      return;
    }
    for (int word = 0; word < 2; ++word) {
      uint64_t remaining = bits[word];
      while (remaining != 0) {
        const int bit = csplendor_ctz64(remaining);
        pool.push_back_unchecked(static_cast<uint8_t>(word * 64 + bit));
        remaining &= remaining - 1;
      }
    }
  }

private:
  // Sum over the five 12-bit fields of max(0, cost - subtract_a -
  // subtract_b).  Subtrahends are at most 2 * 255, so the 0x800 bias keeps
  // each field from borrowing into its neighbour; a positive difference is
  // bounded by the card cost (< 16), so the multiply-fold cannot carry.
  static int packed_positive_sum(uint64_t packed_cost, uint64_t subtract_a,
                                 uint64_t subtract_b) noexcept {
    using cli::ResourceBundle;
    const uint64_t biased =
        (packed_cost + ResourceBundle::BIT_11) - subtract_a - subtract_b;
    const uint64_t positive = (biased & ResourceBundle::BIT_11) >> 11;
    const uint64_t values = biased & (positive * 0x7FFULL);
    return static_cast<int>(((values * 0x0001001001001001ULL) >> 48) & 0xFFF);
  }

  static int csplendor_ctz64(uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(value);
#else
    int index = 0;
    while ((value & 1U) == 0) {
      value >>= 1;
      ++index;
    }
    return index;
#endif
  }

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
