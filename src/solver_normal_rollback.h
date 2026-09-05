#ifndef CSPLENDOR_SOLVER_NORMAL_ROLLBACK_H
#define CSPLENDOR_SOLVER_NORMAL_ROLLBACK_H

#include "game.h"
#include "undo_record.h"
#include <cstdlib>
#include <variant>

namespace csplendor::solver_internal {

// Only for ordinary Game::apply_trusted(action, false), NOT arbitrary reveal
// erase/rotate or oracle transitions. Recursive descendants must restore before
// this guard. Neither Game's public history nor TT/search metadata is owned here.
class NormalBranchRollback {
  struct CompactState {
    csplendor::detail::UndoRecord rule;
    std::array<uint8_t, 3> deck_tops{};
    explicit CompactState(const Board &board) noexcept
        : rule(csplendor::detail::UndoRecord::capture(board)) {
      for (int level = 0; level < 3; ++level)
        if (!board.decks[level].empty())
          deck_tops[level] = board.decks[level].back();
    }
    void restore(Board &board) const noexcept {
      if (!rule.restore(board))
        std::abort(); // Broken append-only contract is not a refutation.
      // A popped top may have become inactive during a child's reveal patch.
      // Restore its value, not just the parent's count. Ordinary apply cannot
      // change the earlier prefix (nested guards own their child prefixes).
      for (int level = 0; level < 3; ++level)
        if (rule.deck_counts[level])
          board.decks[level].data[rule.deck_counts[level] - 1] = deck_tops[level];
    }
  };
  using State = std::variant<CompactState, Board>;

  static State capture(const Game &game, bool eligible) {
#ifdef CSPLENDOR_SOLVER_NORMAL_ROLLBACK
    if (eligible && game.board.current_player < Board::NUM_PLAYERS)
    {
      CSPLENDOR_PERF_INC(SolverCompactRollbackCaptures);
      return State(std::in_place_index<0>, game.board);
    }
#else
    (void)eligible;
#endif
    CSPLENDOR_PERF_INC(BoardSnapshotCopies);
    CSPLENDOR_PERF_INC(SolverFullRollbackCaptures);
    return State(std::in_place_index<1>, game.board);
  }

public:
  explicit NormalBranchRollback(Game &game, bool eligible = true)
      : game_(game), state_(capture(game, eligible)),
        blank_(game.blank_refill_mode), simple_(game.simple_payment_mode)
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
        , parent_(game.board), history_size_(game.history.size()),
        board_history_size_(game.board_history.size())
#endif
  {}
  NormalBranchRollback(const NormalBranchRollback &) = delete;
  NormalBranchRollback &operator=(const NormalBranchRollback &) = delete;
  ~NormalBranchRollback() { restore(); }

  bool mutated() const noexcept { return mutated_; }
  bool compact() const noexcept { return state_.index() == 0; }

  bool apply(const Action &action) {
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
    Game reference = game_.clone_light();
#endif
    mutated_ = true; // Before apply: false/exception may follow partial writes.
    const bool applied = game_.apply_trusted(action, false);
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
    const bool expected = reference.apply_trusted(action, false);
    if (applied != expected ||
        !csplendor::detail::UndoRecord::boards_equal(game_.board, reference.board))
      std::abort();
#endif
    return applied;
  }

  // For failure-injection tests of partial ordinary rule primitives.
  void mark_mutated() noexcept { mutated_ = true; }

  void restore() noexcept {
    if (!mutated_)
      return;
    if (compact()) {
      std::get<0>(state_).restore(game_.board);
      CSPLENDOR_PERF_INC(SolverCompactRollbackRestores);
    } else {
      const Board &parent = std::get<1>(state_);
      // Ordinary apply (including PASS's copy/move) cannot leave less capacity
      // than the original provenance sizes. Check before copy assignment so
      // this noexcept path cannot unexpectedly grow a vector.
      for (int player = 0; player < Board::NUM_PLAYERS; ++player)
        if (game_.board.players[player].purchased_cards.capacity() < parent.players[player].purchased_cards.size() ||
            game_.board.players[player].acquired_nobles.capacity() < parent.players[player].acquired_nobles.size())
          std::abort();
      game_.board = parent;
      CSPLENDOR_PERF_INC(SolverFullRollbackRestores);
    }
    game_.blank_refill_mode = blank_;
    game_.simple_payment_mode = simple_;
    mutated_ = false;
    CSPLENDOR_PERF_INC(BoardRestores);
    CSPLENDOR_PERF_INC(SolverBoardRollbacks);
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
    if (!csplendor::detail::UndoRecord::boards_equal(game_.board, parent_) ||
        game_.history.size() != history_size_ ||
        game_.board_history.size() != board_history_size_)
      std::abort();
#endif
  }

private:
  Game &game_;
  State state_;
  bool blank_;
  bool simple_;
  bool mutated_ = false;
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
  Board parent_;
  size_t history_size_;
  size_t board_history_size_;
#endif
};

} // namespace csplendor::solver_internal

#endif
