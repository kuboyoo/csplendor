#ifndef CSPLENDOR_SOLVER_TAKE_GROUPS_H
#define CSPLENDOR_SOLVER_TAKE_GROUPS_H

#include "game.h"
#include <algorithm>
#include <array>
#include <vector>

namespace csplendor::solver_internal {

// Input is the existing capped legal generator, never a synthesized move.
// Only ordinary token holdings are eligible; editor overflow keeps the scan.
inline std::vector<uint64_t> group_take_candidates(
    const Game &game, std::vector<uint64_t> codes) {
  if (game.current_player() >= Board::NUM_PLAYERS || game.board.waiting_noble)
    return codes;
  int total = 0;
  for (int color = 0; color < 6; ++color) {
    total += game.board.players[game.current_player()].gems[color];
    if (game.board.bank[color] > 7)
      return codes;
  }
  if (total < 8 || total > 10)
    return codes;

  // Six signed deltas in [-3,2], encoded losslessly in six 3-bit fields.
  // This is a full delta comparison, NOT equality of a probabilistic hash.
  // Zero would mean all six deltas are -3, impossible for a generated take.
  struct Entry { uint32_t delta = 0; uint16_t index = 0; };
  std::array<Entry, 1024> table{}; // At most 10*56 + 5*21 take candidates.
  size_t retained = 0;
  for (uint64_t code : codes) {
    const Action action = Action::unpack(code);
    if (action.type != TAKE_DIFFERENT && action.type != TAKE_SAME) {
      codes[retained++] = code;
      continue;
    }
    uint32_t delta = 0;
    for (int color = 0; color < 6; ++color)
      delta |= static_cast<uint32_t>(
          (color < 5 ? action.take[color] : 0) - action.return_gems[color] + 3) << (3 * color);
    size_t slot = (delta * 2654435761U) >> 22;
    while (table[slot].delta && table[slot].delta != delta)
      slot = (slot + 1) & 1023;
    if (!table[slot].delta) {
      table[slot] = {delta, static_cast<uint16_t>(retained)};
      codes[retained++] = code;
    } else {
      // Both generated TAKE types have ActionOrderKey {2, 0, code}.
      auto &representative = codes[table[slot].index];
      representative = std::min(representative, code);
    }
  }
  codes.resize(retained);
  return codes;
}

} // namespace csplendor::solver_internal
#endif
