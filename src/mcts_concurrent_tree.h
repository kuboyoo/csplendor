#ifndef CSPLENDOR_MCTS_CONCURRENT_TREE_H
#define CSPLENDOR_MCTS_CONCURRENT_TREE_H

#include "mcts_parallel_types.h"
#include "perf_counters.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
#include <chrono>
#endif

namespace mcts_parallel {

struct PendingEvaluation;

namespace detail {

struct ConcurrentTreeState;

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
class LockTimer {
public:
  LockTimer(csplendor::perf::Counter acquisitions,
            csplendor::perf::Counter wait,
            csplendor::perf::Counter hold, bool node_operation = false) noexcept
      : acquisitions_(acquisitions), wait_(wait), hold_(hold),
        started_(Clock::now()), depth_(node_operation ? csplendor::perf::traversal_depth() : -1) {}

  void acquired() noexcept {
    acquired_ = Clock::now();
    csplendor::perf::add(acquisitions_);
    csplendor::perf::add(
        wait_, static_cast<uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                       acquired_ - started_)
                       .count()));
    locked_ = true;
    csplendor::perf::record_traversal_lock(depth_, true,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(acquired_ - started_).count()));
  }

  ~LockTimer() {
    if (!locked_)
      return;
    csplendor::perf::record_traversal_lock(depth_, false,
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - acquired_).count()));
    csplendor::perf::add(
        hold_, static_cast<uint64_t>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(
                       Clock::now() - acquired_)
                       .count()));
  }

private:
  using Clock = std::chrono::steady_clock;
  csplendor::perf::Counter acquisitions_;
  csplendor::perf::Counter wait_;
  csplendor::perf::Counter hold_;
  Clock::time_point started_;
  Clock::time_point acquired_{};
  bool locked_ = false;
  int depth_ = -1;
};
#endif

struct EdgeStats64 {
  uint64_t N = 0;
  uint64_t virtual_loss = 0;
  uint64_t availability_count = 0;
  double Q = 0.0;
  uint8_t action = 0;
};

struct NodeRecord {
  NodeRecord(const TreeKey &node_key, uint64_t node_generation,
             const std::shared_ptr<ConcurrentTreeState> &owner_state)
      : key(node_key), generation(node_generation), owner(owner_state) {}

  const TreeKey key;
  mutable std::mutex mutex;
  std::vector<EdgeStats64> edges;
  Policy base_policy{};
  ActionMaskBits information_set_union = 0;
  Value value{};
  uint64_t total_visits = 0;
  ExpansionState state = ExpansionState::Unexpanded;
  const uint64_t generation;
  const std::weak_ptr<ConcurrentTreeState> owner;
  std::weak_ptr<PendingEvaluation> pending;
  std::atomic<uint64_t> last_access{0};
  std::unordered_set<uint64_t> live_reservations;
};

using NodeHandle = std::shared_ptr<NodeRecord>;

struct TreeShard {
  mutable std::mutex mutex;
  std::unordered_map<TreeKey, NodeHandle, TreeKeyHash> nodes;
};

struct ConcurrentTreeState {
  ConcurrentTreeState(uint64_t initial_generation, TreeBackend selected_backend,
                      size_t max_nodes, uint32_t requested_shards)
      : generation(initial_generation), backend(selected_backend),
        capacity(max_nodes) {
    const uint32_t count = std::max<uint32_t>(1, requested_shards);
    shards.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
      shards.emplace_back(std::make_unique<TreeShard>());
  }

  std::atomic<uint64_t> generation{0};
  const TreeBackend backend;
  const size_t capacity;
  mutable std::mutex coarse_mutex;
  std::unordered_map<TreeKey, NodeHandle, TreeKeyHash> coarse_nodes;
  std::vector<std::unique_ptr<TreeShard>> shards;
  std::atomic<size_t> sharded_size{0};
  std::atomic<uint64_t> access_epoch{0};
  std::atomic<uint64_t> next_reservation_id{1};
  std::atomic<uint64_t> next_pending_id{1};
  std::atomic<uint64_t> live_reservation_count{0};
  std::atomic<uint64_t> virtual_loss_count{0};
  std::atomic<uint64_t> evaluating_node_count{0};
  std::atomic<uint64_t> pending_evaluation_count{0};
  mutable std::mutex metadata_mutex;
  bool evaluator_bound = false;
  uint64_t evaluator_version = 0;
};

