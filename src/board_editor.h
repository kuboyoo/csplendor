#ifndef CSPLENDOR_BOARD_EDITOR_H
#define CSPLENDOR_BOARD_EDITOR_H

#include "board.h"
#include "card_data.h"
#include "noble_data.h"
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace csplendor::state::editor {

// Validated mutation surface for Python and tooling. Every function validates
// and prepares the complete payload before entering Board's editor gateway.
// std::invalid_argument maps to Python ValueError and std::out_of_range maps
// to Python IndexError through pybind11's standard exception translation.
inline void set_turn(Board &board, int turn) {
  if (turn < 0 || turn > 65535)
    throw std::invalid_argument("turn out of range");
  board.begin_editor_mutation().turn = static_cast<uint16_t>(turn);
}

inline void set_current_player(Board &board, int player) {
  if (player < 0 || player >= Board::NUM_PLAYERS)
    throw std::out_of_range("current_player out of range");
  board.begin_editor_mutation().current_player = static_cast<uint8_t>(player);
}

inline void set_bank(Board &board, const std::array<uint8_t, 6> &bank) {
  board.begin_editor_mutation().bank = bank;
}

inline void set_visible(Board &board,
                        const std::vector<std::vector<int>> &visible) {
  if (visible.size() != 3)
    throw std::invalid_argument("visible must have 3 levels");

  decltype(board.visible) prepared{};
  for (size_t level = 0; level < prepared.size(); ++level) {
    if (visible[level].size() != Board::CARDS_PER_LEVEL)
      throw std::invalid_argument("each visible level must have 4 slots");
    for (size_t slot = 0; slot < prepared[level].size(); ++slot) {
      const int card_id = visible[level][slot];
      if (card_id != -1 && !is_valid_card_id(card_id))
        throw std::invalid_argument("visible contains invalid card id");
      prepared[level][slot] = static_cast<int8_t>(card_id);
    }
  }

  board.begin_editor_mutation().visible = prepared;
}

inline void set_nobles(Board &board, const std::vector<int> &nobles) {
  if (nobles.size() > Board::MAX_NOBLES_ON_BOARD)
    throw std::invalid_argument("too many nobles");

  FixedStack<uint8_t, Board::MAX_NOBLES_ON_BOARD> prepared;
  for (int noble_id : nobles) {
    if (!is_valid_noble_id(noble_id))
      throw std::invalid_argument("nobles contains invalid noble id");
    if (!prepared.try_push_back(static_cast<uint8_t>(noble_id)))
      throw std::length_error("too many nobles");
  }

  board.begin_editor_mutation().nobles = prepared;
}

inline void set_decks(Board &board,
                      const std::vector<std::vector<int>> &decks) {
  if (decks.size() != 3)
    throw std::invalid_argument("decks must have 3 levels");

  std::array<FixedStack<uint8_t, Board::MAX_DECK_SIZE>, 3> prepared;
  for (size_t level = 0; level < prepared.size(); ++level) {
    if (decks[level].size() > Board::MAX_DECK_SIZE)
      throw std::invalid_argument("deck level exceeds max size");
    for (int card_id : decks[level]) {
      if (!is_valid_card_id(card_id) ||
          get_card(card_id).level != static_cast<int>(level) + 1) {
        throw std::invalid_argument("decks contains invalid card id");
      }
      if (!prepared[level].try_push_back(static_cast<uint8_t>(card_id)))
        throw std::length_error("deck level exceeds max size");
    }
  }

  Board &target = board.begin_editor_mutation();
  for (size_t level = 0; level < prepared.size(); ++level)
    target.decks[level] = prepared[level];
}

inline void set_final_round(Board &board, bool final_round) noexcept {
  board.begin_editor_mutation().final_round = final_round;
}

inline void set_waiting_noble(Board &board, bool waiting_noble) noexcept {
  board.begin_editor_mutation().waiting_noble = waiting_noble;
}

inline void set_winner(Board &board, int winner) {
  if (winner < -2 || winner > 1)
    throw std::invalid_argument("winner out of range");
  board.begin_editor_mutation().winner = static_cast<int8_t>(winner);
}

inline void set_player(Board &board, int player_index,
                       const PlayerState &player) {
  if (player_index < 0 || player_index >= Board::NUM_PLAYERS)
    throw std::out_of_range("player index out of range");

  // Copy and normalize before invalidating the destination. Vector allocation
  // failure therefore leaves both Board state and a valid cached hash intact.
  PlayerState prepared = player;
  prepared.sync_packed();
  board.begin_editor_mutation().players[player_index] = std::move(prepared);
}

} // namespace csplendor::state::editor

#endif // CSPLENDOR_BOARD_EDITOR_H
