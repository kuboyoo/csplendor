#ifndef CSPLENDOR_SOLVER_WIN_THRESHOLD_H
#define CSPLENDOR_SOLVER_WIN_THRESHOLD_H

#include "action.h"
#include "card_data.h"
#include "game.h"
#include "noble_data.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Score bounds shared by the mate solvers, including the last-move cut: outside
// the final round the attacker's last move can only win by ending the turn at
// 15 points or more (doc/mate_usage.md「最終手カット」).
namespace csplendor::solver_internal {

inline constexpr int WIN_POINTS = 15;

// Best noble that `bonuses` qualify for (granted at the end of any turn).
inline int max_noble_points(const Board &board,
                            const std::array<uint8_t, 5> &bonuses) {
  int points = 0;
  for (uint8_t noble_id : board.nobles) {
    const Noble &noble = get_noble(noble_id);
    bool eligible = true;
    for (int color = 0; color < 5; ++color)
      eligible = eligible && bonuses[color] >= noble.requirement[color];
    if (eligible)
      points = std::max(points, static_cast<int>(noble.points));
  }
  return points;
}

// Points after buying `card_id` and taking the best noble; the current points
// when the card is absent or unaffordable.
inline int score_after_purchase(const Board &board, const PlayerState &player,
                                int card_id) {
  if (!is_valid_card_id(card_id))
    return player.points;
  const Card &card = get_card(card_id);
  if (!player.can_afford(card))
    return player.points;
  std::array<uint8_t, 5> bonuses = player.bonuses;
  ++bonuses[card.bonus];
  return static_cast<int>(player.points) + card.points +
         max_noble_points(board, bonuses);
}

// Points the side to move holds when the turn that `action` finishes ends.
inline int points_at_turn_end(const Board &board, const PlayerState &player,
                              const Action &action) {
  if (action.type == VISIT_NOBLE)
    return static_cast<int>(player.points) +
           (is_valid_noble_id(action.noble_choice)
                ? static_cast<int>(get_noble(action.noble_choice).points)
                : 0);
  if (action.type == PURCHASE)
    return score_after_purchase(board, player, action.card_id);
  return static_cast<int>(player.points) +
         max_noble_points(board, player.bonuses);
}

// Whether `player` has a purchase (visible or reserved card) whose turn ends
// at WIN_POINTS or more.
inline bool has_winning_purchase(const Board &board, int player_index) {
  const PlayerState &player = board.players[player_index];
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      if (score_after_purchase(board, player, board.visible[level][slot]) >=
          WIN_POINTS)
        return true;
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
    if (score_after_purchase(board, player, player.reserved[slot]) >= WIN_POINTS)
      return true;
  return false;
}

// Whether the side to move can end this turn with WIN_POINTS or more.
inline bool can_win_this_turn(const Board &board) {
  const PlayerState &player = board.players[board.current_player];
  if (static_cast<int>(player.points) +
          max_noble_points(board, player.bonuses) >=
      WIN_POINTS)
    return true;
  if (board.waiting_noble || board.waiting_return)
    return false; // only the noble choice or the return is left
  return has_winning_purchase(board, board.current_player);
}

// ---------------------------------------------------------------------------
// Defender's last-reply cut (doc/mate_usage.md「守り側の最終応手カット」): at a
// defender node with one attacker move left, the attacker wins after every
// reply when a purchase the defender cannot take away ends the attacker's
// turn at WIN_POINTS or more and above the defender's reach.

using Bonuses = std::array<uint8_t, 5>;
using Gems = std::array<uint8_t, 6>;

// Card ids collected by the caller (hidden cards the defender may get).
struct CardList {
  std::array<uint8_t, CARD_COUNT> ids{};
  size_t count = 0;
  void add(int card_id) {
    if (count < ids.size())
      ids[count++] = static_cast<uint8_t>(card_id);
  }
  const uint8_t *begin() const noexcept { return ids.data(); }
  const uint8_t *end() const noexcept { return ids.data() + count; }
};

inline bool noble_eligible(const Noble &noble, const Bonuses &bonuses) {
  for (int color = 0; color < 5; ++color)
    if (bonuses[color] < noble.requirement[color])
      return false;
  return true;
}

// Bit per noble id: the nobles on the board that `bonuses` qualify for.
inline uint16_t eligible_noble_mask(const Board &board, const Bonuses &bonuses) {
  uint16_t mask = 0;
  for (uint8_t noble_id : board.nobles)
    if (noble_eligible(get_noble(noble_id), bonuses))
      mask |= static_cast<uint16_t>(1u << noble_id);
  return mask;
}

// Points of the two best nobles `bonuses` qualify for (one visit per turn,
// two turns): an upper bound on the nobles gained in two turns.
inline int top_two_noble_points(const Board &board, const Bonuses &bonuses) {
  int best = 0, second = 0;
  for (uint8_t noble_id : board.nobles) {
    const Noble &noble = get_noble(noble_id);
    if (!noble_eligible(noble, bonuses))
      continue;
    const int points = noble.points;
    if (points > best) {
      second = best;
      best = points;
    } else {
      second = std::max(second, points);
    }
  }
  return best + second;
}

// Noble points the attacker is sure to get after the defender took at most
// one noble out of `stealable`: the best eligible noble, or the second best
// when every best-scoring eligible noble can be taken and there is only one.
inline int guaranteed_noble_points(const Board &board, const Bonuses &bonuses,
                                   uint16_t stealable) {
  int best = 0, best_count = 0, second = 0;
  bool best_safe = false;
  for (uint8_t noble_id : board.nobles) {
    const Noble &noble = get_noble(noble_id);
    if (!noble_eligible(noble, bonuses))
      continue;
    const int points = noble.points;
    const bool safe = !(stealable & (1u << noble_id));
    if (points > best) {
      second = best;
      best = points;
      best_count = 1;
      best_safe = safe;
    } else if (points == best) {
      ++best_count;
      best_safe = best_safe || safe;
    } else {
      second = std::max(second, points);
    }
  }
  return (best_safe || best_count >= 2) ? best : second;
}

// Tokens still missing for `card` after bonuses, coloured gems and gold.
inline int token_deficit(const Gems &gems, const Bonuses &bonuses,
                         const Card &card) {
  int missing = 0;
  for (int color = 0; color < 5; ++color)
    missing += std::max(0, static_cast<int>(card.cost[color]) - bonuses[color] -
                               gems[color]);
  return std::max(0, missing - gems[GOLD]);
}

// Gems left after buying `card` (deficit 0) with as little gold as possible;
// gold kept in hand dominates any other payment for later purchases.
inline Gems gems_after_purchase(const Gems &gems, const Bonuses &bonuses,
                                const Card &card) {
  Gems left = gems;
  int gold = 0;
  for (int color = 0; color < 5; ++color) {
    const int need = std::max(0, static_cast<int>(card.cost[color]) - bonuses[color]);
    const int paid = std::min(need, static_cast<int>(gems[color]));
    left[color] = static_cast<uint8_t>(gems[color] - paid);
    gold += need - paid;
  }
  left[GOLD] = static_cast<uint8_t>(gems[GOLD] - gold);
  return left;
}

struct DefenderReach {
  int one_move = 0;     // D1: best turn-end score of one defender move
  uint16_t nobles = 0;  // nobles the defender may take with that move
};

// D1 and the stealable nobles over the defender's purchases available now
// (visible, reserved, `oracle_now` = hidden cards of blank-slot levels).
inline DefenderReach defender_one_move_reach(const Board &board, int defender,
                                             const CardList &oracle_now) {
  const PlayerState &player = board.players[defender];
  DefenderReach reach;
  reach.one_move = static_cast<int>(player.points) +
                   max_noble_points(board, player.bonuses);
  reach.nobles = eligible_noble_mask(board, player.bonuses);
  const auto consider = [&](int card_id) {
    if (!is_valid_card_id(card_id))
      return;
    const Card &card = get_card(card_id);
    if (token_deficit(player.gems, player.bonuses, card) > 0)
      return;
    Bonuses bonuses = player.bonuses;
    ++bonuses[card.bonus];
    reach.one_move =
        std::max(reach.one_move, static_cast<int>(player.points) + card.points +
                                     max_noble_points(board, bonuses));
    reach.nobles |= eligible_noble_mask(board, bonuses);
  };
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      consider(board.visible[level][slot]);
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
    consider(player.reserved[slot]);
  for (uint8_t card_id : oracle_now)
    consider(card_id);
  return reach;
}

// S*: the turn-end score the attacker reaches after any defender reply. One
// reply removes at most one visible card (a purchase or a reservation) and at
// most one noble, and never touches the attacker's gems, bonuses or
// reservations: the best candidate the defender cannot remove, or the second
// best candidate overall.
inline int attacker_guaranteed_score(const Board &board, int attacker,
                                     uint16_t stealable) {
  const PlayerState &player = board.players[attacker];
  const PlayerState &defender = board.players[1 - attacker];
  int best_safe = 0, best = 0, second = 0;
  const auto consider = [&](int card_id, bool visible) {
    if (!is_valid_card_id(card_id))
      return;
    const Card &card = get_card(card_id);
    if (!player.can_afford(card))
      return;
    Bonuses bonuses = player.bonuses;
    ++bonuses[card.bonus];
    const int score = static_cast<int>(player.points) + card.points +
                      guaranteed_noble_points(board, bonuses, stealable);
    const bool removable =
        visible && (defender.can_reserve() || defender.can_afford(card));
    if (!removable)
      best_safe = std::max(best_safe, score);
    if (score > best) {
      second = best;
      best = score;
    } else {
      second = std::max(second, score);
    }
  };
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
    consider(player.reserved[slot], false);
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      consider(board.visible[level][slot], true);
  return std::max(best_safe, second);
}

// Tokens one legal take may add towards `card`: one of each missing colour
// (three different) or two of one colour; the bank is ignored (upper bound).
inline int take_reduction(const Gems &gems, const Bonuses &bonuses,
                          const Card &card) {
  int missing_colors = 0, largest = 0;
  for (int color = 0; color < 5; ++color) {
    const int need = std::max(0, static_cast<int>(card.cost[color]) -
                                     bonuses[color] - gems[color]);
    if (need > 0) {
      ++missing_colors;
      largest = std::max(largest, need);
    }
  }
  return std::max(std::min(3, missing_colors), std::min(2, largest));
}

// Hidden cards per level (0-based) that may surface before the defender's
// second move: the deck arrays of the exact search, the hidden unclaimed
// cards of the blank search.
struct HiddenCards {
  CardList by_level[3];
};

// Whether `player_index` may hold `threshold` points or more after two of its
// moves, as an upper bound (D2 of the defender's last-reply cut, U of the
// attacker's reach cut). The first move is a purchase (visible, reserved,
// `oracle_now`) paid with the least gold, a reservation (one gold; any hidden
// card may be the reserved one) or a token take (`take_reduction`, bank
// ignored); the second purchase is any visible or reserved card, or a hidden
// card of a level that was refilled: by the first move's own purchase or
// reservation, or by the other side (`other_levels`, a bit per level).
// Nobles: the two best of the final bonuses (one visit per turn).
inline bool two_move_reach_at_least(const Board &board, int player_index,
                                    const CardList &oracle_now,
                                    const HiddenCards &hidden, int other_levels,
                                    int threshold) {
  const PlayerState &player = board.players[player_index];
  const int base = static_cast<int>(player.points);
  if (base + top_two_noble_points(board, player.bonuses) >= threshold)
    return true;
  constexpr int ALL_LEVELS = 0b111;
  // `slack`: tokens the reply may still add; with `take` the legal take of
  // the card itself.
  const auto second_purchase = [&](const Gems &gems, const Bonuses &bonuses,
                                   int points, int levels, int slack,
                                   bool take, int bought) {
    const auto reaches = [&](int card_id) {
      if (!is_valid_card_id(card_id) || card_id == bought)
        return false;
      const Card &card = get_card(card_id);
      const int allowed =
          take ? std::max(slack, take_reduction(gems, bonuses, card)) : slack;
      if (token_deficit(gems, bonuses, card) > allowed)
        return false;
      Bonuses after = bonuses;
      ++after[card.bonus];
      return points + card.points + top_two_noble_points(board, after) >=
             threshold;
    };
    for (int level = 0; level < 3; ++level)
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        if (reaches(board.visible[level][slot]))
          return true;
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      if (reaches(player.reserved[slot]))
        return true;
    for (int level = 0; level < 3; ++level)
      if (levels & (1 << level))
        for (uint8_t card_id : hidden.by_level[level])
          if (reaches(card_id))
            return true;
    return false;
  };
  // Token take: no refill of its own.
  if (second_purchase(player.gems, player.bonuses, base, other_levels, 0,
                      true, -1))
    return true;
  // Reservation: one gold, the reserved card may be any hidden card.
  if (second_purchase(player.gems, player.bonuses, base, ALL_LEVELS, 1, false,
                      -1))
    return true;
  const auto first_purchase = [&](int card_id, bool refills) {
    if (!is_valid_card_id(card_id))
      return false;
    const Card &card = get_card(card_id);
    if (token_deficit(player.gems, player.bonuses, card) > 0)
      return false;
    Bonuses bonuses = player.bonuses;
    ++bonuses[card.bonus];
    const int points = base + card.points;
    if (points + top_two_noble_points(board, bonuses) >= threshold)
      return true;
    const int levels =
        other_levels | (refills ? 1 << (card.level - 1) : 0);
    return second_purchase(gems_after_purchase(player.gems, player.bonuses, card),
                           bonuses, points, levels, 0, false, card_id);
  };
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      if (first_purchase(board.visible[level][slot], true))
        return true;
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
    if (first_purchase(player.reserved[slot], false))
      return true;
  for (uint8_t card_id : oracle_now)
    if (first_purchase(card_id, true)) // the slot stays blank
      return true;
  return false;
}

// Levels (a bit each) the attacker's winning purchase may refill: those of
// the visible cards that end its turn at WIN_POINTS or more.
inline int attacker_refill_levels(const Board &board, int attacker) {
  const PlayerState &player = board.players[attacker];
  int levels = 0;
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      if (score_after_purchase(board, player, board.visible[level][slot]) >=
          WIN_POINTS)
        levels |= 1 << level;
  return levels;
}

// The defender's last-reply cut at a node where the defender is to move
// outside the final round with no pending decision and the attacker has one
// move left: true when every reply loses to the attacker's next turn.
inline bool attacker_wins_after_any_reply(const Board &board, int attacker,
                                          const CardList &oracle_now,
                                          const HiddenCards &hidden) {
  if (!has_winning_purchase(board, attacker))
    return false;
  const int defender = 1 - attacker;
  const DefenderReach reach = defender_one_move_reach(board, defender, oracle_now);
  // The second player completes the round: a reply reaching 15 ends the game.
  if (attacker == 0 && reach.one_move >= WIN_POINTS)
    return false;
  const int score = attacker_guaranteed_score(board, attacker, reach.nobles);
  if (score < WIN_POINTS || score <= reach.one_move)
    return false;
  if (attacker == 1)
    return true; // the attacker's turn completes the round
  return !two_move_reach_at_least(board, defender, oracle_now, hidden,
                                  attacker_refill_levels(board, attacker), score);
}

// ---------------------------------------------------------------------------
// Attacker's reach cut (doc/mate_usage.md「攻め側の2手の到達上界による反証」):
// at an attacker node outside the final round with two moves left and no
// pending decision, the attacker cannot win when even the upper bound of its
// points after two turns stays below WIN_POINTS.

inline bool reach_cut_applies(const Game &game, int attacker, int depth) {
  return depth == 2 && game.current_player() == attacker &&
         !game.board.final_round && !game.board.waiting_noble &&
         !game.board.waiting_return;
}

// `hidden`: the cards that may surface (every level may be refilled by either
// side); the attacker never buys from a blank slot, so no `oracle_now`.
inline bool attacker_may_reach_win_in_two_moves(const Board &board,
                                                int attacker,
                                                const HiddenCards &hidden) {
  return two_move_reach_at_least(board, attacker, CardList{}, hidden, 0b111,
                                 WIN_POINTS);
}

// The last-move cut applies to the attacker with one move left outside the
// final round (the final round is resolved by its own exact rule).
inline bool last_move_cut_applies(const Game &game, int attacker, int depth) {
  return depth == 1 && game.current_player() == attacker &&
         !game.board.final_round;
}

// Whether moves other than a purchase may end the turn at WIN_POINTS: a
// pending noble choice or return, or a noble the mover already qualifies for.
inline bool last_move_needs_all_actions(const Board &board) {
  const PlayerState &player = board.players[board.current_player];
  return board.waiting_noble || board.waiting_return ||
         static_cast<int>(player.points) +
                 max_noble_points(board, player.bonuses) >=
             WIN_POINTS;
}

// The winning last moves (doc/mate_usage.md「最終手の直接生成」): the legal
// codes whose turn ends at WIN_POINTS or more, passed to `sink(code)`. They
// are generated directly as the purchases of the cards that reach WIN_POINTS,
// with the move generator's payment patterns, unless other moves may win too.
template <typename Sink>
void for_each_winning_last_move(const Game &game, Sink sink) {
  const Board &board = game.board;
  const PlayerState &player = board.players[board.current_player];
  if (last_move_needs_all_actions(board)) {
    for (uint64_t code : game.legal_action_codes())
      if (points_at_turn_end(board, player, Action::unpack(code)) >= WIN_POINTS)
        sink(code);
    return;
  }
  auto emit = [&](const Action &action) {
    sink(action.pack());
    return true;
  };
  const auto purchases = [&](int card_id, bool from_reserved) {
    if (score_after_purchase(board, player, card_id) >= WIN_POINTS)
      MoveGenerator::emit_card_purchases(board, card_id, from_reserved,
                                         game.simple_payment_mode, emit);
  };
  for (int level = 0; level < 3; ++level)
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      purchases(board.visible[level][slot], false);
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
    purchases(player.reserved[slot], true);
}

// Keeps the attacker's actions whose turn ends at WIN_POINTS or more (the
// visible-only search, whose actions are grouped representatives).
template <typename OrderedAction>
void keep_winning_last_moves(const Board &board,
                             std::vector<OrderedAction> &actions) {
  const PlayerState &player = board.players[board.current_player];
  actions.erase(std::remove_if(actions.begin(), actions.end(),
                               [&](const OrderedAction &ordered) {
                                 return points_at_turn_end(
                                            board, player,
                                            Action::unpack(ordered.code)) <
                                        WIN_POINTS;
                               }),
                actions.end());
}

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_WIN_THRESHOLD_H
