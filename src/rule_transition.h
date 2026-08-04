#ifndef CSPLENDOR_RULE_TRANSITION_H
#define CSPLENDOR_RULE_TRANSITION_H

#include "move_generator.h"
#include <algorithm>
#include <array>

// Allocation-free rule-transition primitives shared by Game and native
// searchers.  Card/reveal selection and source removal deliberately remain in
// the caller: those are information-set policy, not ordinary rule mechanics.
namespace csplendor {
namespace detail {

inline void reserve_card_unchecked(Board &board, int card_id, bool hidden) {
  PlayerState &player = board.players[board.current_player];
  player.reserved_is_hidden[player.reserved_count] = hidden;
  player.reserved[player.reserved_count++] = static_cast<int8_t>(card_id);
}

inline bool grant_reserve_gold(Board &board) {
  if (board.bank[GOLD] == 0)
    return false;
  --board.bank[GOLD];
  ++board.players[board.current_player].gems[GOLD];
  return true;
}

inline void return_gems_unchecked(
    Board &board, const std::array<uint8_t, 6> &returned) {
  PlayerState &player = board.players[board.current_player];
  for (int color = 0; color < 6; ++color) {
    player.gems[color] -= returned[color];
    board.bank[color] += returned[color];
  }
  player.sync_packed_gems();
}

inline bool return_gems_checked(
    Board &board, const std::array<uint8_t, 6> &returned) {
  // Validate and mutate in color order to preserve the existing
  // partial-failure state; reveal traversal normally discards or restores the
  // failed branch.
  PlayerState &player = board.players[board.current_player];
  for (int color = 0; color < 6; ++color) {
    if (returned[color] > player.gems[color])
      return false;
    player.gems[color] -= returned[color];
    board.bank[color] += returned[color];
  }
  player.sync_packed_gems();
  return true;
}

template <bool ValidatePayment>
inline bool purchase_card(Board &board, const Card &card,
                          const std::array<uint8_t, 5> &gold_as) {
  // ValidatePayment intentionally retains the legacy validate-then-mutate
  // ordering per color; a later failure may therefore leave earlier colors
  // changed for the caller to roll back.
  PlayerState &player = board.players[board.current_player];
  int gold_used = 0;
  for (int color = 0; color < 5; ++color) {
    const int cost =
        std::max(0, static_cast<int>(card.cost[color]) -
                        static_cast<int>(player.bonuses[color]));
    const int from_gold = gold_as[color];
    const int from_gems = cost - from_gold;
    if constexpr (ValidatePayment) {
      if (from_gold > cost || from_gems > player.gems[color])
        return false;
    }
    player.gems[color] = static_cast<uint8_t>(
        static_cast<int>(player.gems[color]) - from_gems);
    board.bank[color] = static_cast<uint8_t>(
        static_cast<int>(board.bank[color]) + from_gems);
    gold_used += from_gold;
  }
  if constexpr (ValidatePayment) {
    if (gold_used > player.gems[GOLD])
      return false;
  }
  player.gems[GOLD] = static_cast<uint8_t>(
      static_cast<int>(player.gems[GOLD]) - gold_used);
  board.bank[GOLD] = static_cast<uint8_t>(
      static_cast<int>(board.bank[GOLD]) + gold_used);

  player.purchased_cards.push_back(card.id);
  ++player.purchased_count;
  ++player.bonuses[card.bonus];
  player.points += card.points;
  player.sync_packed();
  return true;
}

inline void acquire_noble_unchecked(Board &board, int noble_id) {
  PlayerState &player = board.players[board.current_player];
  player.points += get_noble(noble_id).points;
  player.acquired_nobles.push_back(static_cast<uint8_t>(noble_id));
  board.nobles.remove(static_cast<uint8_t>(noble_id));
}

inline void check_game_end(Board &board) {
  const int points0 = board.players[0].points;
  const int points1 = board.players[1].points;
  if (points0 != points1) {
    board.winner = points0 > points1 ? 0 : 1;
    return;
  }

  const int purchased0 = board.players[0].purchased_count;
  const int purchased1 = board.players[1].purchased_count;
  board.winner =
      purchased0 == purchased1 ? -2 : purchased0 < purchased1 ? 0 : 1;
}

inline void end_turn(Board &board) {
  if (!board.final_round && board.players[board.current_player].points >= 15)
    board.final_round = true;

  board.current_player = 1 - board.current_player;
  if (board.current_player != 0)
    return;

  ++board.turn;
  if (board.final_round)
    check_game_end(board);
}

inline void finish_standard_action(Board &board) {
  const auto eligible =
      MoveGenerator::get_eligible_nobles_fixed(board, board.current_player);
  if (eligible.size() > 1) {
    board.waiting_noble = true;
    return;
  }
  if (eligible.size() == 1)
    acquire_noble_unchecked(board, eligible[0]);
  end_turn(board);
}

} // namespace detail
} // namespace csplendor

#endif // CSPLENDOR_RULE_TRANSITION_H
