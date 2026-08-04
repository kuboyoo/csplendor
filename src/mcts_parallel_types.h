#ifndef CSPLENDOR_MCTS_PARALLEL_TYPES_H
#define CSPLENDOR_MCTS_PARALLEL_TYPES_H

#include "mcts_rng.h"
#include "mcts_tree_key.h"
#include "mcts_types.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mcts_parallel {

class TreeCapacityReachedError : public std::runtime_error {
public:
  explicit TreeCapacityReachedError(const char *message)
      : std::runtime_error(message) {}
};

// Dense masks remain the public DTO/binding representation. Search and tree
// internals use ActionMaskBits from mcts_types.h.
using ActionMask = DenseActionMask;
using Policy = std::array<float, MAX_ACTIONS>;
using Value = std::array<double, NUM_PLAYERS>;

enum class TreeBackend : uint8_t { Coarse = 0, Sharded = 1 };

enum class ParallelSearchMode : uint8_t {
  Throughput = 0,
  DeterministicEpoch = 1,
  RootParallel = 2,
};

enum class ExpansionState : uint8_t {
  Unexpanded = 0,
  Evaluating = 1,
  Expanded = 2,
  Terminal = 3,
};

enum class PendingState : uint8_t {
  Open = 0,
  Closing = 1,
  Published = 2,
  Failed = 3,
  Cancelled = 4,
};

enum class TicketState : uint8_t {
  Created = 0,
  Traversing = 1,
  WaitingInference = 2,
  Committing = 3,
  Completed = 4,
  Cancelled = 5,
  Failed = 6,
};

enum class CompletionKind : uint8_t {
  None = 0,
  EvaluatedLeaf = 1,
  Terminal = 2,
  MaxDepth = 3,
};

enum class SearchStopReason : uint8_t {
  Completed = 0,
  Cancelled = 1,
  TimedOut = 2,
  TreeCapacityReached = 3,
  CallbackError = 4,
  WorkerError = 5,
};

// One-shot, copyable cooperative cancellation token. Copies share the same
// atomic state, so callers may retain a token while ParallelSearchOptions is
// copied into a running search (or into root-parallel workers). Cancellation
// never interrupts an inference callback; it is observed at the next safe
// scheduler boundary and the session then drains every pending ticket.
class ParallelCancellationToken {
public:
  ParallelCancellationToken()
      : requested_(std::make_shared<std::atomic<bool>>(false)) {}

  void request_cancel() const noexcept {
    requested_->store(true, std::memory_order_release);
  }

  bool is_cancelled() const noexcept {
    return requested_->load(std::memory_order_acquire);
  }

private:
  std::shared_ptr<std::atomic<bool>> requested_;
};

struct NodeStats64 {
  std::array<uint64_t, MAX_ACTIONS> N{};
  std::array<uint64_t, MAX_ACTIONS> virtual_loss{};
  std::array<double, MAX_ACTIONS> Q{};
  uint64_t total_visits = 0;
  Value value{};
};

struct MCTSNodeSnapshot64 {
  TreeKey key{};
  uint64_t generation = 0;
  ActionMask valid_actions{};
  Policy base_policy{};
  NodeStats64 stats{};
  std::array<uint64_t, MAX_ACTIONS> availability_count{};
  uint64_t live_reservation_count = 0;
  bool has_pending_evaluation = false;
  ExpansionState state = ExpansionState::Unexpanded;
};

struct NodeStateView {
  Value value{};
  ExpansionState state = ExpansionState::Unexpanded;
};

struct ReservedPathStep {
  TreeKey key{};
  int32_t action = -1;
  uint8_t player = 0;
};

struct SelectionContext {
  double cpuct = 1.5;
  double fpu = 0.0;
  double forced_playouts_k = 0.5;
  double dirichlet_epsilon = 0.25;
  bool forced_playouts = false;
  bool is_root = false;
  uint64_t simulation_ordinal = 0;
  uint64_t tree_generation = 0;
  const Policy *root_noise = nullptr;
};

