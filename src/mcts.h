#ifndef CSPLENDOR_MCTS_H
#define CSPLENDOR_MCTS_H

#include "action.h"
#include "game.h"
#include "mcts_types.h"
#include <array>
#include <cstdint>
#include <utility>

// Action encoder interface (for getting valid action masks)
class IActionEncoder {
public:
  virtual ~IActionEncoder() = default;
  virtual int encode(const Action &action, const Game &game) = 0;
  virtual Action decode(int action_idx, const Game &game) = 0;
  virtual std::array<uint8_t, MAX_ACTIONS>
  get_action_mask(const Game &game) = 0;

  // At a leaf, MCTSSearcher no longer uses the simulation Game after asking
  // for its mask. Implementations that can safely take ownership may override
  // this to avoid one final deep copy; the default preserves the const-ref
  // callback contract.
  virtual std::array<uint8_t, MAX_ACTIONS> get_action_mask_owned(Game &&game) {
    return get_action_mask(game);
  }
};

// Public facade: including mcts.h continues to expose all existing MCTS types
// and methods while the Game-independent tree and Game orchestration live in
// separate internal headers.
#include "mcts_tree.h"
#include "mcts_orchestration.h"

#endif // CSPLENDOR_MCTS_H
