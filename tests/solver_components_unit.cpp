#include "reveal_solver_components.h"
#include "reveal_verified_solver.h"
#include "solver_action_filter.h"
#include "solver_card_equivalence.h"
#include "solver_path.h"
#include "solver_reveal_order.h"
#include "solver_search_scratch.h"
#include "solver_tt_types.h"
#include "solver_types.h"
#include "visible_only_solver.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>

namespace {

using csplendor::solver_internal::ActionOrderKey;
using csplendor::solver_internal::CardEquivalenceMask;
using csplendor::solver_internal::ForceStatus;
using csplendor::solver_internal::HiddenOutcomeCatalog;
using csplendor::solver_internal::OracleActionMetadata;
using csplendor::solver_internal::ProofDagBuildAborted;
using csplendor::solver_internal::RecursionPath;
using csplendor::solver_internal::RevealProofDagBuilder;
using csplendor::solver_internal::RevealSearchState;
using csplendor::solver_internal::ScopedPathEntry;
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

  RevealVerifiedSolver reveal(0, 3, 500, 0.0, {}, false, 100000, 500000,
                              UINT64_MAX, false, 0, true, true);
  const auto interrupted = reveal.solve(Game(42));
  require(interrupted.unknown_reason == "node limit exceeded",
          "scratch test did not interrupt a recursive search");
  RevealVerifiedSolver reveal_copy = reveal;
  RevealVerifiedSolver assigned(1, 1, 1, 0.0);
  assigned = reveal;
  for (int seed : {43, 42, 44}) {
    const auto reused = reveal.solve(Game(seed));
    const auto copied_result = reveal_copy.solve(Game(seed));
    const auto assigned_result = assigned.solve(Game(seed));
    for (const auto *result : {&copied_result, &assigned_result}) {
      require(result->proven == reused.proven &&
                  result->unknown_reason == reused.unknown_reason &&
                  result->stats.nodes == reused.stats.nodes &&
                  result->stats.legal_moves == reused.stats.legal_moves &&
                  result->stats.memo_hits == reused.stats.memo_hits,
              "reveal copy/reuse after interruption changed search");
    }
  }
}

