#ifndef CSPLENDOR_REVEAL_SOLVER_COMPONENTS_H
#define CSPLENDOR_REVEAL_SOLVER_COMPONENTS_H

#include "game.h"
#include "solver_types.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace csplendor::solver_internal {

namespace config {

#if defined(CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE) &&                  \
    defined(CSPLENDOR_INCREMENTAL_EXACT_HASH)
inline const volatile bool incremental_reveal_search_state_enabled = true;
#else
inline const volatile bool incremental_reveal_search_state_enabled = false;
#endif

} // namespace config

struct CardIdSet {
  uint64_t low = 0;
  uint64_t high = 0;

  void clear() noexcept;
  void add(int card_id) noexcept;
  void remove(int card_id) noexcept;
  bool contains(int card_id) const noexcept;
  bool operator==(const CardIdSet &other) const noexcept;
  bool operator!=(const CardIdSet &other) const noexcept;
};

class HiddenOutcomeCatalog {
public:
  void remember_initial_position(const Game &game) noexcept;

  bool is_initially_known(int card_id) const noexcept;
  bool is_initially_hidden(int card_id) const noexcept;
  static bool is_claimed(const Board &board, int card_id) noexcept;
  static CardIdSet claimed_cards(const Board &board) noexcept;
  CardIdSet unseen_cards(const Board &board) const noexcept;
  CardIdSet acquired_hidden_cards(const Board &board) const noexcept;

private:
  CardIdSet known_;
  CardIdSet hidden_;
};

// Solver-owned sidecar for canonical reveal-search roots.  It deliberately
// stays outside Board so editor/serialization/public ABI contracts remain
// unchanged.  Noncanonical roots leave active()==false and use the legacy
// scan-based path.
class RevealSearchState {
public:
  struct TransitionObservation {
    std::array<size_t, 3> deck_sizes = {0, 0, 0};
    std::array<int, 3> deck_tops = {-1, -1, -1};
    std::array<size_t, Board::NUM_PLAYERS> purchased_sizes = {0, 0};
  };

  bool initialize(const Game &game,
                  const HiddenOutcomeCatalog &catalog) noexcept;
  bool active() const noexcept;

  const std::array<CardIdSet, 3> &remaining_by_level() const noexcept;
  const CardIdSet &remaining_all() const noexcept;
  const CardIdSet &acquired_hidden() const noexcept;
  const CardIdSet &claimed() const noexcept;
  uint64_t rule_hash() const noexcept;

  bool is_claimed(const Board &board, int card_id) const noexcept;
  TransitionObservation observe_before(const Board &board) const noexcept;
  void observe_after(const TransitionObservation &before, const Board &board,
                     const HiddenOutcomeCatalog &catalog) noexcept;

  // Preserve the legacy erase-then-refill deck order while moving an exact
  // outcome to the ordinary top-pop position.  Both Board's exact hash and
  // this sidecar's deck component are adjusted by the same positional salts.
  bool move_deck_card_to_back(Board &board, int level,
                              int card_id) noexcept;

  bool matches_reference(const Board &board,
                         const HiddenOutcomeCatalog &catalog) const noexcept;
  void verify_or_abort(const Board &board,
                       const HiddenOutcomeCatalog &catalog) const noexcept;

private:
  static bool canonical_root(const Board &board) noexcept;
  static uint64_t deck_order_hash(const Board &board) noexcept;
  void deactivate() noexcept;
  void runtime_fallback(const char *reason) noexcept;

  std::array<CardIdSet, 3> remaining_by_level_{};
  CardIdSet remaining_all_;
  CardIdSet acquired_hidden_;
  CardIdSet claimed_;
  uint64_t rule_hash_ = 0;
  uint64_t deck_order_hash_ = 0;
  bool active_ = false;
};

struct OracleActionMetadata {
  int oracle_card = -1;
  bool oracle_reserve = false;
  int oracle_reserve_card = -1;
  int oracle_return_color = -1;
  std::array<uint8_t, 5> oracle_gold_as = {0};

  bool less_than(const OracleActionMetadata &other) const noexcept;
  bool is_oracle() const noexcept;
};

class ProofDagBuildAborted : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class RevealProofDagBuilder {
public:
  RevealProofDagBuilder(size_t node_limit, size_t edge_limit) noexcept;

  void reset() noexcept;
  size_t node_count() const noexcept;
  const std::vector<RevealVerifiedProofNode> &nodes() const noexcept;
  std::vector<RevealVerifiedProofNode> &nodes() noexcept;
  size_t append_node(RevealVerifiedProofNode node);
  void append_edge(size_t parent, RevealVerifiedProofEdge edge);
  std::vector<RevealVerifiedProofNode> release_nodes() noexcept;

private:
  size_t node_limit_ = 0;
  size_t edge_limit_ = 0;
  size_t edge_count_ = 0;
  std::vector<RevealVerifiedProofNode> nodes_;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_REVEAL_SOLVER_COMPONENTS_H
