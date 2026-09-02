#ifndef CSPLENDOR_RULE_TRANSITION_H
#define CSPLENDOR_RULE_TRANSITION_H

#include "perf_counters.h"
#include "rule_query.h"
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
  const auto costs = rules::effective_card_cost(player, card);
  int gold_used = 0;
  for (int color = 0; color < 5; ++color) {
    const int cost = costs[color];
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

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  const size_t purchased_capacity_before = player.purchased_cards.capacity();
#endif
  player.purchased_cards.push_back(card.id);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  if (player.purchased_cards.capacity() != purchased_capacity_before)
    CSPLENDOR_PERF_INC(PurchasedCardVectorReallocations);
#endif
  ++player.purchased_count;
  ++player.bonuses[card.bonus];
  player.points += card.points;
  player.sync_packed();
  return true;
}

inline void acquire_noble_unchecked(Board &board, int noble_id) {
  PlayerState &player = board.players[board.current_player];
  player.points += get_noble(noble_id).points;
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  const size_t noble_capacity_before = player.acquired_nobles.capacity();
#endif
  player.acquired_nobles.push_back(static_cast<uint8_t>(noble_id));
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  if (player.acquired_nobles.capacity() != noble_capacity_before)
    CSPLENDOR_PERF_INC(AcquiredNobleVectorReallocations);
#endif
  board.nobles.remove(static_cast<uint8_t>(noble_id));
}

inline void check_game_end(Board &board) {
  board.winner = rules::winner_after_completed_round(board);
}

inline void end_turn(Board &board) {
  if (rules::should_start_final_round(board))
    board.final_round = true;

  board.current_player = 1 - board.current_player;
  if (board.current_player != 0)
    return;

  ++board.turn;
  if (board.final_round)
    check_game_end(board);
}

inline void finish_standard_action(Board &board) {
  const auto eligible = rules::eligible_nobles(board, board.current_player);
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