template <typename Function>
decltype(auto) with_node_lock(const std::shared_ptr<ConcurrentTreeState> &tree,
                              const NodeHandle &node, Function &&function) {
  if (!node || node->owner.lock().get() != tree.get())
    throw std::logic_error("node handle belongs to a different tree");
  if (tree->backend == TreeBackend::Coarse) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    LockTimer timer(csplendor::perf::Counter::ParallelShardLockAcquisitions,
                    csplendor::perf::Counter::ParallelShardLockWaitNanoseconds,
                    csplendor::perf::Counter::ParallelShardLockHoldNanoseconds, true);
#endif
    std::lock_guard<std::mutex> lock(tree->coarse_mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    timer.acquired();
#endif
    return std::forward<Function>(function)(*node);
  }
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  LockTimer timer(csplendor::perf::Counter::ParallelNodeLockAcquisitions,
                  csplendor::perf::Counter::ParallelNodeLockWaitNanoseconds,
                  csplendor::perf::Counter::ParallelNodeLockHoldNanoseconds, true);
#endif
  std::lock_guard<std::mutex> lock(node->mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  timer.acquired();
#endif
  return std::forward<Function>(function)(*node);
}

inline size_t shard_index(const ConcurrentTreeState &tree,
                          const TreeKey &key) noexcept {
  return TreeKeyHash{}(key) % tree.shards.size();
}

inline uint64_t claim_monotonic_id(std::atomic<uint64_t> &next,
                                   const char *message) {
  uint64_t observed = next.load(std::memory_order_relaxed);
  for (;;) {
    if (observed == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error(message);
    if (next.compare_exchange_weak(observed, observed + 1,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed))
      return observed;
  }
}

inline auto find_edge(NodeRecord &record, size_t action) {
  CSPLENDOR_PERF_INC(ParallelEdgeLookups);
  return std::lower_bound(record.edges.begin(), record.edges.end(), action,
                          [](const EdgeStats64 &edge, size_t requested) {
                            CSPLENDOR_PERF_INC(ParallelEdgeComparisons);
                            return static_cast<size_t>(edge.action) < requested;
                          });
}

inline auto find_edge(const NodeRecord &record, size_t action) {
  CSPLENDOR_PERF_INC(ParallelEdgeLookups);
  return std::lower_bound(record.edges.begin(), record.edges.end(), action,
                          [](const EdgeStats64 &edge, size_t requested) {
                            CSPLENDOR_PERF_INC(ParallelEdgeComparisons);
                            return static_cast<size_t>(edge.action) < requested;
                          });
}

inline EdgeStats64 &ensure_edge(NodeRecord &record, size_t action) {
  auto iterator = find_edge(record, action);
  if (iterator == record.edges.end() || iterator->action != action) {
    EdgeStats64 edge;
    edge.action = static_cast<uint8_t>(action);
    iterator = record.edges.insert(iterator, edge);
  }
  record.information_set_union |= mcts_action_mask::bit(action);
  return *iterator;
}

inline void ensure_edges(NodeRecord &record, ActionMaskBits mask) {
  mask &= mcts_action_mask::ALL;
  const ActionMaskBits missing = mask & ~record.information_set_union;
  if (missing == 0)
    return;
  record.edges.reserve(record.edges.size() +
                       mcts_action_mask::popcount(missing));
  mcts_action_mask::for_each(
      missing, [&](size_t action) { ensure_edge(record, action); });
}

inline void decrement_nonzero(std::atomic<uint64_t> &counter) noexcept {
  const uint64_t previous = counter.fetch_sub(1, std::memory_order_relaxed);
  if (previous == 0)
    std::terminate();
}

} // namespace detail

using NodeHandle = detail::NodeHandle;

struct PendingEvaluation {
  mutable std::mutex mutex;
  PendingState state = PendingState::Open;
  uint64_t id = 0;
  TreeKey key{};
  uint64_t tree_generation = 0;
  uint64_t node_generation = 0;
  uint64_t feature_digest = 0;
  std::vector<uint64_t> attached_ticket_ids;
  ParallelInferenceRequest request{};
};

enum class ReservationState : uint8_t {
  Live = 0,
  Committed = 1,
  Aborted = 2,
  MovedFrom = 3,
};

class ReservedPathEntry {
public:
  ReservedPathEntry(const ReservedPathEntry &) = delete;
  ReservedPathEntry &operator=(const ReservedPathEntry &) = delete;

  ReservedPathEntry(ReservedPathEntry &&other) noexcept { move_from(other); }

  ReservedPathEntry &operator=(ReservedPathEntry &&other) noexcept {
    if (this != &other) {
      abort_noexcept();
      move_from(other);
    }
    return *this;
  }

  ~ReservedPathEntry() noexcept { abort_noexcept(); }

  uint64_t reservation_id() const noexcept { return reservation_id_; }
  int action() const noexcept { return action_; }
  uint8_t player() const noexcept { return player_; }
  const TreeKey &key() const noexcept { return node_->key; }
  uint64_t generation() const noexcept { return node_generation_; }
  ReservationState state() const noexcept { return state_; }
  bool live() const noexcept { return state_ == ReservationState::Live; }

  void commit(double value, uint64_t expected_tree_generation) {
    if (!std::isfinite(value))
      throw std::invalid_argument("reservation value must be finite");
    prevalidate(expected_tree_generation);
    commit_unchecked(value);
  }

  void abort() {
    if (state_ != ReservationState::Live)
      throw std::logic_error("reservation has already been consumed");
    prevalidate_token();
    abort_unchecked();
  }

private:
  friend class ConcurrentTree;
  friend class ReservedPath;

  ReservedPathEntry(std::shared_ptr<detail::ConcurrentTreeState> tree,
                    NodeHandle node, int action, uint8_t player,
                    uint64_t reservation_id,
                    std::shared_ptr<SearchLedger> ledger) noexcept
      : tree_(std::move(tree)), node_(std::move(node)), action_(action),
        player_(player), reservation_id_(reservation_id),
        node_generation_(node_->generation), ledger_(std::move(ledger)) {}

  void move_from(ReservedPathEntry &other) noexcept {
    tree_ = std::move(other.tree_);
    node_ = std::move(other.node_);
    action_ = other.action_;
    player_ = other.player_;
    reservation_id_ = other.reservation_id_;
    node_generation_ = other.node_generation_;
    ledger_ = std::move(other.ledger_);
    state_ = other.state_;
    other.state_ = ReservationState::MovedFrom;
    other.action_ = -1;
    other.reservation_id_ = 0;
  }

  void prevalidate(uint64_t expected_tree_generation) const {
    if (state_ != ReservationState::Live)
      throw std::logic_error("reservation has already been consumed");
    if (!tree_ || !node_ ||
        tree_->generation.load(std::memory_order_acquire) !=
            expected_tree_generation ||
        node_generation_ != expected_tree_generation) {
      if (ledger_) {
        CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerErrorAtomicIncrements, 1);
        ledger_->stale_result.fetch_add(1, std::memory_order_relaxed);
      }
      throw std::logic_error("stale reservation generation");
    }
    prevalidate_token();
  }

  void prevalidate_token() const {
    if (!tree_ || !node_ || action_ < 0 ||
        action_ >= static_cast<int>(MAX_ACTIONS))
      throw std::logic_error("invalid reservation");
    const bool valid = detail::with_node_lock(
        tree_, node_, [&](const detail::NodeRecord &record) {
          const auto edge =
              detail::find_edge(record, static_cast<size_t>(action_));
          return record.generation == node_generation_ &&
                 record.live_reservations.count(reservation_id_) == 1 &&
                 edge != record.edges.end() && edge->action == action_ &&
                 edge->virtual_loss > 0;
        });
    if (!valid)
      throw std::logic_error("reservation token is not live");
  }

  void commit_unchecked(double value) noexcept {
    detail::with_node_lock(tree_, node_, [&](detail::NodeRecord &record) {
      const size_t action_index = static_cast<size_t>(action_);
      auto edge = detail::find_edge(record, action_index);
      record.live_reservations.erase(reservation_id_);
      --edge->virtual_loss;
      ++edge->N;
      ++record.total_visits;
      const double count = static_cast<double>(edge->N);
      edge->Q += (value - edge->Q) / count;
    });
    detail::decrement_nonzero(tree_->virtual_loss_count);
    detail::decrement_nonzero(tree_->live_reservation_count);
    state_ = ReservationState::Committed;
    if (ledger_) {
      CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerReservationAtomicIncrements, 2);
      ledger_->virtual_loss_released.fetch_add(1, std::memory_order_relaxed);
      ledger_->reservations_committed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void abort_unchecked() noexcept {
    bool released = false;
    detail::with_node_lock(tree_, node_, [&](detail::NodeRecord &record) {
      const size_t action_index = static_cast<size_t>(action_);
      auto edge = detail::find_edge(record, action_index);
      const auto erased = record.live_reservations.erase(reservation_id_);
      if (erased == 1 && edge != record.edges.end() &&
          edge->action == action_index && edge->virtual_loss > 0) {
        --edge->virtual_loss;
        released = true;
      }
    });
    if (released) {
      detail::decrement_nonzero(tree_->virtual_loss_count);
      detail::decrement_nonzero(tree_->live_reservation_count);
    }
    state_ = ReservationState::Aborted;
    if (ledger_) {
      if (released) {
        CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerReservationAtomicIncrements, 2);
        ledger_->virtual_loss_released.fetch_add(1, std::memory_order_relaxed);
        ledger_->reservations_aborted.fetch_add(1, std::memory_order_relaxed);
      } else {
        CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerErrorAtomicIncrements, 1);
        ledger_->integrity_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }

  void abort_noexcept() noexcept {
    if (state_ != ReservationState::Live || !tree_ || !node_)
      return;
    abort_unchecked();
  }

  std::shared_ptr<detail::ConcurrentTreeState> tree_;
  NodeHandle node_;
  int action_ = -1;
  uint8_t player_ = 0;
  uint64_t reservation_id_ = 0;
  uint64_t node_generation_ = 0;
  std::shared_ptr<SearchLedger> ledger_;
  ReservationState state_ = ReservationState::Live;
};

using SelectionReservation = ReservedPathEntry;

enum class ReservedPathState : uint8_t {
  Open = 0,
  Committed = 1,
  Aborted = 2,
  MovedFrom = 3,
};

class ReservedPath {
public:
  ReservedPath() = default;
  ReservedPath(const ReservedPath &) = delete;
  ReservedPath &operator=(const ReservedPath &) = delete;

  ReservedPath(ReservedPath &&other) noexcept
      : entries_(std::move(other.entries_)), state_(other.state_) {
    other.state_ = ReservedPathState::MovedFrom;
  }

  ReservedPath &operator=(ReservedPath &&other) noexcept {
    if (this != &other) {
      abort_noexcept();
      entries_ = std::move(other.entries_);
      state_ = other.state_;
      other.state_ = ReservedPathState::MovedFrom;
    }
    return *this;
  }

  ~ReservedPath() noexcept { abort_noexcept(); }

  void append(SelectionReservation &&reservation) {
    if (state_ != ReservedPathState::Open)
      throw std::logic_error("cannot append to a consumed path");
    if (!reservation.live())
      throw std::logic_error("cannot append a consumed reservation");
    entries_.push_back(std::move(reservation));
  }

  size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }
  ReservedPathState state() const noexcept { return state_; }

  std::vector<ReservedPathStep> snapshot_steps() const {
    std::vector<ReservedPathStep> steps;
    steps.reserve(entries_.size());
    for (const auto &entry : entries_)
      steps.push_back(
          ReservedPathStep{entry.key(), entry.action(), entry.player()});
    return steps;
  }

  void commit(const Value &value, uint64_t expected_tree_generation) {
    if (state_ != ReservedPathState::Open)
      throw std::logic_error("path has already been consumed");
    if (!finite_value(value))
      throw std::invalid_argument("path value must be finite");

    // Validate the complete path before the first statistic is changed. This
    // prevents a stale/corrupt result from partially backpropagating.
    for (const auto &entry : entries_) {
      if (entry.player() >= NUM_PLAYERS)
        throw std::invalid_argument("path player is out of range");
      entry.prevalidate(expected_tree_generation);
    }
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend();
         ++iterator)
      iterator->commit_unchecked(value[iterator->player()]);
    state_ = ReservedPathState::Committed;
  }

  void abort() {
    if (state_ != ReservedPathState::Open)
      throw std::logic_error("path has already been consumed");
    for (const auto &entry : entries_)
      entry.prevalidate_token();
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend();
         ++iterator)
      iterator->abort_unchecked();
    state_ = ReservedPathState::Aborted;
  }