void test_purchase_visit_diagnostics() {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  using csplendor::perf::Counter;
  // An explicit editor fixture keeps this test independent of self-play. The
  // diagnostic belongs to the common enumeration path, including fallback.
  for (int count : {1, 3, 20}) {
    Game game(42);
    game.board.decks[0].count = count;
    auto &player = game.board.players[0];
    player.bonuses = {10, 10, 10, 10, 10};
    player.sync_packed();
    game.board.invalidate_hash();
    Action purchase;
    purchase.type = PURCHASE;
    purchase.card_id = game.board.visible[0][0];
    require(game.is_legal(purchase), "diagnostic fixture purchase is illegal");
    for (size_t edge_limit : {size_t(1), size_t(1000)}) {
      RevealVerifiedSolver solver(0, 3, 0, 0.0, {}, false, 100000, 500000,
                                  purchase.pack(), false, 0, false, true);
      csplendor::perf::reset();
      const auto frontier = solver.split_root(game, edge_limit);
      const auto perf = csplendor::perf::snapshot();
      const auto generated = perf.get(Counter::SolverVisiblePurchaseGenerated);
      const auto visited = perf.get(Counter::SolverVisiblePurchaseVisited);
      require(generated == static_cast<uint64_t>(count), "generated count changed");
      require(visited == std::min(generated, edge_limit + 1), "cutoff visits were miscounted");
      require(perf.get(Counter::SolverVisiblePurchaseRefills) == 1 &&
                  perf.get(Counter::SolverVisiblePurchaseApplyCalls) == visited,
              "purchase scope/apply count changed");
      require(perf.get(Counter::SolverVisiblePurchaseVisited0) == 0 &&
                  perf.get(Counter::SolverVisiblePurchaseVisited1) == (visited == 1) &&
                  perf.get(Counter::SolverVisiblePurchaseVisited2To4) == (visited >= 2 && visited <= 4) &&
                  perf.get(Counter::SolverVisiblePurchaseVisited5Plus) == (visited >= 5),
              "exception/full-enumeration visit histogram changed");
      require(frontier.complete == (edge_limit >= generated), "frontier limit contract changed");
    }
  }
#endif
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

void test_reveal_score_order_and_locality() {
  using csplendor::solver_internal::sort_reveal_cards_by_score;
  // Cover the 40-card production bound, larger fallback, all ties, negative
  // values and integer extremes without subtraction-based comparisons.
  for (int size : {0, 1, 2, 16, 40, 41, 90}) {
    for (int action = 0; action < 6; ++action) {
      std::vector<int> cards;
      for (int i = 0; i < size; ++i)
        cards.push_back(size - i - 1);
      const auto score = [action](int card) {
        if (action == 0)
          return 0;
        if (action == 1)
          return card % 2 ? std::numeric_limits<int>::min()
                          : std::numeric_limits<int>::max();
        return (card * 7 + action * card) % 11 - 5;
      };
      auto expected = cards;
      sort_reveal_cards_by_score<false>(expected, score);
      size_t calls = 0;
      sort_reveal_cards_by_score<true>(cards, [&](int card) {
        ++calls;
        return score(card);
      });
      require(cards == expected, "cached reveal order differs from comparator");
#ifndef CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER
      require(calls == (size > 1 ? static_cast<size_t>(size) : 0),
              "reveal score must be computed once per candidate, not per compare");
#endif
    }
  }
  std::vector<int> cards{3, 2, 1, 0};
  const auto original = cards;
  bool threw = false;
  try {
    sort_reveal_cards_by_score<true>(cards, [](int) -> int {
      throw std::runtime_error("score failure");
    });
  } catch (const std::runtime_error &) {
    threw = true;
  }
  require(threw && cards == original, "score exception was swallowed or mutated input");
}

void test_defender_reserve_order_depends_on_return_colour() {
  for (bool simple : {false, true}) {
    Game game(42);
    game.simple_payment_mode = simple;
    Board &board = game.board.begin_editor_mutation();
    board.players[0].gems = {2, 2, 2, 2, 2, 0};
    board.players[0].sync_packed();
    board.bank = {2, 2, 2, 2, 2, 5};
    const auto input_hash = board.hash();
    RevealVerifiedSolver solver(1, 3, 0, 0.0, {}, false, 100000, 500000,
                                UINT64_MAX, false, 0, true, true);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    csplendor::perf::reset();
#endif
    const auto result = solver.split_root(game, 50000);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    const auto counts = csplendor::perf::snapshot();
    const auto score_calls = counts.get(
        csplendor::perf::Counter::SolverDefenderReserveScoreCalls);
    const auto candidates = counts.get(
        csplendor::perf::Counter::SolverDefenderReserveSortCandidates);
    require(candidates > 0, "defender reserve score path was not exercised");
#ifndef CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER
    require(csplendor::solver_internal::cache_reveal_scores_enabled
                ? score_calls == candidates
                : score_calls > candidates,
            "defender reserve score-call mechanism changed");
#endif
    std::cout << "defender reserve simple=" << simple
              << " score_calls=" << score_calls
              << " candidates=" << candidates << '\n';
#endif
    require(result.complete, "defender reserve frontier was not complete");
    std::unordered_map<uint64_t, std::pair<int, int>> last;
    std::set<int> return_colours;
    for (const auto &edge : result.edges) {
      const Action action = Action::unpack(edge.action_code);
      if (action.type != RESERVE_DECK)
        continue;
      const Card &card = get_card(edge.reveal_card);
      int shortage = 0;
      for (int colour = 0; colour < 5; ++colour) {
        const int available = 2 - action.return_gems[colour];
        shortage += std::max(0, static_cast<int>(card.cost[colour]) - available);
      }
      const int gold = 1 - action.return_gems[GOLD];
      const int gap = std::max(0, shortage - gold);
      // This fixture has zero bonuses/points; no card can win or earn a noble.
      const int score = (gap == 0 ? 100000 : 0) + card.points * 10000 -
                        gap * 100 + card.bonus;
      const std::pair<int, int> order{-score, edge.reveal_card};
      const auto previous = last.find(edge.action_code);
      require(previous == last.end() || previous->second <= order,
              "defender reserve reused another action's score/order");
      last[edge.action_code] = order;
      for (int colour = 0; colour < 6; ++colour)
        if (action.return_gems[colour])
          return_colours.insert(colour);
    }
    require(last.size() >= 18 && return_colours.size() == 6,
            "defender reserve oracle did not cover every return colour/level");
    require(game.board.hash() == input_hash && game.simple_payment_mode == simple,
            "sorting a frontier mutated its input game");
  }
}

void test_solver_tt_layout_and_value_contracts() {
  using namespace csplendor::solver_internal;
  const StateKeyCore core{123, 1, 2, 3, 4, true, 0};
  const RevealStateKey key(core, 5, 6, 7, 8, 2, 3);
  const RevealStateKey same(core, 5, 6, 7, 8, 2, 3);
  require(key == same &&
              RevealStateKeyHash{}(key) == RevealStateKeyHash{}(same),
          "reveal TT key equality/hash contract changed");
  require(key != RevealStateKey(core, 9, 6, 7, 8, 2, 3) &&
              key != RevealStateKey(core, 5, 9, 7, 8, 2, 3) &&
              key != RevealStateKey(core, 5, 6, 9, 8, 2, 3) &&
              key != RevealStateKey(core, 5, 6, 7, 9, 2, 3) &&
              key != RevealStateKey(core, 5, 6, 7, 8, 4, 3) &&
              key != RevealStateKey(core, 5, 6, 7, 8, 2, 4),
          "reveal TT key omitted a semantic field");
  require(key != RevealStateKey(StateKeyCore{124, 1, 2, 3, 4, true, 0}, 5, 6, 7,
                                8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 2, 2, 3, 4, true, 0}, 5,
                                    6, 7, 8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 1, 3, 3, 4, true, 0}, 5,
                                    6, 7, 8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 1, 2, 4, 4, true, 0}, 5,
                                    6, 7, 8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 1, 2, 3, 5, true, 0}, 5,
                                    6, 7, 8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 1, 2, 3, 4, false, 0}, 5,
                                    6, 7, 8, 2, 3) &&
              key != RevealStateKey(StateKeyCore{123, 1, 2, 3, 4, true, 1}, 5,
                                    6, 7, 8, 2, 3),
          "reveal TT packed metadata omitted a core field");

  const RevealStateKey default_key;
  const RevealStateKey constructed_default(StateKeyCore{}, 0, 0, 0, 0, 0, 0);
  require(default_key == constructed_default,
          "reveal TT default key differs from its logical default state");

  const RevealStateKey exact_source(core, 5, 6, 0, 0, 2, 3);
  const RevealExactDepthStateKey exact_key(exact_source, 7);
  const RevealExactDepthStateKey exact_same(exact_source, 7);
  require(exact_key == exact_same &&
              RevealExactDepthStateKeyHash{}(exact_key) ==
                  RevealExactDepthStateKeyHash{}(exact_same),
          "exact reveal TT key equality/hash contract changed");
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  require(RevealExactStateKey{} == RevealExactStateKey(constructed_default),
          "exact reveal TT default key differs from its logical default state");
  bool rejected_non_root_independent = false;
  try {
    (void)RevealExactStateKey(key);
  } catch (const std::invalid_argument &) {
    rejected_non_root_independent = true;
  }
  require(rejected_non_root_independent,
          "exact reveal TT key accepted acquired-hidden state");
