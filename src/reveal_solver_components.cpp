#include "reveal_solver_components.h"

#include "card_data.h"
#include <algorithm>
#include <tuple>
#include <utility>

namespace csplendor::solver_internal {

void CardIdSet::clear() noexcept {
  low = 0;
  high = 0;
}

void CardIdSet::add(int card_id) noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return;
  if (card_id < 64)
    low |= uint64_t{1} << card_id;
  else
    high |= uint64_t{1} << (card_id - 64);
}

bool CardIdSet::contains(int card_id) const noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return false;
  if (card_id < 64)
    return (low & (uint64_t{1} << card_id)) != 0;
  return (high & (uint64_t{1} << (card_id - 64))) != 0;
}

void HiddenOutcomeCatalog::remember_initial_position(
    const Game &game) noexcept {
  known_.clear();
  hidden_.clear();
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
      known_.add(game.board.visible[level][slot]);
    for (uint8_t card_id : game.board.decks[level])
      hidden_.add(card_id);
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      known_.add(game.board.players[player].reserved[slot]);
  }
}

bool HiddenOutcomeCatalog::is_initially_known(int card_id) const noexcept {
  return known_.contains(card_id);
}

bool HiddenOutcomeCatalog::is_initially_hidden(int card_id) const noexcept {
  return hidden_.contains(card_id);
}

bool HiddenOutcomeCatalog::is_claimed(const Board &board,
                                      int card_id) noexcept {
  if (card_id < 0 || card_id >= CARD_COUNT)
    return false;
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    const auto &cards = board.players[player].purchased_cards;
    if (std::find(cards.begin(), cards.end(), static_cast<uint8_t>(card_id)) !=
        cards.end())
      return true;
  }
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      if (board.visible[level][slot] == card_id)
        return true;
    }
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
      if (board.players[player].reserved[slot] == card_id)
        return true;
    }
  }
  return false;
}

CardIdSet
HiddenOutcomeCatalog::unseen_cards(const Board &board) const noexcept {
  CardIdSet result;
  for (int level = 0; level < 3; ++level) {
    for (uint8_t card_id : board.decks[level])
      result.add(card_id);
  }
  return result;
}

CardIdSet
HiddenOutcomeCatalog::acquired_hidden_cards(const Board &board) const noexcept {
  CardIdSet result;
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    for (uint8_t card_id : board.players[player].purchased_cards) {
      if (is_initially_hidden(card_id))
        result.add(card_id);
    }
  }
  return result;
}

bool OracleActionMetadata::less_than(
    const OracleActionMetadata &other) const noexcept {
  return std::tie(oracle_card, oracle_reserve, oracle_reserve_card,
                  oracle_return_color, oracle_gold_as) <
         std::tie(other.oracle_card, other.oracle_reserve,
                  other.oracle_reserve_card, other.oracle_return_color,
                  other.oracle_gold_as);
}

bool OracleActionMetadata::is_oracle() const noexcept {
  return oracle_card >= 0 || oracle_reserve;
}

RevealProofDagBuilder::RevealProofDagBuilder(size_t node_limit,
                                             size_t edge_limit) noexcept
    : node_limit_(node_limit), edge_limit_(edge_limit) {}

void RevealProofDagBuilder::reset() noexcept {
  edge_count_ = 0;
  nodes_.clear();
}

size_t RevealProofDagBuilder::node_count() const noexcept {
  return nodes_.size();
}

const std::vector<RevealVerifiedProofNode> &
RevealProofDagBuilder::nodes() const noexcept {
  return nodes_;
}

std::vector<RevealVerifiedProofNode> &RevealProofDagBuilder::nodes() noexcept {
  return nodes_;
}

size_t RevealProofDagBuilder::append_node(RevealVerifiedProofNode node) {
  if (node_limit_ && nodes_.size() >= node_limit_)
    throw ProofDagBuildAborted("proof DAG node limit exceeded");
  const size_t id = nodes_.size();
  nodes_.push_back(std::move(node));
  return id;
}

void RevealProofDagBuilder::append_edge(size_t parent,
                                        RevealVerifiedProofEdge edge) {
  if (edge_limit_ && edge_count_ >= edge_limit_)
    throw ProofDagBuildAborted("proof DAG edge limit exceeded");
  if (parent >= nodes_.size())
    throw ProofDagBuildAborted("proof DAG parent node is missing");
  nodes_[parent].children.push_back(std::move(edge));
  ++edge_count_;
}

std::vector<RevealVerifiedProofNode>
RevealProofDagBuilder::release_nodes() noexcept {
  edge_count_ = 0;
  std::vector<RevealVerifiedProofNode> released = std::move(nodes_);
  nodes_.clear();
  return released;
}

} // namespace csplendor::solver_internal