struct ParallelSearchOptions {
  uint32_t num_threads = 1;
  uint32_t batch_size = 16;
  uint32_t batch_wait_us = 200;
  uint32_t max_inflight = 0;
  uint32_t deterministic_epoch_size = 16;
  uint64_t num_simulations = 0;
  // nullopt resolves OS entropy once from the owning MCTS/session. Explicit
  // zero remains a reproducible seed, rather than doubling as a sentinel.
  std::optional<uint64_t> master_seed = std::nullopt;
  // UINT64_MAX selects the monotonic nonce owned by MCTS. Every other value,
  // including zero, is an explicit reproducible nonce.
  uint64_t search_nonce = std::numeric_limits<uint64_t>::max();
  uint64_t simulation_id_base = 0;
  uint64_t evaluator_version = 0;
  uint64_t timeout_ms = 0;
  // Shared-tree searches apply this limit to their single tree. The
  // root-parallel runner treats the same value as an aggregate node budget and
  // divides it across active worker trees, so memory cannot silently grow by
  // num_workers. The default preserves the legacy single-tree hard limit.
  uint64_t max_tree_nodes = MAX_TREE_SIZE;
  uint32_t shard_count = 64;
  TreeBackend tree_backend = TreeBackend::Coarse;
  ParallelSearchMode mode = ParallelSearchMode::Throughput;
  ParallelCancellationToken cancellation_token{};

  // The Python root-parallel binding enables this because Python callbacks
  // must remain non-overlapping. The root runner owns the mutex so it can
  // re-check cancellation/deadline after waiting and avoid draining a stale
  // callback backlog. Native callers normally keep independent evaluators
  // concurrent and leave this false.
  bool serialize_root_callbacks = false;

  bool cancellation_requested() const noexcept {
    return cancellation_token.is_cancelled() ||
           session_stop_token_.is_cancelled();
  }

  // Root-parallel uses this private, copy-shared channel to stop sibling
  // workers after an internal failure without mutating the caller's token.
  void request_session_stop() const noexcept {
    session_stop_token_.request_cancel();
  }

  void reset_session_stop() noexcept {
    session_stop_token_ = ParallelCancellationToken{};
  }

private:
  ParallelCancellationToken session_stop_token_{};
};

struct SearchLedgerSnapshot {
  uint64_t issued = 0;
  uint64_t selected = 0;
  uint64_t evaluation_owner = 0;
  uint64_t evaluation_waiter = 0;
  uint64_t evaluation_requested = 0;
  uint64_t evaluated_boards = 0;
  uint64_t completed_evaluated = 0;
  uint64_t completed_terminal = 0;
  uint64_t completed_max_depth = 0;
  uint64_t cancelled = 0;
  uint64_t failed = 0;
  uint64_t virtual_loss_added = 0;
  uint64_t virtual_loss_released = 0;
  uint64_t reservations_committed = 0;
  uint64_t reservations_aborted = 0;
  uint64_t expansion_claimed = 0;
  uint64_t expansion_published = 0;
  uint64_t expansion_waited = 0;
  uint64_t stale_result = 0;
  uint64_t duplicate_result = 0;
  uint64_t invalid_replay = 0;
  uint64_t integrity_errors = 0;
  uint64_t max_inflight_observed = 0;

  uint64_t completed() const noexcept {
    return completed_evaluated + completed_terminal + completed_max_depth;
  }

  bool virtual_loss_balanced() const noexcept {
    return virtual_loss_added == virtual_loss_released;
  }
};

// Shared by reservations and a search session. All fields are monotonic so a
// relaxed snapshot is sufficient for metrics; tree/ticket locks establish the
// correctness ordering.
struct SearchLedger {
#define CSPLENDOR_LEDGER_COUNTER(name)                                         \
  std::atomic<uint64_t> name { 0 }
  CSPLENDOR_LEDGER_COUNTER(issued);
  CSPLENDOR_LEDGER_COUNTER(selected);
  CSPLENDOR_LEDGER_COUNTER(evaluation_owner);
  CSPLENDOR_LEDGER_COUNTER(evaluation_waiter);
  CSPLENDOR_LEDGER_COUNTER(evaluation_requested);
  CSPLENDOR_LEDGER_COUNTER(evaluated_boards);
  CSPLENDOR_LEDGER_COUNTER(completed_evaluated);
  CSPLENDOR_LEDGER_COUNTER(completed_terminal);
  CSPLENDOR_LEDGER_COUNTER(completed_max_depth);
  CSPLENDOR_LEDGER_COUNTER(cancelled);
  CSPLENDOR_LEDGER_COUNTER(failed);
  CSPLENDOR_LEDGER_COUNTER(virtual_loss_added);
  CSPLENDOR_LEDGER_COUNTER(virtual_loss_released);
  CSPLENDOR_LEDGER_COUNTER(reservations_committed);
  CSPLENDOR_LEDGER_COUNTER(reservations_aborted);
  CSPLENDOR_LEDGER_COUNTER(expansion_claimed);
  CSPLENDOR_LEDGER_COUNTER(expansion_published);
  CSPLENDOR_LEDGER_COUNTER(expansion_waited);
  CSPLENDOR_LEDGER_COUNTER(stale_result);
  CSPLENDOR_LEDGER_COUNTER(duplicate_result);
  CSPLENDOR_LEDGER_COUNTER(invalid_replay);
  CSPLENDOR_LEDGER_COUNTER(integrity_errors);
  CSPLENDOR_LEDGER_COUNTER(max_inflight_observed);
#undef CSPLENDOR_LEDGER_COUNTER

