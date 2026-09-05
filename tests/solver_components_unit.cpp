#include "reveal_solver_components.h"
#include "reveal_verified_solver.h"
#include "solver_action_filter.h"
#include "solver_card_equivalence.h"
#include "solver_path.h"
#include "solver_types.h"
#include "visible_only_solver.h"

#include <algorithm>
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
using csplendor::solver_internal::RevealSearchState;
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

void test_incremental_reveal_search_state_and_fallback() {
  Game game(20260905);
  HiddenOutcomeCatalog catalog;
  catalog.remember_initial_position(game);
  RevealSearchState state;
  const bool initialized = state.initialize(game, catalog);
  if (csplendor::solver_internal::config::
          incremental_reveal_search_state_enabled) {
    require(initialized && state.active() &&
                state.matches_reference(game.board, catalog),
            "canonical reveal sidecar did not select the fast path");
  } else {
    require(!initialized && !state.active(),
            "disabled reveal sidecar unexpectedly selected the fast path");
  }

  if (state.active()) {
    // Exercise ordinary transitions (including concrete visible refills) and
    // compare every incremental component with the scan oracle.
    for (int ply = 0; ply < 40 && !game.is_game_over(); ++ply) {
      const auto codes = game.legal_action_codes();
      require(!codes.empty(), "reveal sidecar trajectory ran out of actions");
      auto selected = codes.begin();
      for (auto it = codes.begin(); it != codes.end(); ++it) {
        const Action action = Action::unpack(*it);
        if (action.type == PURCHASE || action.type == VISIT_NOBLE) {
          selected = it;
          break;
        }
      }
      const auto before = state.observe_before(game.board);
      require(game.apply_action_code_trusted(*selected, false),
              "reveal sidecar trajectory transition failed");
      state.observe_after(before, game.board, catalog);
      require(state.active() && state.matches_reference(game.board, catalog),
              "reveal sidecar diverged after an ordinary transition");
    }

    // An exact deck-reserve outcome removes an arbitrary card while retaining
    // the legacy erase order of every other card.
    Game reserve_game(99);
    HiddenOutcomeCatalog reserve_catalog;
    reserve_catalog.remember_initial_position(reserve_game);
    RevealSearchState reserve_state;
    require(reserve_state.initialize(reserve_game, reserve_catalog),
            "deck-reserve sidecar initialization failed");
    const int level = 0;
    int selected_card = reserve_game.board.decks[level][0];
    int selected_cost = 1000;
    for (uint8_t candidate_id : reserve_game.board.decks[level]) {
      const Card &candidate = get_card(candidate_id);
      int cost = 0;
      for (uint8_t amount : candidate.cost)
        cost += amount;
      if (cost < selected_cost) {
        selected_card = candidate_id;
        selected_cost = cost;
      }
    }
    std::vector<uint8_t> expected_deck(reserve_game.board.decks[level].begin(),
                                       reserve_game.board.decks[level].end());
    expected_deck.erase(std::find(expected_deck.begin(), expected_deck.end(),
                                  static_cast<uint8_t>(selected_card)));
    require(reserve_state.move_deck_card_to_back(reserve_game.board, level,
                                                  selected_card),
            "exact reveal card could not be moved to the pop position");
    const auto before = reserve_state.observe_before(reserve_game.board);
    Action reserve;
    reserve.type = RESERVE_DECK;
    reserve.deck_level = level;
    require(reserve_game.apply_trusted(reserve, false),
            "exact deck-reserve transition failed");
    reserve_state.observe_after(before, reserve_game.board, reserve_catalog);
    require(std::equal(expected_deck.begin(), expected_deck.end(),
                       reserve_game.board.decks[level].begin()) &&
                expected_deck.size() ==
                    reserve_game.board.decks[level].size(),
            "exact reveal changed the remaining physical deck order");
    require(reserve_state.matches_reference(reserve_game.board,
                                            reserve_catalog),
            "exact deck-reserve sidecar diverged from its oracle");

    // Keep playing until the originally hidden reserved card is purchased.
    // This covers the acquired-hidden delta as well as its ordinary rollback
    // representation; selecting the cheapest level-1 outcome keeps the
    // trajectory short and independent of a specific shuffled deck order.
    const int reserving_player = 0;
    bool purchased_hidden = false;
    for (int ply = 0; ply < 40 && !reserve_game.is_game_over(); ++ply) {
      const auto codes = reserve_game.legal_action_codes();
      require(!codes.empty(), "hidden-purchase trajectory ran out of actions");
      auto selected = codes.begin();
      int selected_score = -100000;
      for (auto it = codes.begin(); it != codes.end(); ++it) {
        const Action candidate = Action::unpack(*it);
        int score = 0;
        if (reserve_game.current_player() == reserving_player &&
            candidate.type == PURCHASE &&
            candidate.card_id == selected_card) {
          score = 100000;
        } else if (candidate.type == TAKE_DIFFERENT ||
                   candidate.type == TAKE_SAME) {
          const Card &target = get_card(selected_card);
          for (int color = 0; color < 5; ++color) {
            const int deficit = std::max(
                0, static_cast<int>(target.cost[color]) -
                       static_cast<int>(reserve_game.board
                                            .players[reserving_player]
                                            .bonuses[color]) -
                       static_cast<int>(reserve_game.board
                                            .players[reserving_player]
                                            .gems[color]));
            const int useful = std::min<int>(candidate.take[color], deficit);
            score += reserve_game.current_player() == reserving_player
                         ? useful * 20
                         : -useful * 20;
            score -= candidate.return_gems[color] * 30;
          }
          score += 10;
        } else if (candidate.type == PASS) {
          score = -1000;
        } else {
          score = -100;
        }
        if (score > selected_score) {
          selected_score = score;
          selected = it;
        }
      }

      const Action chosen = Action::unpack(*selected);
      const auto transition = reserve_state.observe_before(reserve_game.board);
      require(reserve_game.apply_action_code_trusted(*selected, false),
              "hidden-purchase trajectory transition failed");
      reserve_state.observe_after(transition, reserve_game.board,
                                  reserve_catalog);
      require(reserve_state.active() &&
                  reserve_state.matches_reference(reserve_game.board,
                                                  reserve_catalog),
              "hidden-purchase sidecar diverged from its oracle");
      if (chosen.type == PURCHASE && chosen.card_id == selected_card) {
        purchased_hidden = true;
        break;
      }
    }
    require(purchased_hidden &&
                reserve_state.acquired_hidden().contains(selected_card),
            "purchased hidden card was not added to acquired-hidden state");
  }

  Game duplicate(7);
  const int duplicated = duplicate.board.visible[0][0];
  duplicate.board.begin_editor_mutation().decks[0][0] =
      static_cast<uint8_t>(duplicated);
  HiddenOutcomeCatalog duplicate_catalog;
  duplicate_catalog.remember_initial_position(duplicate);
  RevealSearchState fallback;
  require(!fallback.initialize(duplicate, duplicate_catalog) &&
              !fallback.active(),
          "noncanonical duplicate-card root did not use the fallback path");
  require(fallback.is_claimed(duplicate.board, duplicated),
          "fallback claimed-card scan changed semantics");
}

