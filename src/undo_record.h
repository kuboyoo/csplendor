#ifndef CSPLENDOR_UNDO_RECORD_H
#define CSPLENDOR_UNDO_RECORD_H

#include "board.h"
#include <array>
#include <cstddef>

namespace csplendor {
namespace detail {

// Fixed-size candidate for a future delta-undo path. Production Game history
// continues to store complete Board snapshots in Phase 7.3; this record is
// exercised by dual-run tests to make the restoration domain explicit first.
//
// Normal Game transitions only append to purchased/acquired provenance and
// only pop from one deck. The record therefore retains provenance lengths and
// deck counts while copying every other mutable rule field that an action can
// change. Python/editor mutation between apply() and undo() remains one reason
// not to switch the production path without a separate compatibility decision.
struct UndoRecord {
  std::array<uint8_t, 6> bank{};
  std::array<std::array<int8_t, Board::CARDS_PER_LEVEL>, 3> visible{};
  std::array<uint8_t, 3> deck_counts{};
  FixedStack<uint8_t, Board::MAX_NOBLES_ON_BOARD> nobles;

  uint8_t player_index = 0;
  std::array<uint8_t, 6> gems{};
  uint64_t packed_gems = 0;
  std::array<uint8_t, 5> bonuses{};
  uint64_t packed_bonuses = 0;
  uint8_t points = 0;
  std::array<int8_t, Board::MAX_RESERVED> reserved{};
  std::array<bool, Board::MAX_RESERVED> reserved_is_hidden{};
  uint8_t reserved_count = 0;
  uint8_t purchased_count = 0;
  uint16_t noble_eligibility_mask = 0;
  size_t purchased_cards_size = 0;
  size_t acquired_nobles_size = 0;

  uint8_t current_player = 0;
  uint16_t turn = 0;
  bool final_round = false;
  bool waiting_noble = false;
  int8_t winner = -1;
  uint64_t cached_hash = 0;
  bool hash_valid = false;

  static UndoRecord capture(const Board &board) noexcept {
    UndoRecord record;
    record.bank = board.bank;
    record.visible = board.visible;
    for (int level = 0; level < 3; ++level)
      record.deck_counts[level] = board.decks[level].count;
    record.nobles = board.nobles;

    record.player_index = board.current_player;
    if (record.player_index >= Board::NUM_PLAYERS)
      return record;
    const PlayerState &player = board.players[record.player_index];
    record.gems = player.gems;
    record.packed_gems = player.packed_gems;
    record.bonuses = player.bonuses;
    record.packed_bonuses = player.packed_bonuses;
    record.points = player.points;
    record.reserved = player.reserved;
    record.reserved_is_hidden = player.reserved_is_hidden;
    record.reserved_count = player.reserved_count;
    record.purchased_count = player.purchased_count;
    record.noble_eligibility_mask = player.noble_eligibility_mask;
    record.purchased_cards_size = player.purchased_cards.size();
    record.acquired_nobles_size = player.acquired_nobles.size();

    record.current_player = board.current_player;
    record.turn = board.turn;
    record.final_round = board.final_round;
    record.waiting_noble = board.waiting_noble;
    record.winner = board.winner;
    record.cached_hash = board.cached_hash;
    record.hash_valid = board.hash_valid;
    return record;
  }

  bool can_restore(const Board &board) const noexcept {
    if (player_index >= Board::NUM_PLAYERS)
      return false;
    const PlayerState &player = board.players[player_index];
    if (player.purchased_cards.size() < purchased_cards_size ||
        player.acquired_nobles.size() < acquired_nobles_size)
      return false;
    for (int level = 0; level < 3; ++level) {
      if (board.decks[level].count > deck_counts[level])
        return false;
    }
    return true;
  }

  bool restore(Board &board) const noexcept {
    if (!can_restore(board))
      return false;

    board.bank = bank;
    board.visible = visible;
    for (int level = 0; level < 3; ++level)
      board.decks[level].count = deck_counts[level];
    board.nobles = nobles;

    PlayerState &player = board.players[player_index];
    player.gems = gems;
    player.packed_gems = packed_gems;
    player.bonuses = bonuses;
    player.packed_bonuses = packed_bonuses;
    player.points = points;
    player.reserved = reserved;
    player.reserved_is_hidden = reserved_is_hidden;
    player.reserved_count = reserved_count;
    player.purchased_count = purchased_count;
    player.noble_eligibility_mask = noble_eligibility_mask;
    // can_restore proved these are truncations. Erasing uint8_t tails cannot
    // allocate or throw; never grow a vector from a noexcept rollback.
    player.purchased_cards.erase(player.purchased_cards.begin() + purchased_cards_size,
                                 player.purchased_cards.end());
    player.acquired_nobles.erase(player.acquired_nobles.begin() + acquired_nobles_size,
                                 player.acquired_nobles.end());

    board.current_player = current_player;
    board.turn = turn;
    board.final_round = final_round;
    board.waiting_noble = waiting_noble;
    board.winner = winner;
    board.cached_hash = cached_hash;
    board.hash_valid = hash_valid;
    return true;
  }

  // Debug-only dual-run oracle: restore a copy of the post-action board and
  // compare it with the complete snapshot kept by the production history.
  // This deliberately checks provenance contents as well as their lengths.
  bool restores_snapshot(const Board &post_action,
                         const Board &snapshot) const {
    Board restored = post_action;
    return restore(restored) && boards_equal(restored, snapshot);
  }

  // Internal field-wise oracle, also used by solver transactions. Do not use
  // padding, vector object bytes or serialization-only equality for rollback.
  template <typename T, size_t Capacity>
  static bool stacks_equal(const FixedStack<T, Capacity> &lhs,
                           const FixedStack<T, Capacity> &rhs) {
    if (lhs.count != rhs.count)
      return false;
    for (size_t index = 0; index < lhs.size(); ++index) {
      if (lhs[index] != rhs[index])
        return false;
    }
    return true;
  }

  static bool players_equal(const PlayerState &lhs, const PlayerState &rhs) {
    return lhs.gems == rhs.gems && lhs.packed_gems == rhs.packed_gems &&
           lhs.bonuses == rhs.bonuses &&
           lhs.packed_bonuses == rhs.packed_bonuses &&
           lhs.points == rhs.points && lhs.reserved == rhs.reserved &&
           lhs.reserved_is_hidden == rhs.reserved_is_hidden &&
           lhs.reserved_count == rhs.reserved_count &&
           lhs.purchased_count == rhs.purchased_count &&
           lhs.purchased_cards == rhs.purchased_cards &&
           lhs.acquired_nobles == rhs.acquired_nobles &&
           lhs.noble_eligibility_mask == rhs.noble_eligibility_mask;
  }

  static bool boards_equal(const Board &lhs, const Board &rhs) {
    if (lhs.bank != rhs.bank || lhs.visible != rhs.visible ||
        !stacks_equal(lhs.nobles, rhs.nobles))
      return false;
    for (int level = 0; level < 3; ++level) {
      if (!stacks_equal(lhs.decks[level], rhs.decks[level]))
        return false;
    }
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      if (!players_equal(lhs.players[player], rhs.players[player]))
        return false;
    }
    return lhs.current_player == rhs.current_player &&
           lhs.turn == rhs.turn && lhs.final_round == rhs.final_round &&
           lhs.waiting_noble == rhs.waiting_noble &&
           lhs.winner == rhs.winner &&
           lhs.cached_hash == rhs.cached_hash &&
           lhs.hash_valid == rhs.hash_valid;
  }
};

} // namespace detail
} // namespace csplendor

#endif // CSPLENDOR_UNDO_RECORD_H
