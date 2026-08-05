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

struct CardIdSet {
  uint64_t low = 0;
  uint64_t high = 0;

  void clear() noexcept;
  void add(int card_id) noexcept;
  bool contains(int card_id) const noexcept;
};

class HiddenOutcomeCatalog {
public:
  void remember_initial_position(const Game &game) noexcept;

  bool is_initially_known(int card_id) const noexcept;
  bool is_initially_hidden(int card_id) const noexcept;
  static bool is_claimed(const Board &board, int card_id) noexcept;
  CardIdSet unseen_cards(const Board &board) const noexcept;
  CardIdSet acquired_hidden_cards(const Board &board) const noexcept;

private:
  CardIdSet known_;
  CardIdSet hidden_;
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
