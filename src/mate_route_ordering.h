#ifndef CSPLENDOR_MATE_ROUTE_ORDERING_H
#define CSPLENDOR_MATE_ROUTE_ORDERING_H

#include "board.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

namespace csplendor::solver_internal {

// A cheap *ordering heuristic*, never a bound or a proof. Project short solo
// purchase routes using the actual gems, discounts, reserve slots and nobles.
// Interference and every concrete refill are still checked by the AND/OR
// search.
class MateRouteOrdering {
  std::unordered_map<uint64_t, int> estimates_;

  static uint64_t estimate_key(const Board &board, int player_id, bool extended,
                               bool hidden_routes) {
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&](int value) {
      hash = (hash ^ static_cast<uint8_t>(value)) * 1099511628211ULL;
    };
    const PlayerState &player = board.players[player_id];
    for (int value : player.gems)
      mix(value);
    for (int value : player.bonuses)
      mix(value);
    for (int value : player.reserved)
      mix(value);
    mix(player.points);
    mix(player.reserved_count);
    for (const auto &row : board.visible)
      for (int value : row)
        mix(value);
    for (int value : board.nobles)
      mix(value);
    mix(255);
    for (int color = 0; color < 6; ++color)
      mix(board.bank[color] == 0 ? 0 : board.bank[color] >= 4 ? 2 : 1);
    mix(extended);
    mix(hidden_routes);
    if (hidden_routes) {
      uint64_t low = 0, high = 0;
      for (const auto &deck : board.decks)
        for (int card : deck)
          if (card < 64)
            low |= uint64_t(1) << card;
          else
            high |= uint64_t(1) << (card - 64);
      hash ^= low * 0x9e3779b97f4a7c15ULL;
      hash ^= high * 0xc2b2ae3d27d4eb4fULL;
    }
    return hash;
  }

  struct Runner {
    std::array<int, 6> gems{};
    std::array<int, 5> bonuses{};
    int points = 0;
    int reserved = 0;
    uint16_t nobles = 0;
  };

  static int purchase(const Board &board, Runner &runner, int card_id,
                      bool from_reserved) {
    const Card &card = get_card(card_id);
    std::array<int, 5> lack{};
    int missing = 0;
    for (int color = 0; color < 5; ++color) {
      const int cost =
          std::max(0, int(card.cost[color]) - runner.bonuses[color]);
      const int paid = std::min(cost, runner.gems[color]);
      runner.gems[color] -= paid;
      lack[color] = cost - paid;
      missing += lack[color];
    }
    const int gold = std::min(missing, runner.gems[GOLD]);
    runner.gems[GOLD] -= gold;
    missing -= gold;
    int collection = (missing + 2) / 3;
    for (int color = 0; color < 5; ++color) {
      const int gap = std::max(0, lack[color] - gold);
      const int per_take = board.bank[color] >= 4 ? 2 : 1;
      collection = std::max(collection, (gap + per_take - 1) / per_take);
      // An empty color needs a reserve for gold, or an opponent's payment.
      // The latter is deliberately only an estimate, not an impossibility.
      if (gap && board.bank[color] == 0)
        collection = std::max(
            collection, gap + (runner.reserved >= 3 || !board.bank[GOLD]));
    }
    ++runner.bonuses[card.bonus];
    runner.points += card.points;
    if (from_reserved)
      --runner.reserved;
    int best_noble = -1;
    for (uint8_t noble_id : board.nobles) {
      if (runner.nobles & (uint16_t(1) << noble_id))
        continue;
      const Noble &noble = get_noble(noble_id);
      bool eligible = true;
      for (int color = 0; color < 5; ++color)
        eligible =
            eligible && runner.bonuses[color] >= noble.requirement[color];
      if (eligible &&
          (best_noble < 0 || noble.points > get_noble(best_noble).points))
        best_noble = noble_id;
    }
    if (best_noble >= 0) {
      runner.points += get_noble(best_noble).points;
      runner.nobles |= uint16_t(1) << best_noble;
    }
    return collection + 1;
  }

public:
  void clear() { estimates_.clear(); }

