#include "game.h"
#include "perf_counters.h"

#include <cstddef>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using csplendor::perf::Counter;
using csplendor::perf::Snapshot;

void check(bool condition) {
  if (!condition)
    std::abort();
}

void check_all_zero(const Snapshot &snapshot) {
  for (const uint64_t value : snapshot.values)
    check(value == 0);
}

void exercise_representative_counters() {
  Game game(42);
  game.board.invalidate_hash();
  const uint64_t cold_hash = game.board.hash();
  check(game.board.hash() == cold_hash);

  (void)game.board.observable_hash(0);

  const Game light = game.clone_light();
  check(light.board.hash() == game.board.hash());

  const Game determinized = game.shuffled_clone_portable(0, 17);
  check(determinized.board.current_player == game.board.current_player);
}

void test_counter_contract() {
  csplendor::perf::reset();
  check_all_zero(csplendor::perf::snapshot());

#ifndef CSPLENDOR_PERF_INSTRUMENTATION
  uint64_t unevaluated = 0;
  CSPLENDOR_PERF_ADD(ExactHashCalls, ++unevaluated);
  CSPLENDOR_PERF_MAX(ExactHashCacheHits, ++unevaluated);
  CSPLENDOR_PERF_HASH_FIELDS(true, ++unevaluated);
  CSPLENDOR_PERF_RESERVATION_OCCUPANCY(++unevaluated);
  CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(++unevaluated, ++unevaluated);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerErrorAtomicIncrements, ++unevaluated);
  CSPLENDOR_PERF_TRAVERSAL_DEPTH(++unevaluated);
  check(unevaluated == 0);
#endif

  exercise_representative_counters();
  const Snapshot exercised = csplendor::perf::snapshot();

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  check(exercised.get(Counter::ExactHashCalls) >= 2);
  check(exercised.get(Counter::ExactHashCacheMisses) >= 1);
  check(exercised.get(Counter::ExactHashCacheHits) >= 1);
  check(exercised.get(Counter::ExactHashFieldsVisited) > 0);
  check(exercised.get(Counter::ExactDeckCardSaltsVisited) > 0);
  check(exercised.get(Counter::ObservableHashCalls) >= 1);
  check(exercised.get(Counter::ObservableHashFieldsVisited) > 0);
  check(exercised.get(Counter::CloneLightCalls) >= 1);
  check(exercised.get(Counter::DeterminizationCloneCalls) >= 1);
  check(exercised.get(Counter::BoardSnapshotCopies) >= 2);
#else
  check_all_zero(exercised);
#endif

  csplendor::perf::reset();
  check_all_zero(csplendor::perf::snapshot());
}

void test_detailed_instrumentation_contract() {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  csplendor::perf::reset();
  // Comparisons outside a TT probe belong to path/dedup containers and must
  // not contaminate the TT collision counters.
  CSPLENDOR_PERF_TT_KEY_COMPARISON();
  {
    CSPLENDOR_PERF_TT_PROBE_SCOPE(probe0);
    CSPLENDOR_PERF_TT_PROBE_FINISH(probe0);
  }
  {
    CSPLENDOR_PERF_TT_PROBE_SCOPE(probe1);
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_PROBE_FINISH(probe1);
  }
  {
    CSPLENDOR_PERF_TT_PROBE_SCOPE(probe2);
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_PROBE_FINISH(probe2);
  }
  {
    CSPLENDOR_PERF_TT_PROBE_SCOPE(probe3);
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_PROBE_FINISH(probe3);
  }
  {
    CSPLENDOR_PERF_TT_PROBE_SCOPE(probe5);
    for (int comparison = 0; comparison < 5; ++comparison)
      CSPLENDOR_PERF_TT_KEY_COMPARISON();
    CSPLENDOR_PERF_TT_PROBE_FINISH(probe5);
  }
  CSPLENDOR_PERF_RESERVATION_OCCUPANCY(0);
  CSPLENDOR_PERF_RESERVATION_OCCUPANCY(4);
  CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(1, 1);
  CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(1, 13);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerIssuanceAtomicIncrements, 1);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerSelectionAtomicIncrements, 2);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerEvaluationAtomicIncrements, 3);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerCompletionAtomicIncrements, 4);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerReservationAtomicIncrements, 5);
  CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerErrorAtomicIncrements, 6);
  {
    CSPLENDOR_PERF_BLOCKING_SCOPE(worker_wait, Worker);
    CSPLENDOR_PERF_ADD(ParallelQueueWaitNanoseconds, 7);
  }

  const Snapshot details = csplendor::perf::snapshot();
  check(details.get(Counter::SolverTtProbes) == 5);
  check(details.get(Counter::SolverTtKeyComparisons) == 11);
  check(details.get(Counter::SolverTtProbeLength0) == 1);
  check(details.get(Counter::SolverTtProbeLength1) == 1);
  check(details.get(Counter::SolverTtProbeLength2) == 1);
  check(details.get(Counter::SolverTtProbeLength3To4) == 1);
  check(details.get(Counter::SolverTtProbeLength5Plus) == 1);
  check(details.get(Counter::SolverTtProbes) ==
        details.get(Counter::SolverTtProbeLength0) +
            details.get(Counter::SolverTtProbeLength1) +
            details.get(Counter::SolverTtProbeLength2) +
            details.get(Counter::SolverTtProbeLength3To4) +
            details.get(Counter::SolverTtProbeLength5Plus));
  check(details.get(Counter::ParallelReservationOccupancySamples) == 2);
  check(details.get(Counter::ParallelReservationOccupancy0) == 1);
  check(details.get(Counter::ParallelReservationOccupancy4Plus) == 1);
  check(details.get(Counter::ParallelLiveReservationAllocations) == 2);
  check(details.get(Counter::ParallelLiveReservationRehashes) == 1);
  const uint64_t ledger_group_sum =
      details.get(Counter::ParallelLedgerIssuanceAtomicIncrements) +
      details.get(Counter::ParallelLedgerSelectionAtomicIncrements) +
      details.get(Counter::ParallelLedgerEvaluationAtomicIncrements) +
      details.get(Counter::ParallelLedgerCompletionAtomicIncrements) +
      details.get(Counter::ParallelLedgerReservationAtomicIncrements) +
      details.get(Counter::ParallelLedgerErrorAtomicIncrements);
  check(details.get(Counter::ParallelLedgerAtomicIncrements) ==
        ledger_group_sum);
  check(ledger_group_sum == 21);
  check(details.get(Counter::ParallelQueueWaitNanoseconds) == 7);
  check(details.get(Counter::ParallelWorkerIdleNanoseconds) == 7);

  Game game(42);
  csplendor::perf::reset();
  (void)game.board.compute_set_deck_search_hash();
  const Snapshot set_deck = csplendor::perf::snapshot();
  check(set_deck.get(Counter::SolverSetDeckHashCalls) == 1);
  check(set_deck.get(Counter::SolverStateKeyFieldsVisited) > 0);
  check(set_deck.get(Counter::ExactHashFieldsVisited) == 0);