#endif

  RevealMemoEntry transient(ForceStatus::PROVEN, 42, 17, true, 70000, false);
  require(transient.status() == ForceStatus::PROVEN &&
              transient.action_code() == 42 && transient.reveal_card() == 17 &&
              transient.has_action() && transient.action_count() == 70000 &&
              !transient.replayable(),
          "transient reveal TT entry lost a field");
  transient.set_status(ForceStatus::REFUTED);
  transient.set_action_count(70001);
  require(transient.status() == ForceStatus::REFUTED &&
              transient.action_count() == 70001,
          "transient reveal TT entry update changed semantics");

  RevealPersistentEntry persistent(ForceStatus::PROVEN, 84, CARD_COUNT - 1,
                                   true, MAX_MOVES, true);
  const RevealPersistentEntry non_replayable(ForceStatus::REFUTED, 85, -1, true,
                                             MAX_MOVES, false);
  persistent.set_generation(UINT64_MAX - 1);
  persistent.set_last_touched(UINT64_MAX);
  require(persistent.status() == ForceStatus::PROVEN &&
              persistent.action_code() == 84 &&
              persistent.reveal_card() == CARD_COUNT - 1 &&
              persistent.has_action() &&
              persistent.action_count() == MAX_MOVES &&
              persistent.replayable() && !non_replayable.replayable() &&
              persistent.generation() == UINT64_MAX - 1 &&
              persistent.last_touched() == UINT64_MAX,
          "persistent reveal TT entry lost a field or counter width");

  VisibleMemoEntry visible(1, VisibleMemoEntry::Bound::LOWER,
#ifdef CSPLENDOR_COMPACT_SOLVER_REASONS
                           VisibleEntryReason::CurrentPlayerWin,
#else
                           std::string(
                               "current_player_can_force_visible_only_win"),
#endif
                           126, true, MAX_MOVES);
  VisibleForceEntry forced(ForceStatus::REFUTED, 168, true, MAX_MOVES);
  require(visible.score() == 1 &&
              visible.bound() == VisibleMemoEntry::Bound::LOWER &&
              visible.action_code() == 126 && visible.has_action() &&
              visible.action_count() == MAX_MOVES &&
              forced.status() == ForceStatus::REFUTED &&
              forced.action_code() == 168 && forced.has_action() &&
              forced.action_count() == MAX_MOVES,
          "visible TT compact entry lost a field");

