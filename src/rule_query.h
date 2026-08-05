#ifndef CSPLENDOR_RULE_QUERY_H
#define CSPLENDOR_RULE_QUERY_H

#include "action.h"
#include "board.h"
#include <algorithm>
#include <array>

namespace csplendor::rules {

struct VisibleCardSource {
  int8_t level = -1;
  int8_t slot = -1;

  constexpr explicit operator bool() const noexcept { return level >= 0; }
};

inline VisibleCardSource find_visible_card_source(const Board &board,
                                                  int card_id) noexcept {
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      if (board.visible[level][slot] == card_id) {
        return {static_cast<int8_t>(level), static_cast<int8_t>(slot)};
      }
    }
  }
  return {};
}

inline int find_reserved_card_source(const PlayerState &player,
                                     int card_id) noexcept {
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
    if (player.reserved[slot] == card_id)
      return slot;
  }
  return -1;
}

inline std::array<int, 5> effective_card_cost(const PlayerState &player,
                                              const Card &card) noexcept {
  std::array<int, 5> result{};
  for (int color = 0; color < 5; ++color) {
    result[color] = std::max(0, static_cast<int>(card.cost[color]) -
                                    static_cast<int>(player.bonuses[color]));
  }
  return result;
}

inline bool has_no_token_return(const Action &action) noexcept {
  for (uint8_t returned : action.return_gems) {
    if (returned != 0)
      return false;
  }
  return true;
}

inline int token_total(const std::array<uint8_t, 6> &gems) noexcept {
  int result = 0;
  for (uint8_t count : gems)
    result += count;
  return result;
}

inline int required_token_return(const std::array<uint8_t, 6> &gems) noexcept {
  return std::max(0, token_total(gems) - Board::MAX_TOKENS);
}

inline bool
validate_token_return(const std::array<uint8_t, 6> &available,
                      const std::array<uint8_t, 6> &returned) noexcept {
  int returned_total = 0;
  for (int color = 0; color < 6; ++color) {
    if (returned[color] > available[color])
      return false;
    returned_total += returned[color];
  }
  return returned_total == required_token_return(available);
}

inline std::array<uint8_t, 6>
gems_after_token_action(const Board &board, const Action &action) noexcept {
  const PlayerState &player = board.players[board.current_player];
  std::array<uint8_t, 6> result = player.gems;
  if (action.type == TAKE_DIFFERENT || action.type == TAKE_SAME) {
    for (int color = 0; color < 5; ++color)
      result[color] += action.take[color];
  } else if (action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK) {
    if (board.bank[GOLD] > 0)
      ++result[GOLD];
  }
  return result;
}

inline bool
validate_purchase_payment(const PlayerState &player, const Card &card,
                          const std::array<uint8_t, 5> &gold_as) noexcept {
  const auto cost = effective_card_cost(player, card);
  int gold_used = 0;
  for (int color = 0; color < 5; ++color) {
    const int from_gold = gold_as[color];
    if (from_gold > cost[color] ||
        cost[color] - from_gold > player.gems[color]) {
      return false;
    }
    gold_used += from_gold;
  }
  return gold_used <= player.gems[GOLD];
}

using EligibleNobles = FixedStack<uint8_t, Board::MAX_NOBLES_ON_BOARD>;

inline EligibleNobles eligible_nobles(const Board &board,
                                      int player_index) noexcept {
  EligibleNobles result;
  if (player_index < 0 || player_index >= Board::NUM_PLAYERS)
    return result;

  const uint16_t mask = board.players[player_index].noble_eligibility_mask;
  for (uint8_t noble_id : board.nobles) {
    if (is_valid_noble_id(noble_id) &&
        (mask & (uint16_t(1) << noble_id)) != 0) {
      result.push_back_unchecked(noble_id);
    }
  }
  return result;
}

inline bool should_start_final_round(const Board &board) noexcept {
  return !board.final_round && board.current_player < Board::NUM_PLAYERS &&
         board.players[board.current_player].points >= 15;
}

inline int8_t winner_after_completed_round(const Board &board) noexcept {
  const int points0 = board.players[0].points;
  const int points1 = board.players[1].points;
  if (points0 != points1)
    return static_cast<int8_t>(points0 > points1 ? 0 : 1);

  const int purchased0 = board.players[0].purchased_count;
  const int purchased1 = board.players[1].purchased_count;
  return static_cast<int8_t>(purchased0 == purchased1  ? -2
                             : purchased0 < purchased1 ? 0
                                                       : 1);
}

} // namespace csplendor::rules

#endif // CSPLENDOR_RULE_QUERY_H
