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

// Whether the side to move can end this turn with WIN_POINTS or more.
inline bool can_win_this_turn(const Board &board) {
  const PlayerState &player = board.players[board.current_player];
  if (static_cast<int>(player.points) +
          max_noble_points(board, player.bonuses) >=
      WIN_POINTS)
    return true;
  if (board.waiting_noble || board.waiting_return)
    return false; // only the noble choice or the return is left
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

// The last-move cut applies to the attacker with one move left outside the
// final round (the final round is resolved by its own exact rule).
inline bool last_move_cut_applies(const Game &game, int attacker, int depth) {
  return depth == 1 && game.current_player() == attacker &&
         !game.board.final_round;
}

// Keeps the attacker's actions whose turn ends at WIN_POINTS or more (attacker
// nodes never carry the defender's oracle actions).
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
