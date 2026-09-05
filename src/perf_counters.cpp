#include "perf_counters.h"

#include <array>
#include <atomic>

namespace csplendor::perf {
namespace {

constexpr std::array<const char *, COUNTER_COUNT> COUNTER_NAMES = {{
    "exact_hash_calls",
    "exact_hash_cache_hits",
    "exact_hash_cache_misses",
    "exact_hash_fields_visited",
    "exact_deck_card_salts_visited",
    "observable_hash_calls",
    "observable_hash_fields_visited",
    "hash_oracle_failures",
    "clone_light_calls",
    "determinization_clone_calls",
    "board_snapshot_copies",
    "board_restores",
    "action_vector_reallocations",
    "purchased_card_vector_reallocations",
    "acquired_noble_vector_reallocations",
    "solver_temporary_vector_allocations",
    "solver_temporary_set_allocations",
    "solver_state_key_calls",
    "solver_set_deck_hash_calls",
    "solver_state_key_fields_visited",
    "solver_scanned_deck_cards",
    "solver_scanned_purchased_ids",
    "solver_is_claimed_calls",
    "solver_is_claimed_comparisons",
    "solver_reveal_state_fast_initializations",
    "solver_reveal_state_fallback_initializations",
    "solver_reveal_state_fast_key_reads",
    "solver_reveal_state_transitions",
    "solver_reveal_state_runtime_fallbacks",
    "solver_reveal_state_oracle_checks",
    "solver_reveal_state_oracle_failures",
    "solver_path_finds",
    "solver_path_inserts",
    "solver_path_erases",
    "solver_path_depth_samples",
    "solver_path_depth_sum",
    "solver_path_depth_max",
    "solver_path_depth_0",
    "solver_path_depth_1_to_2",
    "solver_path_depth_3_to_4",
    "solver_path_depth_5_to_8",
    "solver_path_depth_9_plus",
    "solver_path_linear_comparisons",
    "solver_tt_probes",
    "solver_tt_hits",
    "solver_tt_stores",
    "solver_tt_key_comparisons",
    "solver_tt_probe_length_0",
    "solver_tt_probe_length_1",
    "solver_tt_probe_length_2",
    "solver_tt_probe_length_3_to_4",
    "solver_tt_probe_length_5_plus",
    "solver_board_rollbacks",
    "solver_reveal_candidates",
    "solver_card_equivalence_lookups",
    "parallel_tree_lookups",
    "parallel_access_epoch_updates",
    "parallel_edge_lookups",
    "parallel_edge_comparisons",
    "parallel_reservation_occupancy_samples",
    "parallel_reservation_occupancy_sum",
    "parallel_reservation_occupancy_max",
    "parallel_reservation_occupancy_0",
    "parallel_reservation_occupancy_1",
    "parallel_reservation_occupancy_2",
    "parallel_reservation_occupancy_3",
    "parallel_reservation_occupancy_4_plus",
    "parallel_live_reservation_inserts",
    "parallel_live_reservation_allocations",
    "parallel_live_reservation_rehashes",
    "parallel_ledger_atomic_increments",
    "parallel_ledger_issuance_atomic_increments",
    "parallel_ledger_selection_atomic_increments",
    "parallel_ledger_evaluation_atomic_increments",
    "parallel_ledger_completion_atomic_increments",
    "parallel_ledger_reservation_atomic_increments",
    "parallel_ledger_error_atomic_increments",
    "parallel_node_lock_acquisitions",
    "parallel_node_lock_wait_nanoseconds",
    "parallel_node_lock_hold_nanoseconds",
    "parallel_shard_lock_acquisitions",
    "parallel_shard_lock_wait_nanoseconds",
    "parallel_shard_lock_hold_nanoseconds",
    "parallel_queue_wait_nanoseconds",
    "parallel_queue_full_waits",
    "parallel_queue_empty_waits",
    "parallel_worker_idle_nanoseconds",
    "parallel_coordinator_idle_nanoseconds",
    "parallel_batch_count",
    "parallel_batch_items",
}};

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
std::array<std::atomic<uint64_t>, COUNTER_COUNT> counters{};
thread_local uint64_t *active_solver_tt_comparisons = nullptr;
thread_local BlockingRole blocking_role = BlockingRole::None;
#endif

} // namespace

const char *counter_name(Counter counter) noexcept {
  const size_t index = static_cast<size_t>(counter);
  return index < COUNTER_NAMES.size() ? COUNTER_NAMES[index]
                                      : "unknown_counter";
}

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
void reset() noexcept {
  for (auto &counter : counters)
    counter.store(0, std::memory_order_relaxed);
}

void add(Counter counter, uint64_t amount) noexcept {
  counters[static_cast<size_t>(counter)].fetch_add(amount,
                                                   std::memory_order_relaxed);
  if (counter == Counter::ParallelQueueWaitNanoseconds) {
    if (blocking_role == BlockingRole::Worker)
      counters[static_cast<size_t>(Counter::ParallelWorkerIdleNanoseconds)]
          .fetch_add(amount, std::memory_order_relaxed);
    else if (blocking_role == BlockingRole::Coordinator)
      counters[static_cast<size_t>(Counter::ParallelCoordinatorIdleNanoseconds)]
          .fetch_add(amount, std::memory_order_relaxed);
  }
}

void update_max(Counter counter, uint64_t candidate) noexcept {
  auto &target = counters[static_cast<size_t>(counter)];
  uint64_t observed = target.load(std::memory_order_relaxed);
  while (observed < candidate &&
         !target.compare_exchange_weak(observed, candidate,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
  }
}

SolverTtProbeScope::SolverTtProbeScope() noexcept
    : previous_(active_solver_tt_comparisons) {
  add(Counter::SolverTtProbes);
  active_solver_tt_comparisons = &comparisons_;
}

SolverTtProbeScope::~SolverTtProbeScope() { finish(); }

void SolverTtProbeScope::finish() noexcept {
  if (!active_)
    return;
  active_solver_tt_comparisons = previous_;
  Counter bucket = Counter::SolverTtProbeLength5Plus;
  if (comparisons_ == 0)
    bucket = Counter::SolverTtProbeLength0;
  else if (comparisons_ == 1)
    bucket = Counter::SolverTtProbeLength1;
  else if (comparisons_ == 2)
    bucket = Counter::SolverTtProbeLength2;
  else if (comparisons_ <= 4)
    bucket = Counter::SolverTtProbeLength3To4;
  add(bucket);
  active_ = false;
}

BlockingCallScope::BlockingCallScope(BlockingRole role) noexcept
    : previous_(blocking_role) {
  blocking_role = role;
}

BlockingCallScope::~BlockingCallScope() { blocking_role = previous_; }

void note_solver_tt_key_comparison() noexcept {
  if (!active_solver_tt_comparisons)
    return;
  add(Counter::SolverTtKeyComparisons);
  ++*active_solver_tt_comparisons;
}

void record_reservation_occupancy(size_t occupancy) noexcept {
  add(Counter::ParallelReservationOccupancySamples);
  add(Counter::ParallelReservationOccupancySum, occupancy);
  update_max(Counter::ParallelReservationOccupancyMax, occupancy);
  const Counter bucket =
      occupancy == 0   ? Counter::ParallelReservationOccupancy0
      : occupancy == 1 ? Counter::ParallelReservationOccupancy1
      : occupancy == 2 ? Counter::ParallelReservationOccupancy2
      : occupancy == 3 ? Counter::ParallelReservationOccupancy3
                       : Counter::ParallelReservationOccupancy4Plus;
  add(bucket);
}

void record_live_reservation_insert(size_t buckets_before,
                                    size_t buckets_after) noexcept {
  add(Counter::ParallelLiveReservationInserts);
  add(Counter::ParallelLiveReservationAllocations);
  if (buckets_before != buckets_after)
    add(Counter::ParallelLiveReservationRehashes);
}

Snapshot snapshot() noexcept {
  Snapshot result;
  for (size_t index = 0; index < counters.size(); ++index)
    result.values[index] = counters[index].load(std::memory_order_relaxed);
  return result;
}
#endif

} // namespace csplendor::perf