#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  bool rejected_visible_count = false;
  try {
    (void)VisibleForceEntry(
        ForceStatus::PROVEN, 0, false,
        static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1);
  } catch (const std::overflow_error &) {
    rejected_visible_count = true;
  }
  require(rejected_visible_count,
          "compact visible TT accepted an overflowing action count");
  if constexpr (sizeof(size_t) > sizeof(uint32_t)) {
    bool rejected_persistent_count = false;
    try {
      (void)RevealPersistentEntry(
          ForceStatus::PROVEN, 0, -1, false,
          static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1, true);
    } catch (const std::overflow_error &) {
      rejected_persistent_count = true;
    }
    require(rejected_persistent_count,
            "compact persistent TT accepted an overflowing action count");
  }
#endif

  const SolverTtLayoutMetrics layout = solver_tt_layout_metrics();
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  require(layout.reveal_exact_state_key < layout.reveal_state_key &&
              layout.reveal_exact_depth_key < layout.reveal_depth_key &&
              layout.reveal_memo_entry < layout.reveal_persistent_entry &&
              layout.visible_force_entry < layout.reveal_persistent_entry &&
              layout.visible_force_bounds < 2 * layout.reveal_persistent_entry,
          "compact solver TT did not retain its portable size relationships");
#if defined(__GLIBCXX__)
  if constexpr (sizeof(void *) == 8) {
    require(layout.reveal_state_key == 48 && layout.reveal_depth_key == 56 &&
                layout.reveal_exact_state_key == 32 &&
                layout.reveal_exact_depth_key == 40 &&
                layout.reveal_memo_entry == 24 &&
                layout.reveal_persistent_entry == 32 &&
                layout.reveal_memo_value == 80 &&
                layout.reveal_persistent_value == 72 &&
                layout.reveal_proof_node_value == 64 &&
                layout.visible_force_entry == 16 &&
                layout.visible_force_bounds == 40 &&
                layout.visible_force_value == 40 &&
                layout.visible_bounds_value == 56,
            "compact reveal TT layout exceeded its 64-bit libstdc++ budget");
#ifdef CSPLENDOR_COMPACT_SOLVER_REASONS
    require(layout.visible_memo_entry == 16 && layout.visible_memo_value == 32,
            "compact visible minimax TT layout exceeded its 64-bit libstdc++ "
            "budget");
#else
    require(layout.visible_memo_entry == 56 && layout.visible_memo_value == 72,
            "string-reason visible minimax TT 64-bit libstdc++ baseline "
            "changed");
#endif
  }
