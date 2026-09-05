#include "action_encoder_v3.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
using Encoder = ActionEncoderV3;
using Gold = std::array<uint8_t, 5>;
void check(bool condition) {
  if (!condition) throw std::runtime_error("V3 payment oracle mismatch");
}
int index(const Gold &gold) {
  int n = 0;
  for (auto value : gold) n = n * 6 + value;
  return n;
}
bool valid(const Gold &gold, int card) {
  int sum = 0;
  for (size_t i = 0; i < gold.size(); ++i) {
    if (gold[i] > CARDS[card].cost[i]) return false;
    sum += gold[i];
  }
  return sum <= 5;
}
struct Oracle {
  std::array<std::array<int, 7776>, CARD_COUNT> rank;
  std::array<std::vector<Gold>, CARD_COUNT> patterns;
  Oracle() {
    for (int card = 0; card < CARD_COUNT; ++card) {
      rank[card].fill(-1);
      for (int n = 0; n < 7776; ++n) {
        int remaining = n;
        Gold gold{};
        for (int i = 4; i >= 0; --i) { gold[i] = remaining % 6; remaining /= 6; }
        if (valid(gold, card)) patterns[card].push_back(gold);
      }
      // Independent enumerative oracle: no composition DP or recursive count.
      std::sort(patterns[card].begin(), patterns[card].end(), [](const Gold &a, const Gold &b) {
        const int sa = std::accumulate(a.begin(), a.end(), 0), sb = std::accumulate(b.begin(), b.end(), 0);
        return sa != sb ? sa < sb : a < b;
      });
      for (size_t p = 0; p < patterns[card].size(); ++p) rank[card][index(patterns[card][p])] = static_cast<int>(p);
    }
  }
  int encode(const Gold &gold, int card) const {
    return valid(gold, card) ? rank[card][index(gold)] : -1;
  }
};
void exhaustive(const Oracle &oracle) {
  int total = 0;
  for (int card = 0; card < CARD_COUNT; ++card) {
    check(Encoder::CARD_PAYMENT_OFFSET[card] == total);
    const auto &patterns = oracle.patterns[card];
    check(patterns.size() == Encoder::CARD_PATTERN_COUNT[card]);
    check(Encoder::compute_pattern_count(card) == static_cast<int>(patterns.size()));
    total += static_cast<int>(patterns.size());
    for (int n = 0; n < 7776; ++n) {
      int remaining = n;
      Gold gold{};
      for (int i = 4; i >= 0; --i) { gold[i] = remaining % 6; remaining /= 6; }
      check(Encoder::encode_payment_for_card(gold, card) == oracle.rank[card][n]);
    }
    for (size_t p = 0; p < patterns.size(); ++p) {
      check(Encoder::decode_payment_for_card(static_cast<int>(p), card) == patterns[p]);
      // Every uint8 value in every component, with every valid surrounding
      // pattern, including sum overflow and >printed-cost boundaries.
      for (int pos = 0; pos < 5; ++pos)
        for (int value = 0; value <= 255; ++value) {
          auto gold = patterns[p]; gold[pos] = static_cast<uint8_t>(value);
          check(Encoder::encode_payment_for_card(gold, card) == oracle.encode(gold, card));
        }
    }
    for (int p : {std::numeric_limits<int>::min(), -1, static_cast<int>(patterns.size()), 256, std::numeric_limits<int>::max()})
      check(Encoder::decode_payment_for_card(p, card) == Gold{});
  }
  check(total == 2035 && Encoder::ACTION_SIZE == 3133);
  for (int card : {std::numeric_limits<int>::min(), -1, 90, 255, std::numeric_limits<int>::max()}) {
    check(Encoder::encode_payment_for_card(Gold{}, card) == -1);
    check(Encoder::decode_payment_for_card(0, card) == Gold{});
    check(Encoder::compute_pattern_count(card) == -1);
  }
}
void masks_and_ids(const Oracle &oracle) {
  const Game initial(42);
  for (int id = 0; id < Encoder::ACTION_SIZE; ++id) {
    const auto action = Encoder::decode(id, initial);
    check(Encoder::encode(action, initial) == id);
    if (action.type == PURCHASE) {
      const int p = id - Encoder::OFFSET_PURCHASE - Encoder::CARD_PAYMENT_OFFSET[action.card_id];
      check(action.gold_as == oracle.patterns[action.card_id][p]);
    }
  }
  for (uint64_t seed = 0; seed < 16; ++seed) {
    Game game(seed);
    for (int ply = 0; ply < 128 && !game.is_game_over(); ++ply) {
      const auto legal = MoveGenerator::generate_all(game.board, false);
      std::array<uint8_t, Encoder::ACTION_SIZE> expected{};
      for (const Action &action : legal) {
        int id = Encoder::encode(action, game);
        if (action.type == PURCHASE)
          id = Encoder::OFFSET_PURCHASE + Encoder::CARD_PAYMENT_OFFSET[action.card_id] + oracle.encode(action.gold_as, action.card_id);
        check(id >= 0 && id < Encoder::ACTION_SIZE);
        expected[id] = 1;
      }
      if (game.requires_forced_pass()) expected[Encoder::OFFSET_PASS] = 1;
      check(expected == Encoder::get_action_mask(game));
      if (legal.empty()) {
        Action pass; pass.type = PASS; check(game.apply(pass, false));
      } else {
        const Action action = legal[(seed + static_cast<uint64_t>(ply) * 37) % legal.size()];
        const int id = Encoder::encode(action, game);
        check(Encoder::decode_and_match(id, game).pack() == action.pack());
        check(game.apply(action, false));
      }
    }
  }
}
} // namespace
int main() {
  try {
    // Keep the 2.8 MB independent oracle off the stack (including ASan builds).
    const auto oracle = std::make_unique<Oracle>();
    exhaustive(*oracle); masks_and_ids(*oracle);
    std::cout << "699840 inputs / 2035 patterns / all uint8 components / 3133 IDs / reachable masks PASS\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