private:
  void abort_noexcept() noexcept {
    if (state_ != ReservedPathState::Open)
      return;
    for (auto iterator = entries_.rbegin(); iterator != entries_.rend();
         ++iterator)
      iterator->abort_noexcept();
    state_ = ReservedPathState::Aborted;
  }

  std::vector<SelectionReservation> entries_;
  ReservedPathState state_ = ReservedPathState::Open;
};

enum class ExpansionClaimKind : uint8_t {
  Owner = 0,
  Waiter = 1,
  Expanded = 2,
  Terminal = 3,
  Retry = 4,
};

struct ExpansionClaim {
  ExpansionClaimKind kind = ExpansionClaimKind::Retry;
  std::shared_ptr<PendingEvaluation> pending;
  Value cached_value{};
};

class ConcurrentTree {
public:
  explicit ConcurrentTree(uint64_t generation = 0,
                          TreeBackend backend = TreeBackend::Coarse,
                          size_t capacity = MAX_TREE_SIZE,
                          uint32_t shard_count = 64)
      : state_(std::make_shared<detail::ConcurrentTreeState>(
            generation, backend, capacity, shard_count)) {}

  TreeBackend backend() const noexcept { return state_->backend; }

  uint32_t shard_count() const noexcept {
    return static_cast<uint32_t>(state_->shards.size());
  }

  uint64_t generation() const noexcept {
    return state_->generation.load(std::memory_order_acquire);
  }

  size_t capacity() const noexcept { return state_->capacity; }

