// Oracle tests for performance fast paths that must stay bit-identical to the
// straightforward implementations they replace.
#include "game.h"
#include "portable_rng.h"
#include "rule_query.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <random>

namespace {

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

} // namespace

int main() {
  lazy_mt19937_matches_standard_engine();
  seeded_shuffles_match_standard_engine();
  scalar_token_excess_matches_array_oracle();
  return 0;
}