#endif
#else
  require(layout.reveal_exact_state_key == layout.reveal_state_key &&
              layout.reveal_exact_depth_key == layout.reveal_depth_key &&
              layout.reveal_memo_entry == layout.reveal_persistent_entry,
          "legacy solver TT aliases changed their portable layout contract");
#if defined(__GLIBCXX__)
  if constexpr (sizeof(void *) == 8) {
    require(layout.reveal_state_key == 56 && layout.reveal_depth_key == 64 &&
                layout.reveal_exact_state_key == 56 &&
                layout.reveal_exact_depth_key == 64 &&
                layout.reveal_memo_entry == 56 &&
                layout.reveal_persistent_entry == 56 &&
                layout.reveal_memo_value == 120 &&
                layout.reveal_persistent_value == 120 &&
                layout.reveal_proof_node_value == 72 &&
                layout.visible_force_entry == 32 &&
                layout.visible_force_bounds == 72 &&
                layout.visible_force_value == 56 &&
                layout.visible_bounds_value == 88,
            "legacy reveal TT 64-bit libstdc++ baseline changed");
#ifdef CSPLENDOR_COMPACT_SOLVER_REASONS
    require(layout.visible_memo_entry == 32 && layout.visible_memo_value == 48,
            "legacy visible minimax TT 64-bit libstdc++ baseline changed");
#else
    require(layout.visible_memo_entry == 64 && layout.visible_memo_value == 80,
            "string-reason visible minimax TT 64-bit libstdc++ baseline "
            "changed");
#endif
  }
#endif
#endif
}