  void bind_evaluator(uint64_t evaluator_version) {
    std::lock_guard<std::mutex> lock(state_->metadata_mutex);
    if (state_->evaluator_bound &&
        state_->evaluator_version != evaluator_version)
      throw std::logic_error(
          "parallel tree cannot be reused with a different evaluator; clear "
          "the MCTS tree first");
    state_->evaluator_bound = true;
    state_->evaluator_version = evaluator_version;
  }

  size_t size() const noexcept {
    if (state_->backend == TreeBackend::Coarse) {
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
      return state_->coarse_nodes.size();
    }
    return state_->sharded_size.load(std::memory_order_acquire);
  }

  NodeHandle find(const TreeKey &key) const {
    CSPLENDOR_PERF_INC(ParallelTreeLookups);
    NodeHandle result;
    if (state_->backend == TreeBackend::Coarse) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      detail::LockTimer timer(
          csplendor::perf::Counter::ParallelShardLockAcquisitions,
          csplendor::perf::Counter::ParallelShardLockWaitNanoseconds,
          csplendor::perf::Counter::ParallelShardLockHoldNanoseconds);
#endif
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      timer.acquired();
#endif
      const auto iterator = state_->coarse_nodes.find(key);
      if (iterator != state_->coarse_nodes.end())
        result = iterator->second;
    } else {
      auto &shard = *state_->shards[detail::shard_index(*state_, key)];
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      detail::LockTimer timer(
          csplendor::perf::Counter::ParallelShardLockAcquisitions,
          csplendor::perf::Counter::ParallelShardLockWaitNanoseconds,
          csplendor::perf::Counter::ParallelShardLockHoldNanoseconds);
#endif
      std::lock_guard<std::mutex> lock(shard.mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      timer.acquired();
#endif
      const auto iterator = shard.nodes.find(key);
      if (iterator != shard.nodes.end())
        result = iterator->second;
    }
    if (result) {
      CSPLENDOR_PERF_INC(ParallelAccessEpochUpdates);
      result->last_access.store(
          state_->access_epoch.fetch_add(1, std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
    }
    return result;
  }

  NodeHandle find_or_create(const TreeKey &key) {
    CSPLENDOR_PERF_INC(ParallelTreeLookups);
    const uint64_t current_generation = generation();
    NodeHandle result;
    if (state_->backend == TreeBackend::Coarse) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      detail::LockTimer timer(
          csplendor::perf::Counter::ParallelShardLockAcquisitions,
          csplendor::perf::Counter::ParallelShardLockWaitNanoseconds,
          csplendor::perf::Counter::ParallelShardLockHoldNanoseconds);
#endif
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      timer.acquired();
#endif
      auto iterator = state_->coarse_nodes.find(key);
      if (iterator == state_->coarse_nodes.end()) {
        if (state_->coarse_nodes.size() >= state_->capacity)
          throw TreeCapacityReachedError("parallel MCTS tree capacity reached");
        result = std::make_shared<detail::NodeRecord>(key, current_generation,
                                                      state_);
        iterator = state_->coarse_nodes.emplace(key, result).first;
      }
      result = iterator->second;
    } else {
      auto &shard = *state_->shards[detail::shard_index(*state_, key)];
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      detail::LockTimer timer(
          csplendor::perf::Counter::ParallelShardLockAcquisitions,
          csplendor::perf::Counter::ParallelShardLockWaitNanoseconds,
          csplendor::perf::Counter::ParallelShardLockHoldNanoseconds);
#endif
      std::lock_guard<std::mutex> lock(shard.mutex);
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      timer.acquired();
#endif
      auto iterator = shard.nodes.find(key);
      if (iterator == shard.nodes.end()) {
        // Capacity is a hard upper bound. A small transient underfill is
        // preferable to overshooting it under concurrent insertions.
        size_t observed = state_->sharded_size.load(std::memory_order_relaxed);
        while (true) {
          if (observed >= state_->capacity)
            throw TreeCapacityReachedError(
                "parallel MCTS tree capacity reached");
          if (state_->sharded_size.compare_exchange_weak(
                  observed, observed + 1, std::memory_order_acq_rel,
                  std::memory_order_relaxed))
            break;
        }
        try {
          result = std::make_shared<detail::NodeRecord>(key, current_generation,
                                                        state_);
          iterator = shard.nodes.emplace(key, result).first;
        } catch (...) {
          state_->sharded_size.fetch_sub(1, std::memory_order_acq_rel);
          throw;
        }
      }
      result = iterator->second;
    }
    CSPLENDOR_PERF_INC(ParallelAccessEpochUpdates);
    result->last_access.store(
        state_->access_epoch.fetch_add(1, std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    return result;
  }

  void clear_and_set_generation(uint64_t new_generation) {
    if (state_->backend == TreeBackend::Coarse) {
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
      state_->coarse_nodes.clear();
      state_->generation.store(new_generation, std::memory_order_release);
    } else {
      for (auto &shard_ptr : state_->shards) {
        std::lock_guard<std::mutex> lock(shard_ptr->mutex);
        shard_ptr->nodes.clear();
      }
      state_->sharded_size.store(0, std::memory_order_release);
      state_->generation.store(new_generation, std::memory_order_release);
    }
    std::lock_guard<std::mutex> metadata_lock(state_->metadata_mutex);
    state_->evaluator_bound = false;
    state_->evaluator_version = 0;
  }

  void expand(const NodeHandle &node, const Policy &base_policy,
              const Value &value, const ActionMask &initial_mask) {
    expand_bits(node, base_policy, value,
                mcts_action_mask::from_dense(initial_mask));
  }

  void expand_bits(const NodeHandle &node, const Policy &base_policy,
                   const Value &value, ActionMaskBits initial_mask) {
    validate_policy(base_policy);
    if (!finite_value(value))
      throw std::invalid_argument("node value must be finite");
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      require_current(record);
      if (record.state == ExpansionState::Evaluating)
        throw std::logic_error("evaluation owner must publish via pending");
      if (record.state == ExpansionState::Terminal)
        throw std::logic_error("cannot expand a terminal node");
      if (record.state == ExpansionState::Expanded)
        throw std::logic_error("node has already been expanded");
      detail::ensure_edges(record, initial_mask);
      record.base_policy = base_policy;
      record.value = value;
      record.state = ExpansionState::Expanded;
    });
  }

  void set_terminal(const NodeHandle &node, const Value &value) {
    if (!finite_value(value))
      throw std::invalid_argument("terminal value must be finite");
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      require_current(record);
      if (record.state == ExpansionState::Evaluating)
        throw std::logic_error("cannot replace an evaluating node");
      if (record.state == ExpansionState::Expanded)
        throw std::logic_error("cannot replace an expanded node with terminal");
      if (record.state == ExpansionState::Terminal) {
        if (record.value != value)
          throw std::logic_error("terminal node value is inconsistent");
        return;
      }
      record.value = value;
      record.state = ExpansionState::Terminal;
    });
  }

  std::optional<SelectionReservation>
  select_and_reserve(const NodeHandle &node, const ActionMask &world_mask,
                     const SelectionContext &context, uint8_t player,
                     const std::shared_ptr<SearchLedger> &ledger) {
    return select_and_reserve_bits(node,
                                   mcts_action_mask::from_dense(world_mask),
                                   context, player, ledger);
  }

  std::optional<SelectionReservation>
  select_and_reserve_bits(const NodeHandle &node, ActionMaskBits world_mask,
                          const SelectionContext &context, uint8_t player,
                          const std::shared_ptr<SearchLedger> &ledger) {
    if (!node)
      throw std::invalid_argument("node handle is empty");
    if (player >= NUM_PLAYERS)
      throw std::invalid_argument("reservation player is out of range");
    if (context.tree_generation != generation())
      throw std::logic_error("selection uses a stale tree generation");
    world_mask &= mcts_action_mask::ALL;
    uint64_t reservation_id = 0;
    int selected_action = -1;

    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      require_current(record);
      if (record.state == ExpansionState::Terminal)
        return;
      if (record.state != ExpansionState::Expanded)
        throw std::logic_error("cannot select from an unexpanded node");

      constexpr uint64_t counter_max = std::numeric_limits<uint64_t>::max();
      double policy_sum = 0.0;
      const size_t candidate_count = mcts_action_mask::popcount(world_mask);
      mcts_action_mask::for_each(world_mask, [&](size_t action) {
        // Validate every candidate before any availability/union mutation.
        const auto edge = detail::find_edge(record, action);
        if (edge != record.edges.end() && edge->action == action &&
            edge->availability_count == counter_max)
          throw std::overflow_error(
              "parallel action availability counter is exhausted");
        const float prior = record.base_policy[action];
        if (std::isfinite(prior) && prior > 0.0f)
          policy_sum += static_cast<double>(prior);
      });
      if (candidate_count == 0)
        return;

      constexpr double virtual_loss_weight = 0.3;
      double total_virtual_loss = 0.0;
      for (const auto &edge : record.edges)
        total_virtual_loss +=
            static_cast<double>(edge.virtual_loss) * virtual_loss_weight;
      const double sqrt_total =
          std::sqrt(static_cast<double>(record.total_visits) +
                    total_virtual_loss + static_cast<double>(EPS));
      const double fpu = context.fpu == 0.0 ? 0.0 : -std::abs(context.fpu);
      double best_score = -std::numeric_limits<double>::infinity();
      bool forced_choice = false;

      mcts_action_mask::for_each(world_mask, [&](size_t action) {
        if (forced_choice)
          return;
        const auto edge = detail::find_edge(record, action);
        const bool has_edge =
            edge != record.edges.end() && edge->action == action;
        const uint64_t visits = has_edge ? edge->N : 0;
        const uint64_t virtual_loss = has_edge ? edge->virtual_loss : 0;
        const uint64_t availability = has_edge ? edge->availability_count : 0;
        const double raw_prior =
            std::isfinite(record.base_policy[action]) &&
                    record.base_policy[action] > 0.0f
                ? static_cast<double>(record.base_policy[action])
                : 0.0;
        double prior = policy_sum > static_cast<double>(EPS)
                           ? raw_prior / policy_sum
                           : 1.0 / static_cast<double>(candidate_count);
        if (context.is_root && context.root_noise) {
          prior = (1.0 - context.dirichlet_epsilon) * prior +
                  context.dirichlet_epsilon *
                      static_cast<double>((*context.root_noise)[action]);
        }

        if (context.forced_playouts) {
          const uint64_t opportunity = availability + 1;
          const long double radicand = std::max<long double>(
              0.0L, static_cast<long double>(context.forced_playouts_k) *
                        static_cast<long double>(prior) *
                        static_cast<long double>(opportunity));
          const long double threshold_value = std::sqrt(radicand);
          const uint64_t threshold =
              threshold_value >= static_cast<long double>(
                                     std::numeric_limits<uint64_t>::max())
                  ? std::numeric_limits<uint64_t>::max()
                  : static_cast<uint64_t>(threshold_value);
          if (visits < threshold && virtual_loss < threshold - visits) {
            selected_action = static_cast<int>(action);
            forced_choice = true;
            return;
          }
        }

        const double live = static_cast<double>(virtual_loss);
        const double effective_n =
            static_cast<double>(visits) + live * virtual_loss_weight;
        double q = fpu;
        if (visits > 0) {
          const double penalty = live * virtual_loss_weight * 0.5;
          q = (edge->Q * static_cast<double>(visits) - penalty) /
              (static_cast<double>(visits) + penalty);
        } else if (virtual_loss > 0) {
          q = fpu - 0.2;
        }
        const double score =
            q + context.cpuct * prior * sqrt_total / (1.0 + effective_n);
        if (score > best_score) {
          best_score = score;
          selected_action = static_cast<int>(action);
        }
      });

      if (selected_action >= 0) {
        const size_t action = static_cast<size_t>(selected_action);
        const auto selected_edge = detail::find_edge(record, action);
        const uint64_t selected_virtual_loss =
            selected_edge != record.edges.end() &&
                    selected_edge->action == action
                ? selected_edge->virtual_loss
                : 0;
        const uint64_t selected_visits = selected_edge != record.edges.end() &&
                                                 selected_edge->action == action
                                             ? selected_edge->N
                                             : 0;
        if (selected_virtual_loss == counter_max)
          throw std::overflow_error(
              "parallel action virtual loss counter is exhausted");
        if (selected_visits >= counter_max - selected_virtual_loss)
          throw std::overflow_error(
              "parallel action visit counter is exhausted");
        const uint64_t remaining_visits = counter_max - record.total_visits;
        if (record.live_reservations.size() >= remaining_visits)
          throw std::overflow_error("parallel node visit counter is exhausted");
        CSPLENDOR_PERF_RESERVATION_OCCUPANCY(record.live_reservations.size());

        const ActionMaskBits missing =
            world_mask & ~record.information_set_union;
        record.edges.reserve(record.edges.size() +
                             mcts_action_mask::popcount(missing));
        reservation_id = detail::claim_monotonic_id(
            state_->next_reservation_id,
            "parallel reservation identifier is exhausted");
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
        const size_t reservation_buckets_before =
            record.live_reservations.bucket_count();
#endif
        const auto inserted =
            record.live_reservations.insert(reservation_id).second;
        if (!inserted)
          throw std::logic_error("duplicate reservation identifier");
        CSPLENDOR_PERF_LIVE_RESERVATION_INSERT(
            reservation_buckets_before,
            record.live_reservations.bucket_count());
      }

      detail::ensure_edges(record, world_mask);
      for (auto &edge : record.edges) {
        if (mcts_action_mask::contains(world_mask, edge.action))
          ++edge.availability_count;
      }
      if (selected_action >= 0) {
        auto edge =
            detail::find_edge(record, static_cast<size_t>(selected_action));
        ++edge->virtual_loss;
      }
    });

    if (selected_action < 0)
      return std::nullopt;
    state_->live_reservation_count.fetch_add(1, std::memory_order_relaxed);
    state_->virtual_loss_count.fetch_add(1, std::memory_order_relaxed);
    if (ledger) {
      CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerSelectionAtomicIncrements, 1);
      ledger->selected.fetch_add(1, std::memory_order_relaxed);
      CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerReservationAtomicIncrements, 1);
      ledger->virtual_loss_added.fetch_add(1, std::memory_order_relaxed);
    }
    return SelectionReservation(state_, node, selected_action, player,
                                reservation_id, ledger);
  }

  ExpansionClaim
  claim_or_attach(const NodeHandle &node, uint64_t ticket_id,
                  uint64_t feature_digest,
                  const std::array<float, FEATURE_SIZE> &features,
                  const ActionMask &owner_world_mask,
                  const std::shared_ptr<SearchLedger> &ledger) {
    return claim_or_attach_bits(node, ticket_id, feature_digest, features,
                                mcts_action_mask::from_dense(owner_world_mask),
                                ledger);
  }

  ExpansionClaim
  claim_or_attach_bits(const NodeHandle &node, uint64_t ticket_id,
                       uint64_t feature_digest,
                       const std::array<float, FEATURE_SIZE> &features,
                       ActionMaskBits owner_world_mask,
                       const std::shared_ptr<SearchLedger> &ledger) {
    if (!node)
      throw std::invalid_argument("node handle is empty");
    owner_world_mask &= mcts_action_mask::ALL;

    ExpansionClaim result;
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      require_current(record);
      switch (record.state) {
      case ExpansionState::Unexpanded: {
        auto pending = std::make_shared<PendingEvaluation>();
        pending->id = detail::claim_monotonic_id(
            state_->next_pending_id,
            "parallel pending identifier is exhausted");
        pending->key = record.key;
        pending->tree_generation = generation();
        pending->node_generation = record.generation;
        pending->feature_digest = feature_digest;
        pending->attached_ticket_ids.push_back(ticket_id);
        pending->request.pending_id = pending->id;
        pending->request.owner_simulation_id = ticket_id;
        pending->request.key = record.key;
        pending->request.features = features;
        pending->request.owner_world_mask =
            mcts_action_mask::to_dense(owner_world_mask);
        record.pending = pending;
        record.state = ExpansionState::Evaluating;
        result.kind = ExpansionClaimKind::Owner;
        result.pending = std::move(pending);
        break;
      }
      case ExpansionState::Evaluating:
        result.pending = record.pending.lock();
        result.kind = result.pending ? ExpansionClaimKind::Waiter
                                     : ExpansionClaimKind::Retry;
        break;
      case ExpansionState::Expanded:
        result.kind = ExpansionClaimKind::Expanded;
        result.cached_value = record.value;
        break;
      case ExpansionState::Terminal:
        result.kind = ExpansionClaimKind::Terminal;
        result.cached_value = record.value;
        break;
      }
    });

    if (result.kind == ExpansionClaimKind::Owner) {
      state_->evaluating_node_count.fetch_add(1, std::memory_order_relaxed);
      state_->pending_evaluation_count.fetch_add(1, std::memory_order_relaxed);
      if (ledger) {
        CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerEvaluationAtomicIncrements, 2);
        ledger->evaluation_owner.fetch_add(1, std::memory_order_relaxed);
        ledger->expansion_claimed.fetch_add(1, std::memory_order_relaxed);
      }
      return result;
    }
    if (result.kind != ExpansionClaimKind::Waiter)
      return result;

    std::lock_guard<std::mutex> pending_lock(result.pending->mutex);
    if (result.pending->state != PendingState::Open) {
      result.kind = ExpansionClaimKind::Retry;
      result.pending.reset();
      return result;
    }
    if (result.pending->feature_digest != feature_digest ||
        result.pending->request.features != features)
      throw std::logic_error(
          "equal TreeKey produced different public features");
    if (std::find(result.pending->attached_ticket_ids.begin(),
                  result.pending->attached_ticket_ids.end(),
                  ticket_id) != result.pending->attached_ticket_ids.end())
      throw std::logic_error("ticket is already attached to evaluation");
    // Different world masks are expected inside one information set. They are
    // deliberately ticket-local and are not a deduplication discriminator.
    result.pending->attached_ticket_ids.push_back(ticket_id);
    if (ledger) {
      CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerEvaluationAtomicIncrements, 2);
      ledger->evaluation_waiter.fetch_add(1, std::memory_order_relaxed);
      ledger->expansion_waited.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
  }

  std::vector<uint64_t>
  publish_evaluation(const NodeHandle &node,
                     const std::shared_ptr<PendingEvaluation> &pending,
                     const Policy &base_policy, const Value &value,
                     const std::shared_ptr<SearchLedger> &ledger) {
    validate_policy(base_policy);
    if (!finite_value(value))
      throw std::invalid_argument("inference value must be finite");
    begin_pending_close(pending);

    try {
      detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
        require_pending(record, pending);
        detail::ensure_edges(record, mcts_action_mask::from_dense(
                                         pending->request.owner_world_mask));
        record.base_policy = base_policy;
        record.value = value;
        record.pending.reset();
        record.state = ExpansionState::Expanded;
      });
    } catch (...) {
      finish_pending_failure(node, pending, PendingState::Failed);
      throw;
    }

    detail::decrement_nonzero(state_->evaluating_node_count);
    auto tickets = finish_pending(pending, PendingState::Published);
    detail::decrement_nonzero(state_->pending_evaluation_count);
    if (ledger) {
      CSPLENDOR_PERF_LEDGER_ADD(ParallelLedgerEvaluationAtomicIncrements, 3);
      ledger->expansion_published.fetch_add(1, std::memory_order_relaxed);
      ledger->evaluation_requested.fetch_add(1, std::memory_order_relaxed);
      ledger->evaluated_boards.fetch_add(1, std::memory_order_relaxed);
    }
    return tickets;
  }

  std::vector<uint64_t>
  fail_evaluation(const NodeHandle &node,
                  const std::shared_ptr<PendingEvaluation> &pending,
                  PendingState terminal_state = PendingState::Failed) {
    if (terminal_state != PendingState::Failed &&
        terminal_state != PendingState::Cancelled)
      throw std::invalid_argument("pending failure state is not terminal");
    begin_pending_close(pending);
    return finish_pending_failure(node, pending, terminal_state);
  }

  MCTSNodeSnapshot64 snapshot(const NodeHandle &node) const {
    if (!node)
      throw std::invalid_argument("node handle is empty");
    return detail::with_node_lock(state_, node,
                                  [&](const detail::NodeRecord &record) {
                                    return make_snapshot(record);
                                  });
  }

  NodeStateView state_view(const NodeHandle &node) const {
    if (!node)
      throw std::invalid_argument("node handle is empty");
    return detail::with_node_lock(
        state_, node, [&](const detail::NodeRecord &record) {
          return NodeStateView{record.value, record.state};
        });
  }

  std::optional<MCTSNodeSnapshot64> snapshot(const TreeKey &key) const {
    auto node = find(key);
    if (!node)
      return std::nullopt;
    return snapshot(node);
  }

  std::vector<MCTSNodeSnapshot64> snapshot_all() const {
    std::vector<NodeHandle> handles;
    if (state_->backend == TreeBackend::Coarse) {
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
      handles.reserve(state_->coarse_nodes.size());
      for (const auto &entry : state_->coarse_nodes)
        handles.push_back(entry.second);
      // The coarse mutex also protects node data, so copying while holding it
      // avoids recursively taking the same mutex through snapshot().
      std::vector<MCTSNodeSnapshot64> result;
      result.reserve(handles.size());
      for (const auto &node : handles)
        result.push_back(make_snapshot(*node));
      return result;
    }

    for (const auto &shard_ptr : state_->shards) {
      std::lock_guard<std::mutex> lock(shard_ptr->mutex);
      for (const auto &entry : shard_ptr->nodes)
        handles.push_back(entry.second);
    }
    std::vector<MCTSNodeSnapshot64> result;
    result.reserve(handles.size());
    for (const auto &handle : handles)
      result.push_back(snapshot(handle));
    return result;
  }

  void validate_quiescent_fast() const {
    if (state_->virtual_loss_count.load(std::memory_order_acquire) != 0)
      throw std::logic_error("parallel tree retains virtual loss");
    if (state_->live_reservation_count.load(std::memory_order_acquire) != 0)
      throw std::logic_error("parallel tree retains reservation tokens");
    if (state_->evaluating_node_count.load(std::memory_order_acquire) != 0)
      throw std::logic_error("parallel tree retains evaluating node");
    if (state_->pending_evaluation_count.load(std::memory_order_acquire) != 0)
      throw std::logic_error("parallel tree retains pending evaluation");
  }

  void validate_quiescent_full() const {
    for (const auto &node : snapshot_all()) {
      const uint64_t total = std::accumulate(node.stats.N.begin(),
                                             node.stats.N.end(), uint64_t{0});
      if (total != node.stats.total_visits)
        throw std::logic_error("node total_visits differs from sum(N)");
      const uint64_t virtual_loss =
          std::accumulate(node.stats.virtual_loss.begin(),
                          node.stats.virtual_loss.end(), uint64_t{0});
      if (virtual_loss != 0)
        throw std::logic_error("parallel tree retains virtual loss");
      if (node.live_reservation_count != 0)
        throw std::logic_error("parallel tree retains reservation tokens");
      if (node.state == ExpansionState::Evaluating)
        throw std::logic_error("parallel tree retains evaluating node");
      if (node.has_pending_evaluation)
        throw std::logic_error("parallel tree retains pending evaluation");
      if (!finite_value(node.stats.value))
        throw std::logic_error("parallel tree contains non-finite value");
      for (size_t action = 0; action < MAX_ACTIONS; ++action) {
        if (!std::isfinite(node.base_policy[action]) ||
            node.base_policy[action] < 0.0f ||
            !std::isfinite(node.stats.Q[action]))
          throw std::logic_error("parallel tree contains non-finite stats");
        if (node.valid_actions[action] > 1)
          throw std::logic_error("parallel tree contains invalid action mask");
        if (node.availability_count[action] > 0 &&
            node.valid_actions[action] == 0)
          throw std::logic_error(
              "parallel availability is outside its information-set union");
      }
    }
    // Also catches outstanding handles removed from the current generation's
    // node map, which a snapshot-only audit cannot see.
    validate_quiescent_fast();
  }

  void validate_quiescent() const { validate_quiescent_full(); }

  void validate_quiescent_for_search() const {
#if !defined(NDEBUG) || defined(CSPLENDOR_MCTS_FULL_AUDIT)
    validate_quiescent_full();
#else
    validate_quiescent_fast();
#endif
  }

