// Oracle tests for performance fast paths that must stay bit-identical to the
// straightforward implementations they replace.
#include "game.h"
#include "portable_rng.h"
#include "rule_query.h"
#include "state_encoder.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <random>

namespace {

// Verbatim copy of the pre-optimisation StateEncoder implementation.
namespace reference_encoder {
float probability_at_least_one(size_t population, size_t successes,
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

  std::array<float, PUBLIC_CARD_FEATURE_SIZE>
  reference_public_card_statistics(const Game &game, int player,
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
} // namespace reference_encoder


void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "check failed: %s\n", what);
    std::abort();
  }
}

// LazyMt19937 must reproduce std::mt19937 for every seed and position,
// including outputs past the first lazily twisted prefix (227), the first
// full block (624) and later blocks.
void lazy_mt19937_matches_standard_engine() {
  std::mt19937_64 seeds(0x5eedULL);
  std::array<uint32_t, 8> fixed = {0U,          1U,          5489U,
                                   0x7fffffffU, 0x80000000U, 0xffffffffU,
                                   42U,         123456789U};
  for (int trial = 0; trial < 200; ++trial) {
    const uint32_t seed = trial < static_cast<int>(fixed.size())
                              ? fixed[trial]
                              : static_cast<uint32_t>(seeds());
    std::mt19937 reference(seed);
    LazyMt19937 lazy(seed);
    const int outputs = trial < 20 ? 2500 : 300;
    for (int index = 0; index < outputs; ++index)
      check(static_cast<uint32_t>(reference()) == lazy(), "lazy mt19937 stream");
  }
}

// Board::init and hidden-information shuffles must produce the same layout as
// the previous std::mt19937-based implementation.
void seeded_shuffles_match_standard_engine() {
  for (uint64_t seed : {0ULL, 1ULL, 7ULL, 0x123456789abcdefULL, ~0ULL}) {
    std::mt19937 reference(static_cast<std::mt19937::result_type>(seed));
    LazyMt19937 lazy(static_cast<uint32_t>(seed));
    std::array<int, 43> a{};
    std::array<int, 43> b{};
    for (int round = 0; round < 6; ++round) {
      for (size_t i = 0; i < a.size(); ++i)
        a[i] = b[i] = static_cast<int>(i);
      const size_t length = 2 + static_cast<size_t>(round) * 8;
      portable_mt19937_shuffle(a.begin(), a.begin() + length, reference);
      portable_mt19937_shuffle(b.begin(), b.begin() + length, lazy);
      check(a == b, "seeded shuffle layout");
    }
  }
}

int legacy_excess(const Board &board, const Action &action) {
  return csplendor::rules::required_token_return(
      csplendor::rules::gems_after_token_action(board, action));
}

// The scalar excess must equal the array-based oracle, including editor
// states whose uint8_t gem counts wrap on addition.
void scalar_token_excess_matches_array_oracle() {
  std::mt19937_64 rng(99);
  Board board;
  const std::array<ActionType, 6> types = {TAKE_DIFFERENT, TAKE_SAME,
                                           RESERVE_VISIBLE, RESERVE_DECK,
                                           PURCHASE,        PASS};
  for (int trial = 0; trial < 200000; ++trial) {
    board.current_player = static_cast<uint8_t>(rng() & 1U);
    PlayerState &player = board.players[board.current_player];
    const bool extreme = (trial % 7) == 0;
    for (int color = 0; color < 6; ++color)
      player.gems[color] = static_cast<uint8_t>(extreme ? rng() : rng() % 8);
    board.bank[GOLD] = static_cast<uint8_t>(rng() % 3);
    Action action;
    action.type = types[rng() % types.size()];
    for (int color = 0; color < 5; ++color)
      action.take[color] =
          static_cast<uint8_t>(extreme ? rng() : rng() % 3);
    check(csplendor::rules::token_excess_after_token_action(board, action) ==
              legacy_excess(board, action) ||
              (csplendor::rules::token_excess_after_token_action(board,
                                                                 action) <= 0 &&
               legacy_excess(board, action) == 0),
          "scalar token excess");
  }
}

// The allocation-free public card pool must equal observable_card_pool()
// element for element, so the float accumulation order is unchanged.
void sorted_card_pool_matches_observable_pool() {
  std::mt19937_64 rng(2024);
  for (int trial = 0; trial < 3000; ++trial) {
    Game game(trial);
    const int plies = static_cast<int>(rng() % 60);
    for (int ply = 0; ply < plies && !game.is_game_over(); ++ply)
      game.apply_random_action(rng());
    Board &board = game.board;
    if (trial % 5 == 0) {
      // Hide some reservations, and occasionally corrupt the editor state
      // with a duplicate deck card to exercise the sort fallback.
      for (auto &player : board.players)
        for (int slot = 0; slot < 3; ++slot)
          player.reserved_is_hidden[slot] = (rng() & 1U) != 0;
      if (trial % 15 == 0 && board.decks[0].size() >= 2)
        board.decks[0][0] = board.decks[0][1];
    }
    for (uint8_t observer = 0; observer < 2; ++observer) {
      for (int level = 1; level <= 3; ++level) {
        StateEncoder::SortedCardPool pool;
        StateEncoder::sorted_observable_card_pool(board, observer, level, pool);
        const auto expected = board.observable_card_pool(observer, level);
        check(pool.size() == expected.size(), "card pool size");
        for (size_t index = 0; index < expected.size(); ++index)
          check(pool[index] == expected[index], "card pool order");
      }
    }
  }
}

void public_card_statistics_match_reference() {
  std::mt19937_64 rng(77);
  for (int trial = 0; trial < 4000; ++trial) {
    Game game(trial + 17);
    const int plies = static_cast<int>(rng() % 70);
    for (int ply = 0; ply < plies && !game.is_game_over(); ++ply)
      game.apply_random_action(rng());
    if (trial % 4 == 0) {
      for (auto &player : game.board.players) {
        for (int slot = 0; slot < 3; ++slot)
          player.reserved_is_hidden[slot] = (rng() & 1U) != 0;
        // Editor-range values, deliberately without sync_packed().
        if (trial % 8 == 0)
          for (int color = 0; color < 6; ++color)
            player.gems[color] = static_cast<uint8_t>(rng());
        if (trial % 12 == 0)
          for (int color = 0; color < 5; ++color)
            player.bonuses[color] = static_cast<uint8_t>(rng());
      }
    }
    for (int player = 0; player < 2; ++player) {
      for (uint8_t observer = 0; observer < 2; ++observer) {
        const auto actual =
            StateEncoder::encode_public_card_statistics(game, player, observer);
        const auto expected = reference_encoder::reference_public_card_statistics(
            game, player, observer);
        check(std::memcmp(actual.data(), expected.data(),
                          sizeof(float) * actual.size()) == 0,
              "public card statistics bits");
      }
    }
  }
}

} // namespace

int main() {
  public_card_statistics_match_reference();
  sorted_card_pool_matches_observable_pool();
  lazy_mt19937_matches_standard_engine();
  seeded_shuffles_match_standard_engine();
  scalar_token_excess_matches_array_oracle();
  return 0;
}
