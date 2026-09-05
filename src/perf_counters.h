#ifndef CSPLENDOR_PERF_COUNTERS_H
#define CSPLENDOR_PERF_COUNTERS_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace csplendor::perf {

// Phase-0 diagnostic counters. They are intentionally unavailable to game
// logic and public bindings: an instrumentation build reads them only from
// native benchmark code. Keep the normal Release build free of branches,
// atomics and TLS by compiling every call site out when the option is OFF.
enum class Counter : size_t {
  ExactHashCalls,
  ExactHashCacheHits,
  ExactHashCacheMisses,
  ExactHashFieldsVisited,
  ExactDeckCardSaltsVisited,
  ObservableHashCalls,
  ObservableHashFieldsVisited,
  HashOracleFailures,
  CloneLightCalls,
  DeterminizationCloneCalls,
  BoardSnapshotCopies,
  BoardRestores,
  ActionVectorReallocations,
  PurchasedCardVectorReallocations,
  AcquiredNobleVectorReallocations,
  SolverTemporaryVectorAllocations,
  SolverScratchFrameCountMax,
  SolverScratchPayloadBytesMax,
  SolverScratchActionCapacityMax,
  SolverScratchRevealCapacityMax,
  SolverScratchActions0,
  SolverScratchActions1To16,
  SolverScratchActions17Plus,
  SolverScratchReveals0,
  SolverScratchReveals1To16,
  SolverScratchReveals17Plus,
  SolverTemporarySetAllocations,
  SolverStateKeyCalls,
  SolverSetDeckHashCalls,
  SolverStateKeyFieldsVisited,
  SolverScannedDeckCards,
  SolverScannedPurchasedIds,
  SolverIsClaimedCalls,
  SolverIsClaimedComparisons,
  SolverRevealStateFastInitializations,
  SolverRevealStateFallbackInitializations,
  SolverRevealStateFastKeyReads,
  SolverRevealStateTransitions,
  SolverRevealStateRuntimeFallbacks,
  SolverRevealStateOracleChecks,
  SolverRevealStateOracleFailures,
  SolverPathFinds,
  SolverPathInserts,
  SolverPathErases,
  SolverPathDepthSamples,
  SolverPathDepthSum,
  SolverPathDepthMax,
  SolverPathDepth0,
  SolverPathDepth1To2,
  SolverPathDepth3To4,
  SolverPathDepth5To8,
  SolverPathDepth9Plus,
  SolverPathLinearComparisons,
  SolverTtProbes,
  SolverTtHits,
  SolverTtStores,
  SolverTtKeyComparisons,
  SolverTtProbeLength0,
  SolverTtProbeLength1,
  SolverTtProbeLength2,
  SolverTtProbeLength3To4,
  SolverTtProbeLength5Plus,
  SolverBoardRollbacks,
  SolverCompactRollbackCaptures,
  SolverFullRollbackCaptures,
  SolverCompactRollbackRestores,
  SolverFullRollbackRestores,
  SolverRevealCandidates,
  SolverVisibleRefillScoreCalls,
  SolverDefenderReserveScoreCalls,
  SolverVisibleRefillSortCandidates,
  SolverDefenderReserveSortCandidates,
  SolverCardEquivalenceLookups,
  ParallelTreeLookups,
  ParallelAccessEpochUpdates,
  ParallelEdgeLookups,
  ParallelEdgeComparisons,
  ParallelReservationOccupancySamples,
  ParallelReservationOccupancySum,
  ParallelReservationOccupancyMax,
  ParallelReservationOccupancy0,
  ParallelReservationOccupancy1,
  ParallelReservationOccupancy2,
  ParallelReservationOccupancy3,
  ParallelReservationOccupancy4Plus,
  ParallelLiveReservationInserts,
  ParallelLiveReservationAllocations,
  ParallelLiveReservationRehashes,
  ParallelLedgerAtomicIncrements,
  ParallelLedgerIssuanceAtomicIncrements,
  ParallelLedgerSelectionAtomicIncrements,
  ParallelLedgerEvaluationAtomicIncrements,
  ParallelLedgerCompletionAtomicIncrements,
  ParallelLedgerReservationAtomicIncrements,
  ParallelLedgerErrorAtomicIncrements,
  ParallelNodeLockAcquisitions,
  ParallelNodeLockWaitNanoseconds,
  ParallelNodeLockHoldNanoseconds,
  ParallelShardLockAcquisitions,
  ParallelShardLockWaitNanoseconds,
  ParallelShardLockHoldNanoseconds,
  ParallelQueueWaitNanoseconds,
  ParallelQueueFullWaits,
  ParallelQueueEmptyWaits,
  ParallelWorkerIdleNanoseconds,
  ParallelCoordinatorIdleNanoseconds,
  ParallelBatchCount,
  ParallelBatchItems,
  Count,
};

inline constexpr size_t COUNTER_COUNT = static_cast<size_t>(Counter::Count);