  static void update_max(std::atomic<uint64_t> &target,
                         uint64_t candidate) noexcept {
    uint64_t observed = target.load(std::memory_order_relaxed);
    while (observed < candidate &&
           !target.compare_exchange_weak(observed, candidate,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
  }

  SearchLedgerSnapshot snapshot() const noexcept {
    SearchLedgerSnapshot out;
#define CSPLENDOR_LEDGER_LOAD(name)                                            \
  out.name = name.load(std::memory_order_relaxed)
    CSPLENDOR_LEDGER_LOAD(issued);
    CSPLENDOR_LEDGER_LOAD(selected);
    CSPLENDOR_LEDGER_LOAD(evaluation_owner);
    CSPLENDOR_LEDGER_LOAD(evaluation_waiter);
    CSPLENDOR_LEDGER_LOAD(evaluation_requested);
    CSPLENDOR_LEDGER_LOAD(evaluated_boards);
    CSPLENDOR_LEDGER_LOAD(completed_evaluated);
    CSPLENDOR_LEDGER_LOAD(completed_terminal);
    CSPLENDOR_LEDGER_LOAD(completed_max_depth);
    CSPLENDOR_LEDGER_LOAD(cancelled);
    CSPLENDOR_LEDGER_LOAD(failed);
    CSPLENDOR_LEDGER_LOAD(virtual_loss_added);
    CSPLENDOR_LEDGER_LOAD(virtual_loss_released);
    CSPLENDOR_LEDGER_LOAD(reservations_committed);
    CSPLENDOR_LEDGER_LOAD(reservations_aborted);
    CSPLENDOR_LEDGER_LOAD(expansion_claimed);
    CSPLENDOR_LEDGER_LOAD(expansion_published);
    CSPLENDOR_LEDGER_LOAD(expansion_waited);
    CSPLENDOR_LEDGER_LOAD(stale_result);
    CSPLENDOR_LEDGER_LOAD(duplicate_result);
    CSPLENDOR_LEDGER_LOAD(invalid_replay);
    CSPLENDOR_LEDGER_LOAD(integrity_errors);
    CSPLENDOR_LEDGER_LOAD(max_inflight_observed);
#undef CSPLENDOR_LEDGER_LOAD
    return out;
  }
};

struct ParallelInferenceRequest {
  uint64_t pending_id = 0;
  uint64_t owner_simulation_id = 0;
  TreeKey key{};
  std::array<float, FEATURE_SIZE> features{};
  ActionMask owner_world_mask{};
};

struct ParallelInferenceResult {
  Policy policy{};
  std::array<float, NUM_PLAYERS> value{};
};

struct ParallelSearchResult {
  std::array<uint64_t, MAX_ACTIONS> visits{};
  std::array<double, MAX_ACTIONS> q_values{};
  std::array<float, MAX_ACTIONS> probabilities{};
  SearchLedgerSnapshot ledger{};
  SearchStopReason stop_reason = SearchStopReason::Completed;
  uint64_t resolved_seed = 0;
  uint32_t rng_version = MCTS_RNG_VERSION;
  uint64_t search_nonce = 0;
  uint64_t tree_generation = 0;
  uint64_t tree_size = 0;
  uint64_t elapsed_microseconds = 0;
  bool partial = false;
};

inline bool finite_value(const Value &value) noexcept {
  for (double element : value) {
    if (!std::isfinite(element))
      return false;
  }
  return true;
}

inline Value checked_value(const std::array<float, NUM_PLAYERS> &value) {
  Value out{};
  for (size_t i = 0; i < NUM_PLAYERS; ++i) {
    if (!std::isfinite(value[i]))
      throw std::invalid_argument("inference value must be finite");
    out[i] = static_cast<double>(value[i]);
  }
  return out;
}

inline void validate_policy(const Policy &policy) {
  for (float element : policy) {
    if (!std::isfinite(element) || element < 0.0f)
      throw std::invalid_argument(
          "inference policy must contain finite non-negative values");
  }
}

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_PARALLEL_TYPES_H
