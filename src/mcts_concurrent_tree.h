#ifndef CSPLENDOR_MCTS_CONCURRENT_TREE_H
#define CSPLENDOR_MCTS_CONCURRENT_TREE_H

#include "mcts_parallel_types.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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

namespace mcts_parallel {

struct PendingEvaluation;

namespace detail {

struct ConcurrentTreeState;

struct NodeRecord {
  NodeRecord(const TreeKey &node_key, uint64_t node_generation,
             const std::shared_ptr<ConcurrentTreeState> &owner_state)
      : key(node_key), generation(node_generation), owner(owner_state) {}

  const TreeKey key;
  mutable std::mutex mutex;
  NodeStats64 stats{};
  Policy base_policy{};
  ActionMask information_set_union{};
  std::array<uint64_t, MAX_ACTIONS> availability_count{};
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
    std::lock_guard<std::mutex> lock(tree->coarse_mutex);
    return std::forward<Function>(function)(*node);
  }
  std::lock_guard<std::mutex> lock(node->mutex);
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
      if (ledger_)
        ledger_->stale_result.fetch_add(1, std::memory_order_relaxed);
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
          return record.generation == node_generation_ &&
                 record.live_reservations.count(reservation_id_) == 1 &&
                 record.stats.virtual_loss[static_cast<size_t>(action_)] > 0;
        });
    if (!valid)
      throw std::logic_error("reservation token is not live");
  }

  void commit_unchecked(double value) noexcept {
    detail::with_node_lock(tree_, node_, [&](detail::NodeRecord &record) {
      const size_t action_index = static_cast<size_t>(action_);
      record.live_reservations.erase(reservation_id_);
      --record.stats.virtual_loss[action_index];
      ++record.stats.N[action_index];
      ++record.stats.total_visits;
      const double count = static_cast<double>(record.stats.N[action_index]);
      record.stats.Q[action_index] +=
          (value - record.stats.Q[action_index]) / count;
    });
    state_ = ReservationState::Committed;
    if (ledger_) {
      ledger_->virtual_loss_released.fetch_add(1, std::memory_order_relaxed);
      ledger_->reservations_committed.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void abort_unchecked() noexcept {
    bool released = false;
    detail::with_node_lock(tree_, node_, [&](detail::NodeRecord &record) {
      const size_t action_index = static_cast<size_t>(action_);
      const auto erased = record.live_reservations.erase(reservation_id_);
      if (erased == 1 && record.stats.virtual_loss[action_index] > 0) {
        --record.stats.virtual_loss[action_index];
        released = true;
      }
    });
    state_ = ReservationState::Aborted;
    if (ledger_) {
      if (released) {
        ledger_->virtual_loss_released.fetch_add(1, std::memory_order_relaxed);
        ledger_->reservations_aborted.fetch_add(1, std::memory_order_relaxed);
      } else {
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
    NodeHandle result;
    if (state_->backend == TreeBackend::Coarse) {
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
      const auto iterator = state_->coarse_nodes.find(key);
      if (iterator != state_->coarse_nodes.end())
        result = iterator->second;
    } else {
      auto &shard = *state_->shards[detail::shard_index(*state_, key)];
      std::lock_guard<std::mutex> lock(shard.mutex);
      const auto iterator = shard.nodes.find(key);
      if (iterator != shard.nodes.end())
        result = iterator->second;
    }
    if (result)
      result->last_access.store(
          state_->access_epoch.fetch_add(1, std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
    return result;
  }

  NodeHandle find_or_create(const TreeKey &key) {
    const uint64_t current_generation = generation();
    NodeHandle result;
    if (state_->backend == TreeBackend::Coarse) {
      std::lock_guard<std::mutex> lock(state_->coarse_mutex);
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
      std::lock_guard<std::mutex> lock(shard.mutex);
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
      record.base_policy = base_policy;
      record.stats.value = value;
      for (size_t action = 0; action < MAX_ACTIONS; ++action)
        record.information_set_union[action] |= initial_mask[action];
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
        if (record.stats.value != value)
          throw std::logic_error("terminal node value is inconsistent");
        return;
      }
      record.stats.value = value;
      record.state = ExpansionState::Terminal;
    });
  }

  std::optional<SelectionReservation>
  select_and_reserve(const NodeHandle &node, const ActionMask &world_mask,
                     const SelectionContext &context, uint8_t player,
                     const std::shared_ptr<SearchLedger> &ledger) {
    if (!node)
      throw std::invalid_argument("node handle is empty");
    if (player >= NUM_PLAYERS)
      throw std::invalid_argument("reservation player is out of range");
    if (context.tree_generation != generation())
      throw std::logic_error("selection uses a stale tree generation");
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
      size_t candidate_count = 0;
      for (size_t action = 0; action < MAX_ACTIONS; ++action) {
        if (!world_mask[action])
          continue;
        // Validate every candidate before any availability/union mutation.
        // Folding the preflight into the policy scan keeps the hot selection
        // path at the same number of full action-array passes.
        if (record.availability_count[action] == counter_max)
          throw std::overflow_error(
              "parallel action availability counter is exhausted");
        ++candidate_count;
        const float prior = record.base_policy[action];
        if (std::isfinite(prior) && prior > 0.0f)
          policy_sum += static_cast<double>(prior);
      }
      if (candidate_count == 0)
        return;

      constexpr double virtual_loss_weight = 0.3;
      double total_virtual_loss = 0.0;
      for (uint64_t count : record.stats.virtual_loss)
        total_virtual_loss += static_cast<double>(count) * virtual_loss_weight;
      const double sqrt_total =
          std::sqrt(static_cast<double>(record.stats.total_visits) +
                    total_virtual_loss + static_cast<double>(EPS));
      const double fpu = context.fpu == 0.0 ? 0.0 : -std::abs(context.fpu);
      double best_score = -std::numeric_limits<double>::infinity();

      for (size_t action = 0; action < MAX_ACTIONS; ++action) {
        if (!world_mask[action])
          continue;
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
          const uint64_t opportunity = record.availability_count[action] + 1;
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
          const uint64_t visits = record.stats.N[action];
          const uint64_t live = record.stats.virtual_loss[action];
          if (visits < threshold && live < threshold - visits) {
            selected_action = static_cast<int>(action);
            break;
          }
        }

        const double live =
            static_cast<double>(record.stats.virtual_loss[action]);
        const double effective_n = static_cast<double>(record.stats.N[action]) +
                                   live * virtual_loss_weight;
        double q = fpu;
        if (record.stats.N[action] > 0) {
          const double penalty = live * virtual_loss_weight * 0.5;
          q = (record.stats.Q[action] *
                   static_cast<double>(record.stats.N[action]) -
               penalty) /
              (static_cast<double>(record.stats.N[action]) + penalty);
        } else if (record.stats.virtual_loss[action] > 0) {
          q = fpu - 0.2;
        }
        const double score =
            q + context.cpuct * prior * sqrt_total / (1.0 + effective_n);
        if (score > best_score) {
          best_score = score;
          selected_action = static_cast<int>(action);
        }
      }

      if (selected_action >= 0) {
        const size_t action = static_cast<size_t>(selected_action);
        if (record.stats.virtual_loss[action] == counter_max)
          throw std::overflow_error(
              "parallel action virtual loss counter is exhausted");
        if (record.stats.N[action] >=
            counter_max - record.stats.virtual_loss[action])
          throw std::overflow_error(
              "parallel action visit counter is exhausted");
        const uint64_t remaining_visits =
            counter_max - record.stats.total_visits;
        if (record.live_reservations.size() >= remaining_visits)
          throw std::overflow_error("parallel node visit counter is exhausted");

        reservation_id = detail::claim_monotonic_id(
            state_->next_reservation_id,
            "parallel reservation identifier is exhausted");
        const auto inserted =
            record.live_reservations.insert(reservation_id).second;
        if (!inserted)
          throw std::logic_error("duplicate reservation identifier");
      }

      for (size_t action = 0; action < MAX_ACTIONS; ++action) {
        if (!world_mask[action])
          continue;
        record.information_set_union[action] = 1;
        ++record.availability_count[action];
      }
      if (selected_action >= 0)
        ++record.stats.virtual_loss[static_cast<size_t>(selected_action)];
    });

    if (selected_action < 0)
      return std::nullopt;
    if (ledger) {
      ledger->selected.fetch_add(1, std::memory_order_relaxed);
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
    if (!node)
      throw std::invalid_argument("node handle is empty");

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
        pending->request.owner_world_mask = owner_world_mask;
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
        result.cached_value = record.stats.value;
        break;
      case ExpansionState::Terminal:
        result.kind = ExpansionClaimKind::Terminal;
        result.cached_value = record.stats.value;
        break;
      }
    });

    if (result.kind == ExpansionClaimKind::Owner) {
      if (ledger) {
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
        record.base_policy = base_policy;
        record.stats.value = value;
        for (size_t action = 0; action < MAX_ACTIONS; ++action)
          record.information_set_union[action] |=
              pending->request.owner_world_mask[action];
        record.pending.reset();
        record.state = ExpansionState::Expanded;
      });
    } catch (...) {
      finish_pending_failure(node, pending, PendingState::Failed);
      throw;
    }

    auto tickets = finish_pending(pending, PendingState::Published);
    if (ledger) {
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
    return detail::with_node_lock(
        state_, node, [&](const detail::NodeRecord &record) {
          MCTSNodeSnapshot64 out;
          out.key = record.key;
          out.generation = record.generation;
          out.valid_actions = record.information_set_union;
          out.base_policy = record.base_policy;
          out.stats = record.stats;
          out.availability_count = record.availability_count;
          out.live_reservation_count = record.live_reservations.size();
          out.has_pending_evaluation = !record.pending.expired();
          out.state = record.state;
          return out;
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
      for (const auto &node : handles) {
        MCTSNodeSnapshot64 out;
        out.key = node->key;
        out.generation = node->generation;
        out.valid_actions = node->information_set_union;
        out.base_policy = node->base_policy;
        out.stats = node->stats;
        out.availability_count = node->availability_count;
        out.live_reservation_count = node->live_reservations.size();
        out.has_pending_evaluation = !node->pending.expired();
        out.state = node->state;
        result.push_back(out);
      }
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

  void validate_quiescent() const {
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
  }

#ifdef CSPLENDOR_MCTS_TESTING
  void set_stats_for_testing(const NodeHandle &node, size_t action,
                             uint64_t visits, double q) {
    if (action >= MAX_ACTIONS || !std::isfinite(q))
      throw std::invalid_argument("invalid test statistics");
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      const uint64_t old = record.stats.N[action];
      record.stats.N[action] = visits;
      record.stats.total_visits = record.stats.total_visits - old + visits;
      record.stats.Q[action] = q;
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
      record.stats.N[action] = visits;
      record.stats.total_visits = total_visits;
      record.stats.virtual_loss[action] = virtual_loss;
      record.availability_count[action] = availability_count;
    });
  }
#endif

private:
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
    detail::with_node_lock(state_, node, [&](detail::NodeRecord &record) {
      const auto registered = record.pending.lock();
      if (record.state == ExpansionState::Evaluating && registered &&
          registered.get() == pending.get()) {
        record.pending.reset();
        record.state = ExpansionState::Unexpanded;
      }
    });
    return finish_pending(pending, state);
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
