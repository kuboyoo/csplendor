#ifndef CSPLENDOR_MATE_SCORE_BOUND_H
#define CSPLENDOR_MATE_SCORE_BOUND_H

#include "board.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace csplendor::solver_internal {

// An upper bound, not a route estimate. Give the attacker all remaining cards
// immediately, reusable cards, three wildcard tokens per nonpurchase turn,
// and no token/bank constraints. Retain colored holdings after a purchase,
// but still subtract its discounted price from a relaxed total wallet.
// Every real continuation embeds in this relaxation. Failure to reach 15 is
// therefore a sound bounded refutation; success proves absolutely nothing.
class MateScoreBound {
  struct Key {
    std::array<uint8_t, 11> resources{};
    uint64_t cards_low = 0, cards_high = 0;
    uint16_t nobles = 0;
    uint8_t points = 0, turns = 0;
    bool operator==(const Key &other) const {
      return resources == other.resources && cards_low == other.cards_low &&
             cards_high == other.cards_high && nobles == other.nobles &&
             points == other.points && turns == other.turns;
    }
  };
  struct Hash {
    size_t operator()(const Key &key) const {
      uint64_t hash = key.cards_low ^ (key.cards_high * 0x9e3779b97f4a7c15ULL);
      for (int value : key.resources)
        hash = (hash ^ value) * 1099511628211ULL;
      return hash ^ (uint64_t(key.nobles) << 24) ^ (uint64_t(key.points) << 8) ^
             key.turns;
    }
  };
  std::unordered_map<Key, bool, Hash> cache_;

  static int noble_score(const Board &board, const std::array<int, 5> &bonuses,
                         int turns) {
    std::array<int, NOBLE_COUNT> points{};
    int count = 0;
    for (int id : board.nobles) {
      const Noble &noble = get_noble(id);
      bool eligible = true;
      for (int color = 0; color < 5; ++color)
        eligible = eligible && bonuses[color] >= noble.requirement[color];
      if (eligible)
        points[count++] = noble.points;
    }
    std::sort(points.begin(), points.begin() + count, std::greater<int>());
    int score = 0;
    for (int i = 0; i < std::min(count, turns); ++i)
      score += points[i];
    return score;
  }

  static bool route(const Board &board, const std::vector<int> &cards,
                    const std::array<int, 6> &gems, std::array<int, 5> &bonuses,
                    int points, int purchases, int turns, int wallet,
                    int &work) {
    // Giving up on this bound means "possibly reachable", NEVER a refutation.
    if (++work > 4096)
      return true;
    if (points + noble_score(board, bonuses, turns) >= 15)
      return true;
    if (purchases == 0)
      return false;
    std::array<std::array<int, 6>, 5> cheapest;
    for (auto &row : cheapest)
      row.fill(10000);
    for (int id : cards) {
      const Card &card = get_card(id);
      int missing = 0, cost = 0;
      for (int color = 0; color < 5; ++color) {
        const int need = std::max(0, int(card.cost[color]) - bonuses[color]);
        cost += need;
        missing += std::max(0, need - gems[color]);
      }
      if (missing <= gems[GOLD] && cost <= wallet)
        cheapest[card.bonus][card.points] =
            std::min(cheapest[card.bonus][card.points], cost);
    }
    for (int color = 0; color < 5; ++color) {
      for (int gain = 5; gain >= 0; --gain) {
        if (cheapest[color][gain] > wallet)
          continue;
        ++bonuses[color];
        const bool possible =
            route(board, cards, gems, bonuses, points + gain, purchases - 1,
                  turns, wallet - cheapest[color][gain], work);
        --bonuses[color];
        if (possible)
          return true;
      }
    }
    return false;
  }

public:
  void clear() { cache_.clear(); }

  bool can_reach_15(const Board &board, int player_id, int turns,
                    bool &cache_hit) {
    const PlayerState &player = board.players[player_id];
    Key key;
    std::copy(player.gems.begin(), player.gems.end(), key.resources.begin());
    std::copy(player.bonuses.begin(), player.bonuses.end(),
              key.resources.begin() + 6);
    key.points = player.points;
    key.turns = turns;
    const auto add = [&](int card) {
      if (!is_valid_card_id(card))
        return;
      if (card < 64)
        key.cards_low |= uint64_t(1) << card;
      else
        key.cards_high |= uint64_t(1) << (card - 64);
    };
    for (const auto &row : board.visible)
      for (int card : row)
        add(card);
    for (const auto &deck : board.decks)
      for (int card : deck)
        add(card);
    for (int card : player.reserved)
      add(card);
    for (int id : board.nobles)
      key.nobles |= uint16_t(1) << id;
    const auto cached = cache_.find(key);
    cache_hit = cached != cache_.end();
    if (cache_hit)
      return cached->second;

    std::vector<int> cards;
    for (int id = 0; id < CARD_COUNT; ++id) {
      if (id < 64 ? (key.cards_low & (uint64_t(1) << id))
                  : (key.cards_high & (uint64_t(1) << (id - 64))))
        cards.push_back(id);
    }
    std::array<int, 5> bonuses{};
    std::copy(player.bonuses.begin(), player.bonuses.end(), bonuses.begin());
    bool possible = false;
    int work = 0;
    for (int purchases = turns; purchases >= 0 && !possible; --purchases) {
      std::array<int, 6> gems{};
      std::copy(player.gems.begin(), player.gems.end(), gems.begin());
      gems[GOLD] += 3 * (turns - purchases);
      int wallet = 0;
      for (int gem : gems)
        wallet += gem;
      possible = route(board, cards, gems, bonuses, player.points, purchases,
                       turns, wallet, work);
    }
    if (cache_.size() >= 131072)
      cache_.clear();
    cache_.emplace(key, possible);
    return possible;
  }
};

} // namespace csplendor::solver_internal
#endif
