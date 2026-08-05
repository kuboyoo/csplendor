#include "reveal_solver_components.h"
#include "reveal_verified_solver.h"
#include "solver_types.h"
#include "visible_only_solver.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using csplendor::solver_internal::ActionOrderKey;
using csplendor::solver_internal::HiddenOutcomeCatalog;
using csplendor::solver_internal::OracleActionMetadata;
using csplendor::solver_internal::ProofDagBuildAborted;
using csplendor::solver_internal::RevealProofDagBuilder;
using csplendor::solver_internal::SearchLimit;
using csplendor::solver_internal::SearchLimitExceeded;
using csplendor::solver_internal::StateKeyCore;
using csplendor::solver_internal::ZeroSumScore;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void test_public_solver_value_contracts() {
  static_assert(std::is_copy_constructible_v<VisibleOnlySolver>);
  static_assert(std::is_copy_assignable_v<VisibleOnlySolver>);
  static_assert(std::is_nothrow_move_constructible_v<VisibleOnlySolver>);
  static_assert(std::is_copy_constructible_v<RevealVerifiedSolver>);
  static_assert(std::is_copy_assignable_v<RevealVerifiedSolver>);
  static_assert(std::is_nothrow_move_constructible_v<RevealVerifiedSolver>);

  VisibleOnlySolver original(1, 1.0);
  VisibleOnlySolver copied = original;
  const auto first = original.solve(Game(1));
  const auto second = copied.solve(Game(1));
  require(first.winner == second.winner &&
              first.unknown_reason == "node limit exceeded" &&
              first.unknown_reason == second.unknown_reason,
          "visible solver copy changed its limit/result contract");
}

void test_common_search_values() {
  SearchLimit limit(3, 0.0);
  limit.reset();
  limit.check(2);
  bool threw = false;
  try {
    limit.check(3);
  } catch (const SearchLimitExceeded &exc) {
    threw = std::string(exc.what()) == "node limit exceeded";
  }
  require(threw, "node limit boundary or error text changed");

  const StateKeyCore left{123, 1, 2, 3, 4, true, 0};
  const StateKeyCore same{123, 1, 2, 3, 4, true, 0};
  const StateKeyCore other{123, 1, 2, 3, 5, true, 0};
  require(left == same && !(left == other),
          "shared state-key equality is inconsistent");
  require(ZeroSumScore::from_winner(0) == 1 &&
              ZeroSumScore::from_winner(1) == -1 &&
              ZeroSumScore::winner(0) == -2,
          "shared solver score/tie mapping changed");

  const ActionOrderKey purchase{0, -3, 9};
  const ActionOrderKey take{2, 0, 1};
  require(purchase < take, "shared action ordering changed");
}

void test_hidden_and_oracle_components() {
  Game game(0);
  HiddenOutcomeCatalog catalog;
  catalog.remember_initial_position(game);
  const int visible = game.board.visible[0][0];
  const int hidden = game.board.decks[0][0];
  require(catalog.is_initially_known(visible) &&
              catalog.is_initially_hidden(hidden),
          "initial visible/hidden card classification changed");
  require(HiddenOutcomeCatalog::is_claimed(game.board, visible) &&
              !HiddenOutcomeCatalog::is_claimed(game.board, hidden),
          "hidden-card claim detection changed");
  const auto unseen = catalog.unseen_cards(game.board);
  require(unseen.contains(hidden), "unseen-card state key omitted a deck card");

  OracleActionMetadata ordinary;
  OracleActionMetadata purchase;
  purchase.oracle_card = hidden;
  require(!ordinary.is_oracle() && purchase.is_oracle() &&
              ordinary.less_than(purchase),
          "oracle metadata classification/order changed");
}

void test_proof_dag_builder_owns_limits() {
  RevealProofDagBuilder builder(1, 1);
  RevealVerifiedProofNode root;
  root.id = 0;
  require(builder.append_node(root) == 0 && builder.node_count() == 1,
          "proof builder did not own its root node");
  builder.append_edge(0, RevealVerifiedProofEdge{});

  bool node_limit = false;
  try {
    builder.append_node(RevealVerifiedProofNode{});
  } catch (const ProofDagBuildAborted &exc) {
    node_limit = std::string(exc.what()) == "proof DAG node limit exceeded";
  }
  require(node_limit, "proof DAG node boundary changed");

  bool edge_limit = false;
  try {
    builder.append_edge(0, RevealVerifiedProofEdge{});
  } catch (const ProofDagBuildAborted &exc) {
    edge_limit = std::string(exc.what()) == "proof DAG edge limit exceeded";
  }
  require(edge_limit, "proof DAG edge boundary changed");

  auto nodes = builder.release_nodes();
  require(nodes.size() == 1 && nodes[0].children.size() == 1 &&
              builder.node_count() == 0,
          "proof DAG release retained or lost owned state");
}

} // namespace

int main() {
  try {
    test_public_solver_value_contracts();
    test_common_search_values();
    test_hidden_and_oracle_components();
    test_proof_dag_builder_owns_limits();
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
  return 0;
}