struct Snapshot {
  std::array<uint64_t, COUNTER_COUNT> values{};

  uint64_t get(Counter counter) const noexcept {
    return values[static_cast<size_t>(counter)];
  }
};

const char *counter_name(Counter counter) noexcept;

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
enum class BlockingRole : uint8_t { None, Worker, Coordinator };

class SolverTtProbeScope {
public:
  SolverTtProbeScope() noexcept;
  SolverTtProbeScope(const SolverTtProbeScope &) = delete;
  SolverTtProbeScope &operator=(const SolverTtProbeScope &) = delete;
  ~SolverTtProbeScope();

  void finish() noexcept;

private:
  uint64_t comparisons_ = 0;
  uint64_t *previous_ = nullptr;
  bool active_ = true;
};

class BlockingCallScope {
public:
  explicit BlockingCallScope(BlockingRole role) noexcept;
  BlockingCallScope(const BlockingCallScope &) = delete;
  BlockingCallScope &operator=(const BlockingCallScope &) = delete;
  ~BlockingCallScope();

private:
  BlockingRole previous_ = BlockingRole::None;
};

// Counter updates are safe from multiple participating threads. reset() must
// run before those producers start. snapshot() is data-race-free during an
// active search, but its independently relaxed loads are not a coherent
// multi-counter observation; exact totals and cross-counter invariants require
// every producer to be joined first.
void reset() noexcept;
void add(Counter counter, uint64_t amount = 1) noexcept;
void update_max(Counter counter, uint64_t candidate) noexcept;
void note_solver_tt_key_comparison() noexcept;
void record_reservation_occupancy(size_t occupancy) noexcept;
void record_live_reservation_insert(size_t buckets_before,
                                    size_t buckets_after) noexcept;
Snapshot snapshot() noexcept;
#else
inline void reset() noexcept {}
inline void add(Counter, uint64_t = 1) noexcept {}
inline void update_max(Counter, uint64_t) noexcept {}
inline Snapshot snapshot() noexcept { return {}; }
#endif

} // namespace csplendor::perf

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
#define CSPLENDOR_PERF_INC(name)                                               \
  ::csplendor::perf::add(::csplendor::perf::Counter::name)
#define CSPLENDOR_PERF_ADD(name, amount)                                       \
  ::csplendor::perf::add(::csplendor::perf::Counter::name,                     \
                         static_cast<uint64_t>(amount))
#define CSPLENDOR_PERF_MAX(name, candidate)                                    \
  ::csplendor::perf::update_max(::csplendor::perf::Counter::name,              \
                                static_cast<uint64_t>(candidate))
#define CSPLENDOR_PERF_HASH_FIELDS(exact, amount)                              \
  do {                                                                         \
    if constexpr (exact)                                                       \
      CSPLENDOR_PERF_ADD(ExactHashFieldsVisited, amount);                      \
    else                                                                       \
      CSPLENDOR_PERF_ADD(SolverStateKeyFieldsVisited, amount);                 \
  } while (false)
#define CSPLENDOR_PERF_TT_KEY_COMPARISON()                                     \
  ::csplendor::perf::note_solver_tt_key_comparison()
#define CSPLENDOR_PERF_TT_PROBE_SCOPE(name)                                    \
  ::csplendor::perf::SolverTtProbeScope name
#define CSPLENDOR_PERF_TT_PROBE_FINISH(name) name.finish()
#define CSPLENDOR_PERF_RESERVATION_OCCUPANCY(occupancy)                        \
  ::csplendor::perf::record_reservation_occupancy(                             \
      static_cast<size_t>(occupancy))
#define CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(before, after)                  \
  ::csplendor::perf::record_live_reservation_insert(                           \
      static_cast<size_t>(before), static_cast<size_t>(after))
#define CSPLENDOR_PERF_LEDGER_ADD(group, amount)                               \
  do {                                                                         \
    CSPLENDOR_PERF_ADD(ParallelLedgerAtomicIncrements, amount);                \
    CSPLENDOR_PERF_ADD(group, amount);                                         \
  } while (false)
#define CSPLENDOR_PERF_BLOCKING_SCOPE(name, role)                              \
  ::csplendor::perf::BlockingCallScope name(                                   \
      ::csplendor::perf::BlockingRole::role)
#else
#define CSPLENDOR_PERF_INC(name)                                               \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_ADD(name, amount)                                       \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_MAX(name, candidate)                                    \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_HASH_FIELDS(exact, amount)                              \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_TT_KEY_COMPARISON()                                     \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_TT_PROBE_SCOPE(name)                                    \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_TT_PROBE_FINISH(name)                                   \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_RESERVATION_OCCUPANCY(occupancy)                        \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(before, after)                  \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_LEDGER_ADD(group, amount)                               \
  do {                                                                         \
  } while (false)
#define CSPLENDOR_PERF_BLOCKING_SCOPE(name, role)                              \
  do {                                                                         \
  } while (false)
#endif

#endif // CSPLENDOR_PERF_COUNTERS_H