#endif
}

void test_concurrent_counter_updates() {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  constexpr uint64_t thread_count = 8;
  constexpr uint64_t increments_per_thread = 10000;
  csplendor::perf::reset();
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (uint64_t thread_index = 0; thread_index < thread_count; ++thread_index) {
    workers.emplace_back([thread_index] {
      for (uint64_t increment = 0; increment < increments_per_thread;
           ++increment) {
        csplendor::perf::add(Counter::ExactHashCalls);
        csplendor::perf::update_max(Counter::ParallelReservationOccupancyMax,
                                    thread_index * increments_per_thread +
                                        increment + 1);
      }
    });
  }
  for (auto &worker : workers)
    worker.join();

  const Snapshot concurrent = csplendor::perf::snapshot();
  check(concurrent.get(Counter::ExactHashCalls) ==
        thread_count * increments_per_thread);
  check(concurrent.get(Counter::ParallelReservationOccupancyMax) ==
        thread_count * increments_per_thread);
#endif
}

} // namespace

int main() {
  test_counter_contract();
  test_detailed_instrumentation_contract();
  test_concurrent_counter_updates();
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  csplendor::perf::reset();
  check(csplendor::perf::traversal_depth() == -1);
  {
    CSPLENDOR_PERF_TRAVERSAL_DEPTH(0);
    CSPLENDOR_PERF_RESERVATION_OCCUPANCY(32);
    csplendor::perf::record_traversal_lock(0, true, 10);
    csplendor::perf::record_traversal_lock(0, false, 20);
    {
      CSPLENDOR_PERF_TRAVERSAL_DEPTH(1);
      CSPLENDOR_PERF_RESERVATION_OCCUPANCY(4);
    }
    check(csplendor::perf::traversal_depth() == 0);
    std::thread worker([] {
      check(csplendor::perf::traversal_depth() == -1);
      CSPLENDOR_PERF_TRAVERSAL_DEPTH(9);
      CSPLENDOR_PERF_RESERVATION_OCCUPANCY(1);
    });
    worker.join();
  }
  check(csplendor::perf::traversal_depth() == -1);
  const auto levels = csplendor::perf::snapshot();
  check(levels.get(Counter::ParallelTraversalRootReservationMax) == 32);
  check(levels.get(Counter::ParallelTraversalRootReservationSixteenPlus) == 1);
  check(levels.get(Counter::ParallelTraversalRootLockAcquisitions) == 1);
  check(levels.get(Counter::ParallelTraversalRootLockWaitNanoseconds) == 10);
  check(levels.get(Counter::ParallelTraversalRootLockHoldNanoseconds) == 20);
  check(levels.get(Counter::ParallelTraversalDepthOneReservationMax) == 4);
  check(levels.get(Counter::ParallelTraversalDeepReservationMax) == 1);
#endif
  return 0;
}