#ifdef CSPLENDOR_MCTS_TESTING
  size_t compact_edge_count_for_testing(const NodeHandle &node) const {
    return detail::with_node_lock(
        state_, node,
        [](const detail::NodeRecord &record) { return record.edges.size(); });
  }

  void set_stats_for_testing(const NodeHandle &node, size_t action,
                             uint64_t visits, double q) {
    if (action >= MAX_ACTIONS || !std::isfinite(q))
      throw std::invalid_argument("invalid test statistics");
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      auto &edge = detail::ensure_edge(record, action);
      const uint64_t old = edge.N;
      edge.N = visits;
      record.total_visits = record.total_visits - old + visits;
      edge.Q = q;
    });
  }

  void set_selection_counters_for_testing(const NodeHandle &node, size_t action,
                                          uint64_t visits,
                                          uint64_t total_visits,
                                          uint64_t virtual_loss,
                                          uint64_t availability_count) {
    if (action >= MAX_ACTIONS)
      throw std::invalid_argument("invalid test counter action");
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      auto &edge = detail::ensure_edge(record, action);
      const uint64_t old_virtual_loss = edge.virtual_loss;
      edge.N = visits;
      record.total_visits = total_visits;
      edge.virtual_loss = virtual_loss;
      edge.availability_count = availability_count;
      if (virtual_loss > old_virtual_loss) {
        state_->virtual_loss_count.fetch_add(virtual_loss - old_virtual_loss,
                                             std::memory_order_relaxed);
      } else if (old_virtual_loss > virtual_loss) {
        state_->virtual_loss_count.fetch_sub(old_virtual_loss - virtual_loss,
                                             std::memory_order_relaxed);
      }
    });
  }
