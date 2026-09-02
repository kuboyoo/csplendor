#include "reveal_solver_components.h"
#include "reveal_verified_solver.h"
#include "solver_action_filter.h"
#include "solver_card_equivalence.h"
#include "solver_path.h"
#include "solver_types.h"
#include "visible_only_solver.h"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using csplendor::solver_internal::ActionOrderKey;
using csplendor::solver_internal::CardEquivalenceMask;
using csplendor::solver_internal::HiddenOutcomeCatalog;
using csplendor::solver_internal::OracleActionMetadata;
using csplendor::solver_internal::ProofDagBuildAborted;
using csplendor::solver_internal::RevealProofDagBuilder;
using csplendor::solver_internal::RecursionPath;
using csplendor::solver_internal::SearchLimit;
using csplendor::solver_internal::SearchLimitExceeded;
using csplendor::solver_internal::ScopedPathEntry;
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

void test_recursive_path_lifo_guard() {
  using Path = RecursionPath<int, std::hash<int>>;
  Path path(4);
  const int first_key = 7;
  const int second_key = 9;
  require(path.empty() && path.size() == 0 && !path.contains(7),
          "recursive path did not start empty");
  {
    ScopedPathEntry<Path> first(path, first_key);
    require(!path.empty() && path.size() == 1 && path.contains(7) &&
                !path.contains(9),
            "recursive path did not retain its first frame");
    {
      ScopedPathEntry<Path> second(path, second_key);
      require(path.size() == 2 && path.contains(7) && path.contains(9),
              "recursive path lost a nested frame");
    }
    require(path.size() == 1 && path.contains(7) && !path.contains(9),
            "recursive path guard did not pop the nested frame");
  }
  require(path.empty() && !path.contains(7),
          "recursive path guard did not pop the root frame");

  Path unbounded_path(4, false);
  {
    ScopedPathEntry<Path> entry(unbounded_path, first_key);
    require(unbounded_path.contains(first_key),
            "unbounded recursive path did not retain its hash entry");
  }
  require(unbounded_path.empty(),
          "unbounded recursive path guard did not erase its hash entry");
}

void test_card_equivalence_classes_match_tuple_oracle() {
  const auto &classes =
      csplendor::solver_internal::CARD_EQUIVALENCE_CLASSES;
  require(classes.class_count > 0 && classes.class_count <= 128,
          "card equivalence class count does not fit its mask");
  for (int left = 0; left < CARD_COUNT; ++left) {
    for (int right = 0; right < CARD_COUNT; ++right) {
      const bool same_class = classes.class_ids[left] == classes.class_ids[right];
      const bool same_tuple =
          csplendor::solver_internal::same_card_equivalence_tuple(
              get_card(left), get_card(right));
      require(same_class == same_tuple,
              "card equivalence class and tuple oracle disagree");
    }
  }

  CardEquivalenceMask mask;
  const uint8_t first = classes.class_ids[0];
  require(mask.insert(first) && !mask.insert(first),
          "card equivalence mask did not reject a duplicate class");
}

ActionOrderKey ordered_test_action(Action action, int rank, int neg_points) {
  return ActionOrderKey{rank, neg_points, action.pack()};
}

void test_compact_forced_action_filter_order_and_caps() {
  std::vector<ActionOrderKey> actions;
  Action purchase;
  purchase.type = PURCHASE;
  purchase.card_id = 4;
  actions.push_back(ordered_test_action(purchase, 0, -1));
  for (int card = 9; card >= 0; --card) {
    Action reserve;
    reserve.type = RESERVE_VISIBLE;
    reserve.card_id = static_cast<int8_t>(card);
    actions.push_back(ordered_test_action(reserve, 1, 0));
  }
  for (int score = 0; score < 10; ++score) {
    Action take;
    take.type = TAKE_DIFFERENT;
    take.take[score % 5] = 1;
    take.return_gems[(score + 1) % 5] = static_cast<uint8_t>(score / 5);
    actions.push_back(ordered_test_action(take, 2, 0));
  }
  Action pass;
  pass.type = PASS;
  actions.push_back(ordered_test_action(pass, 9, 0));

  const auto filtered =
      csplendor::solver_internal::compact_forced_attacker_actions<6, 3>(
          actions, [](const Action &action) {
            return static_cast<int>(action.return_gems[0]) * 10 +
                   static_cast<int>(action.take[0]);
          });
  require(filtered.size() == 11,
          "compact forced-action filter did not enforce category caps");
  require(Action::unpack(filtered.front().code).type == PURCHASE,
          "compact forced-action filter moved purchases out of first place");
  for (size_t index = 1; index < 7; ++index)
    require(Action::unpack(filtered[index].code).type == TAKE_DIFFERENT,
            "compact forced-action filter did not group selected takes");
  for (size_t index = 7; index < 10; ++index)
    require(Action::unpack(filtered[index].code).type == RESERVE_VISIBLE,
            "compact forced-action filter did not group selected reserves");
  require(Action::unpack(filtered.back().code).type == PASS,
          "compact forced-action filter did not preserve passthrough tail");
  require(filtered[7].code < filtered[8].code &&
              filtered[8].code < filtered[9].code,
          "compact forced-action filter did not order reserves");
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
    test_recursive_path_lifo_guard();
    test_card_equivalence_classes_match_tuple_oracle();
    test_compact_forced_action_filter_order_and_caps();
    test_hidden_and_oracle_components();
    test_proof_dag_builder_owns_limits();
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
  return 0;
}