  int distance(const Board &board, int player_id, bool extended = true,
               bool hidden_routes = false) {
    hidden_routes = hidden_routes && extended;
    const uint64_t key =
        estimate_key(board, player_id, extended, hidden_routes);
    const auto cached = estimates_.find(key);
    if (cached != estimates_.end())
      return cached->second;
    const int result =
        compute_distance(board, player_id, extended, hidden_routes);
    if (estimates_.size() >= 131072)
      estimates_.clear();
    // This bounded cache holds estimates, not proofs. Even a hash collision
    // can only change move ordering, never prune a move or establish a result.
    estimates_.emplace(key, result);
    return result;
  }

private:
  // Smaller is better. Fractional-turn tie breakers reward points and retained
  // resources. Multiple routes are examined so blocking one target is not
  // mistaken for blocking every way to win.
  static int compute_distance(const Board &board, int player_id, bool extended,
                              bool hidden_routes) {
    const PlayerState &player = board.players[player_id];
    Runner start;
    std::copy(player.gems.begin(), player.gems.end(), start.gems.begin());
    std::copy(player.bonuses.begin(), player.bonuses.end(),
              start.bonuses.begin());
    start.points = player.points;
    start.reserved = player.reserved_count;
    if (start.points >= 15)
      return -100 * (start.points - 15);

    std::array<int, 25> cards{};
    std::array<bool, 25> reserved{};
    std::array<bool, 25> hidden{};
    int count = 0;
    for (const auto &row : board.visible) {
      for (int card : row) {
        if (is_valid_card_id(card))
          cards[count++] = card;
      }
    }
    for (int card : player.reserved) {
      if (is_valid_card_id(card)) {
        reserved[count] = true;
        cards[count++] = card;
      }
    }
    if (hidden_routes) {
      // Nature can supply a useful hidden reservation to the defender. Model
      // the reserve turn and slot before buying, rather than assuming that
      // every future target must already be face up. This remains ONLY an
      // ordering heuristic; all actual moves/reveals are checked by the solver.
      std::array<std::vector<std::pair<int, int>>, 5> targets;
      for (const auto &deck : board.decks) {
        for (int id : deck) {
          const Card &card = get_card(id);
          int gap = 0;
          for (int color = 0; color < 5; ++color)
            gap += std::max(0, int(card.cost[color]) - start.bonuses[color] -
                                   start.gems[color]);
          gap = std::max(0, gap - start.gems[GOLD] - int(board.bank[GOLD] > 0));
          targets[card.bonus].emplace_back(gap * 100 - card.points * 25, id);
        }
      }
      for (auto &color : targets) {
        std::sort(color.begin(), color.end());
        for (size_t i = 0; i < std::min<size_t>(2, color.size()); ++i) {
          hidden[count] = true;
          reserved[count] = true;
          cards[count++] = color[i].second;
        }
      }
    }
    int best = 20000 + (15 - start.points) * 1000;
    int best_complete = std::numeric_limits<int>::max();
    const auto consider = [&](const Runner &runner, int turns) {
      // Beyond the short purchase horizon use a smooth optimistic continuation;
      // this is useful early in the game, but cannot refute a mate.
      const int remaining = std::max(0, 15 - runner.points);
      const int estimate =
          turns * 1000 + remaining * 650 - std::max(0, runner.points - 15) * 60;
      best = std::min(best, estimate);
      if (remaining == 0)
        best_complete = std::min(best_complete, estimate);
    };
    struct Route {
      Runner runner;
      int turns;
      uint32_t used;
      int priority;
    };
    // At most six competing routes, three purchases deep. This beam limits
    // only heuristic work: not a single legal search action is removed.
    std::vector<Route> beam{{start, 0, 0, 0}};
    for (int step = 0; step < (extended ? 3 : 1); ++step) {
      std::vector<Route> next;
      next.reserve(beam.size() * count);
      for (const Route &route : beam) {
        for (int card = 0; card < count; ++card) {
          if (route.used & (uint32_t(1) << card))
            continue;
          Route child = route;
          child.used |= uint32_t(1) << card;
          if (hidden[card]) {
            if (child.runner.reserved >= Board::MAX_RESERVED)
              continue;
            ++child.runner.reserved;
            child.runner.gems[GOLD] += int(board.bank[GOLD] > 0);
            ++child.turns;
          }
          child.turns +=
              purchase(board, child.runner, cards[card], reserved[card]);
          consider(child.runner, child.turns);
          if (child.runner.points >= 15)
            continue;
          int noble_gap = 10;
          for (uint8_t noble_id : board.nobles) {
            if (child.runner.nobles & (uint16_t(1) << noble_id))
              continue;
            int gap = 0;
            for (int color = 0; color < 5; ++color)
              gap += std::max(0, int(get_noble(noble_id).requirement[color]) -
                                     child.runner.bonuses[color]);
            noble_gap = std::min(noble_gap, gap);
          }
          child.priority = child.turns * 1000 +
                           (15 - child.runner.points) * 650 + noble_gap * 100;
          next.push_back(child);
        }
      }
      const size_t keep = std::min<size_t>(6, next.size());
      std::partial_sort(next.begin(), next.begin() + keep, next.end(),
                        [](const Route &left, const Route &right) {
                          return left.priority < right.priority ||
                                 (left.priority == right.priority &&
                                  left.used < right.used);
                        });
      next.resize(keep);
      beam = std::move(next);
    }
    return best_complete == std::numeric_limits<int>::max() ? best
                                                            : best_complete;
  }
};

} // namespace csplendor::solver_internal
#endif