#endif

private:
  static MCTSNodeSnapshot64 make_snapshot(const detail::NodeRecord &record) {
    MCTSNodeSnapshot64 out;
    out.key = record.key;
    out.generation = record.generation;
    out.valid_actions =
        mcts_action_mask::to_dense(record.information_set_union);
    out.base_policy = record.base_policy;
    out.stats.value = record.value;
    out.stats.total_visits = record.total_visits;
    for (const auto &edge : record.edges) {
      const size_t action = edge.action;
      out.stats.N[action] = edge.N;
      out.stats.virtual_loss[action] = edge.virtual_loss;
      out.stats.Q[action] = edge.Q;
      out.availability_count[action] = edge.availability_count;
    }
    out.live_reservation_count = record.live_reservations.size();
    out.has_pending_evaluation = !record.pending.expired();
    out.state = record.state;
    return out;
  }

  void require_current(const detail::NodeRecord &record) const {
    if (record.generation != generation())
      throw std::logic_error("node belongs to a stale tree generation");
  }

  void
  require_pending(const detail::NodeRecord &record,
                  const std::shared_ptr<PendingEvaluation> &pending) const {
    require_current(record);
    if (!pending || record.state != ExpansionState::Evaluating)
      throw std::logic_error("node is not awaiting this evaluation");
    const auto registered = record.pending.lock();
    if (!registered || registered.get() != pending.get() ||
        pending->tree_generation != generation() ||
        pending->node_generation != record.generation)
      throw std::logic_error("stale pending evaluation");
  }

  static void
  begin_pending_close(const std::shared_ptr<PendingEvaluation> &pending) {
    if (!pending)
      throw std::invalid_argument("pending evaluation is empty");
    std::lock_guard<std::mutex> lock(pending->mutex);
    if (pending->state != PendingState::Open)
      throw std::logic_error("pending evaluation has already been consumed");
    pending->state = PendingState::Closing;
  }

  static std::vector<uint64_t>
  finish_pending(const std::shared_ptr<PendingEvaluation> &pending,
                 PendingState state) {
    std::lock_guard<std::mutex> lock(pending->mutex);
    if (pending->state != PendingState::Closing)
      throw std::logic_error("pending evaluation is not closing");
    std::vector<uint64_t> tickets;
    tickets.swap(pending->attached_ticket_ids);
    pending->state = state;
    return tickets;
  }

  std::vector<uint64_t>
  finish_pending_failure(const NodeHandle &node,
                         const std::shared_ptr<PendingEvaluation> &pending,
                         PendingState state) {
    bool released_evaluating = false;
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      const auto registered = record.pending.lock();
      if (record.state == ExpansionState::Evaluating && registered &&
          registered.get() == pending.get()) {
        record.pending.reset();
        record.state = ExpansionState::Unexpanded;
        released_evaluating = true;
      }
    });
    if (released_evaluating) {
      detail::decrement_nonzero(state_->evaluating_node_count);
    }
    auto tickets = finish_pending(pending, state);
    if (released_evaluating)
      detail::decrement_nonzero(state_->pending_evaluation_count);
    return tickets;
  }

  std::shared_ptr<detail::ConcurrentTreeState> state_;
};