void test_solver_tt_full_key_collision_contract() {
  using namespace csplendor::solver_internal;
  struct ConstantHash {
    size_t operator()(const RevealDepthStateKey &) const noexcept { return 0; }
  };

  std::unordered_map<RevealDepthStateKey, int, ConstantHash> table;
  table.reserve(128);
  for (int index = 0; index < 128; ++index) {
    const StateKeyCore core{static_cast<uint64_t>(1000 + index),
                            static_cast<uint8_t>(index % 16),
                            static_cast<uint8_t>((index + 1) % 16),
                            static_cast<uint8_t>(index % 32),
                            static_cast<uint8_t>((index + 3) % 32),
                            (index & 1) != 0,
                            static_cast<int8_t>((index % 3) - 1)};
    const RevealStateKey state(
        core, static_cast<uint64_t>(index), static_cast<uint64_t>(index) << 1,
        static_cast<uint64_t>(index) << 2, static_cast<uint64_t>(index) << 3,
        static_cast<uint8_t>(index % 4), static_cast<uint8_t>((index + 1) % 4));
    table.emplace(RevealDepthStateKey{state, index % 9}, index);
  }
  require(table.size() == 128,
          "solver TT collapsed distinct keys under hash collision");

  for (int index = 0; index < 128; ++index) {
    const StateKeyCore core{static_cast<uint64_t>(1000 + index),
                            static_cast<uint8_t>(index % 16),
                            static_cast<uint8_t>((index + 1) % 16),
                            static_cast<uint8_t>(index % 32),
                            static_cast<uint8_t>((index + 3) % 32),
                            (index & 1) != 0,
                            static_cast<int8_t>((index % 3) - 1)};
    const RevealStateKey state(
        core, static_cast<uint64_t>(index), static_cast<uint64_t>(index) << 1,
        static_cast<uint64_t>(index) << 2, static_cast<uint64_t>(index) << 3,
        static_cast<uint8_t>(index % 4), static_cast<uint8_t>((index + 1) % 4));
    const RevealDepthStateKey key{state, index % 9};
    const auto found = table.find(key);
    require(found != table.end() && found->second == index,
            "solver TT failed a full-key lookup under hash collision");
  }

  const RevealStateKey duplicate_state(StateKeyCore{1007, 7, 8, 7, 10, true, 0},
                                       7, 14, 28, 56, 3, 0);
  const RevealDepthStateKey duplicate{duplicate_state, 7};
  table[duplicate] = 999;
  require(table.size() == 128 && table.at(duplicate) == 999,
          "solver TT collision update duplicated an existing key");
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
  const auto &classes = csplendor::solver_internal::CARD_EQUIVALENCE_CLASSES;
  require(classes.class_count > 0 && classes.class_count <= 128,
          "card equivalence class count does not fit its mask");
  for (int left = 0; left < CARD_COUNT; ++left) {
    for (int right = 0; right < CARD_COUNT; ++right) {
      const bool same_class =
          classes.class_ids[left] == classes.class_ids[right];
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
                expected_deck.size() == reserve_game.board.decks[level].size(),
            "exact reveal changed the remaining physical deck order");
    require(
        reserve_state.matches_reference(reserve_game.board, reserve_catalog),
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
            candidate.type == PURCHASE && candidate.card_id == selected_card) {
          score = 100000;
        } else if (candidate.type == TAKE_DIFFERENT ||
                   candidate.type == TAKE_SAME) {
          const Card &target = get_card(selected_card);
          for (int color = 0; color < 5; ++color) {
            const int deficit =
                std::max(0, static_cast<int>(target.cost[color]) -
                                static_cast<int>(
                                    reserve_game.board.players[reserving_player]
                                        .bonuses[color]) -
                                static_cast<int>(
                                    reserve_game.board.players[reserving_player]
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
        " nodes=" + std::to_string(result.stats.nodes) + " oracle=" +
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

void test_recursive_scratch_lifetime() {
  struct Frame {
    std::vector<int> values;
    void clear() { values.clear(); }
  };
  using Scratch = csplendor::solver_internal::RecursionScratch<Frame>;
  Scratch scratch;
  const auto recurse = [&](auto &self, int depth) -> void {
    Scratch::Lease lease(scratch);
    auto &frame = lease.get();
    frame.values.assign(1 + depth % 91, depth);
    const auto *address = frame.values.data();
    if (depth)
      self(self, depth - 1);
    require(frame.values.data() == address && frame.values.back() == depth,
            "scratch growth invalidated or overwrote parent");
  };
  recurse(recurse, 256); // Much deeper than the bounded mate depth or 108 plies.
  require(scratch.active() == 0 && scratch.frames().size() == 257,
          "scratch did not unwind");
  const auto *address = scratch.frames().front()->values.data();
  recurse(recurse, 256);
  require(scratch.frames().front()->values.data() == address,
          "warm scratch lost capacity");
  try {
    Scratch::Lease outer(scratch);
    outer.get().values.push_back(42);
    Scratch::Lease inner(scratch);
    throw std::runtime_error("cancellation");
  } catch (const std::runtime_error &) {}
  require(scratch.active() == 0, "exception retained a scratch lease");
  Scratch copy(scratch);
  require(copy.active() == 0 && copy.frames().empty(),
          "solver copy retained scratch state");
  copy = scratch;
  Scratch::Lease lease(scratch);
  require(lease.get().values.empty(), "lease exposed stale values");
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
    test_recursive_scratch_lifetime();
    test_purchase_visit_diagnostics();
    test_common_search_values();
    test_reveal_score_order_and_locality();
    test_defender_reserve_order_depends_on_return_colour();
    test_solver_tt_layout_and_value_contracts();
    test_solver_tt_full_key_collision_contract();
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