void test_reveal_solver_sidecar_restores_on_node_limit() {
  // A canonical root enables the sidecar.  Depth one creates blank visible
  // slots, so the defender's non-exact search also traverses oracle actions.
  // The deliberately small limit exits from a nested branch; verify builds
  // compare the restored root sidecar after the exception is caught.
  RevealVerifiedSolver solver(0, 1, 20, 0.0);
  const auto result = solver.solve(Game(0));
  if (result.unknown_reason != "node limit exceeded") {
    throw std::runtime_error(
        "reveal solver did not stop at the nested node limit: reason=" +
        result.reason + " unknown=" + result.unknown_reason +
        " nodes=" + std::to_string(result.stats.nodes) +
        " oracle=" +
        std::to_string(result.stats.oracle_purchase_actions +
                       result.stats.oracle_reserve_actions));
  }
  require(result.stats.oracle_purchase_actions +
                  result.stats.oracle_reserve_actions >
              0,
          "reveal solver node-limit path did not exercise oracle branches");
}

void test_incremental_reveal_search_state_random_differential() {
  if (!csplendor::solver_internal::config::
          incremental_reveal_search_state_enabled)
    return;
  for (uint64_t seed = 0; seed < 1000; ++seed) {
    Game game(seed);
    HiddenOutcomeCatalog catalog;
    catalog.remember_initial_position(game);
    RevealSearchState state;
    require(state.initialize(game, catalog),
            "random canonical root did not select reveal fast path");
    for (uint64_t ply = 0; ply < 4 && !game.is_game_over(); ++ply) {
      const auto codes = game.legal_action_codes();
      require(!codes.empty(), "random reveal differential found no action");
      const uint64_t code = codes[static_cast<size_t>(
          (seed * 0x9e3779b97f4a7c15ULL + ply * 17) % codes.size())];
      const auto before = state.observe_before(game.board);
      require(game.apply_action_code_trusted(code, false),
              "random reveal differential transition failed");
      state.observe_after(before, game.board, catalog);
      require(state.active() && state.matches_reference(game.board, catalog),
              "random reveal sidecar differed from scan oracle");
    }
  }
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
    test_incremental_reveal_search_state_and_fallback();
    test_incremental_reveal_search_state_random_differential();
    test_reveal_solver_sidecar_restores_on_node_limit();
    test_proof_dag_builder_owns_limits();
  } catch (const std::exception &exc) {
    std::cerr << exc.what() << '\n';
    return 1;
  }
  return 0;
}