inline MCTSNode to_legacy_node(const MCTSNodeSnapshot64 &snapshot) {
  MCTSNode result;
  if (snapshot.stats.total_visits >
      static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    throw std::overflow_error("parallel total_visits exceeds legacy range");
  result.total_visits = static_cast<uint32_t>(snapshot.stats.total_visits);
  result.valid_actions = snapshot.valid_actions;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (snapshot.stats.N[action] >
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
      throw std::overflow_error("parallel visit count exceeds legacy range");
    if (snapshot.stats.virtual_loss[action] >
        static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
      throw std::overflow_error("parallel virtual loss exceeds legacy range");
    if (!std::isfinite(snapshot.stats.Q[action]) ||
        std::abs(snapshot.stats.Q[action]) >
            static_cast<double>(std::numeric_limits<float>::max()))
      throw std::overflow_error("parallel Q value exceeds legacy range");
    result.prior[action] = snapshot.base_policy[action];
    result.Q[action] = static_cast<float>(snapshot.stats.Q[action]);
    result.N[action] = static_cast<uint32_t>(snapshot.stats.N[action]);
    result.virtual_loss[action] =
        static_cast<int32_t>(snapshot.stats.virtual_loss[action]);
  }
  for (size_t player = 0; player < NUM_PLAYERS; ++player) {
    if (!std::isfinite(snapshot.stats.value[player]) ||
        std::abs(snapshot.stats.value[player]) >
            static_cast<double>(std::numeric_limits<float>::max()))
      throw std::overflow_error("parallel node value exceeds legacy range");
    result.value[player] = static_cast<float>(snapshot.stats.value[player]);
  }
  result.is_terminal = snapshot.state == ExpansionState::Terminal;
  result.is_expanded = snapshot.state == ExpansionState::Expanded ||
                       snapshot.state == ExpansionState::Terminal;
  return result;
}

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_CONCURRENT_TREE_H
