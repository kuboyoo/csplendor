#ifndef CSPLENDOR_MCTS_PARALLEL_TRACE_H
#define CSPLENDOR_MCTS_PARALLEL_TRACE_H

#include "mcts_concurrent_tree.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mcts_parallel {

// Version 3 stores only the nodes changed by each committed simulation.  The
// v2 full-tree-per-event representation grew quadratically and could make a
// successful search fail while recording its post-commit snapshot.
static constexpr uint32_t MCTS_PARALLEL_TRACE_VERSION = 3;
static constexpr uint64_t MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS = 131072;
static constexpr uint64_t MCTS_PARALLEL_TRACE_MAX_EVENTS =
    (MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS - 1) /
    (static_cast<uint64_t>(MAX_DEPTH) * 2 + 1);

namespace trace_detail {

inline void hash_byte(uint64_t &hash, uint8_t value) noexcept {
  hash ^= value;
  hash *= 1099511628211ULL;
}

template <typename Integer>
inline void hash_integer(uint64_t &hash, Integer value) noexcept {
  using Unsigned = typename std::make_unsigned<Integer>::type;
  Unsigned bits = static_cast<Unsigned>(value);
  for (size_t index = 0; index < sizeof(bits); ++index)
    hash_byte(hash, static_cast<uint8_t>(bits >> (index * 8)));
}

inline bool key_less(const TreeKey &left, const TreeKey &right) noexcept {
  if (left.position_hash != right.position_hash)
    return left.position_hash < right.position_hash;
  if (left.key_version != right.key_version)
    return left.key_version < right.key_version;
  if (left.observer != right.observer)
    return left.observer < right.observer;
  if (left.domain != right.domain)
    return static_cast<uint8_t>(left.domain) <
           static_cast<uint8_t>(right.domain);
  return left.mode_bits < right.mode_bits;
}

inline uint64_t
snapshot_digest(std::vector<MCTSNodeSnapshot64> snapshots) noexcept {
  std::sort(
      snapshots.begin(), snapshots.end(),
      [](const MCTSNodeSnapshot64 &left, const MCTSNodeSnapshot64 &right) {
        return key_less(left.key, right.key);
      });
  uint64_t hash = 1469598103934665603ULL;
  hash_integer(hash, static_cast<uint64_t>(snapshots.size()));
  for (const auto &node : snapshots) {
    hash_integer(hash, node.key.position_hash);
    hash_integer(hash, node.key.key_version);
    hash_integer(hash, node.key.observer);
    hash_integer(hash, static_cast<uint8_t>(node.key.domain));
    hash_integer(hash, node.key.mode_bits);
    hash_integer(hash, node.generation);
    hash_integer(hash, static_cast<uint8_t>(node.state));
    for (size_t action = 0; action < MAX_ACTIONS; ++action) {
      hash_integer(hash, node.valid_actions[action]);
      uint32_t prior_bits = 0;
      std::memcpy(&prior_bits, &node.base_policy[action], sizeof(prior_bits));
      hash_integer(hash, prior_bits);
      hash_integer(hash, node.stats.N[action]);
      hash_integer(hash, node.stats.virtual_loss[action]);
      uint64_t q_bits = 0;
      std::memcpy(&q_bits, &node.stats.Q[action], sizeof(q_bits));
      hash_integer(hash, q_bits);
      hash_integer(hash, node.availability_count[action]);
    }
    hash_integer(hash, node.stats.total_visits);
    hash_integer(hash, node.live_reservation_count);
    hash_integer(hash, static_cast<uint8_t>(node.has_pending_evaluation));
    for (double value : node.stats.value) {
      uint64_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      hash_integer(hash, bits);
    }
  }
  return hash;
}

inline bool snapshot_equal(const MCTSNodeSnapshot64 &left,
                           const MCTSNodeSnapshot64 &right) noexcept {
  return left.key == right.key && left.generation == right.generation &&
         left.valid_actions == right.valid_actions &&
         left.base_policy == right.base_policy &&
         left.stats.N == right.stats.N &&
         left.stats.virtual_loss == right.stats.virtual_loss &&
         left.stats.Q == right.stats.Q &&
         left.stats.total_visits == right.stats.total_visits &&
         left.stats.value == right.stats.value &&
         left.availability_count == right.availability_count &&
         left.live_reservation_count == right.live_reservation_count &&
         left.has_pending_evaluation == right.has_pending_evaluation &&
         left.state == right.state;
}

inline uint64_t config_digest(const MCTSConfig &config) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  auto hash_float = [&](float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_integer(hash, bits);
  };
  hash_float(config.cpuct);
  hash_float(config.dirichlet_alpha);
  hash_float(config.dirichlet_epsilon);
  hash_integer(hash, static_cast<uint8_t>(config.use_dirichlet_noise));
  hash_integer(hash, static_cast<uint8_t>(config.use_determinization));
  hash_integer(hash, config.num_simulations);
  hash_integer(hash, config.num_determinizations);
  hash_float(config.fpu);
  hash_integer(hash, static_cast<uint8_t>(config.forced_playouts));
  hash_float(config.forced_playouts_k);
  return hash;
}

inline uint64_t policy_digest(const Policy &policy) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  for (float value : policy) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    hash_integer(hash, bits);
  }
  return hash;
}

inline uint64_t inference_digest(const Policy &policy,
                                 const Value &value) noexcept {
  uint64_t hash = policy_digest(policy);
  for (double element : value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &element, sizeof(bits));
    hash_integer(hash, bits);
  }
  return hash;
}

inline uint64_t mask_digest(const ActionMask &mask) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  for (uint8_t value : mask)
    hash_integer(hash, value);
  return hash;
}

inline uint64_t mask_digest(ActionMaskBits mask) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    const uint8_t value =
        mcts_action_mask::contains(mask, action) ? uint8_t{1} : uint8_t{0};
    hash_integer(hash, value);
  }
  return hash;
}

inline uint64_t ledger_digest(const SearchLedgerSnapshot &ledger) noexcept {
  uint64_t hash = 1469598103934665603ULL;
#define CSPLENDOR_TRACE_LEDGER(field) hash_integer(hash, ledger.field)
  CSPLENDOR_TRACE_LEDGER(issued);
  CSPLENDOR_TRACE_LEDGER(selected);
  CSPLENDOR_TRACE_LEDGER(evaluation_owner);
  CSPLENDOR_TRACE_LEDGER(evaluation_waiter);
  CSPLENDOR_TRACE_LEDGER(evaluation_requested);
  CSPLENDOR_TRACE_LEDGER(evaluated_boards);
  CSPLENDOR_TRACE_LEDGER(completed_evaluated);
  CSPLENDOR_TRACE_LEDGER(completed_terminal);
  CSPLENDOR_TRACE_LEDGER(completed_max_depth);
  CSPLENDOR_TRACE_LEDGER(cancelled);
  CSPLENDOR_TRACE_LEDGER(failed);
  CSPLENDOR_TRACE_LEDGER(virtual_loss_added);
  CSPLENDOR_TRACE_LEDGER(virtual_loss_released);
  CSPLENDOR_TRACE_LEDGER(reservations_committed);
  CSPLENDOR_TRACE_LEDGER(reservations_aborted);
  CSPLENDOR_TRACE_LEDGER(expansion_claimed);
  CSPLENDOR_TRACE_LEDGER(expansion_published);
  CSPLENDOR_TRACE_LEDGER(expansion_waited);
  CSPLENDOR_TRACE_LEDGER(stale_result);
  CSPLENDOR_TRACE_LEDGER(duplicate_result);
  CSPLENDOR_TRACE_LEDGER(invalid_replay);
  CSPLENDOR_TRACE_LEDGER(integrity_errors);
  CSPLENDOR_TRACE_LEDGER(max_inflight_observed);
#undef CSPLENDOR_TRACE_LEDGER
  return hash;
}

} // namespace trace_detail

inline uint64_t canonical_tree_digest(const ConcurrentTree &tree) {
  return trace_detail::snapshot_digest(tree.snapshot_all());
}

enum class TraceLeafRole : uint8_t { None = 0, Owner = 1, Waiter = 2 };

struct DeterministicTraceTicketData {
  TraceLeafRole leaf_role = TraceLeafRole::None;
  TreeKey leaf_key{};
  uint64_t pending_id = 0;
  uint64_t feature_digest = 0;
  uint64_t world_mask_digest = 0;
  uint64_t inference_digest = 0;
};

struct DeterministicTraceEvent {
  uint64_t simulation_id = 0;
  uint64_t determinization_seed = 0;
  CompletionKind completion = CompletionKind::None;
  std::vector<ReservedPathStep> path;
  Value value{};
  DeterministicTraceTicketData ticket;
  uint64_t ledger_digest = 0;
  std::vector<MCTSNodeSnapshot64> nodes;
  uint64_t tree_digest = 0;
  uint64_t chain_digest = 0;
};

struct DeterministicTrace {
  uint32_t version = MCTS_PARALLEL_TRACE_VERSION;
  TreeKey root_key{};
  uint64_t master_seed = 0;
  uint64_t search_nonce = 0;
  uint32_t rng_version = MCTS_RNG_VERSION;
  uint64_t config_digest = 0;
  uint64_t evaluator_version = 0;
  bool has_root_noise = false;
  Policy root_noise{};
  std::vector<MCTSNodeSnapshot64> initial_nodes;
  uint64_t initial_tree_digest = 0;
  uint64_t aggregate_node_snapshots = 0;
  uint64_t previous_chain = 1469598103934665603ULL;
  std::vector<DeterministicTraceEvent> events;

  // Recorder-only state.  It is reconstructed by capture_initial() and is
  // deliberately not serialized.  Event payloads contain deltas while each
  // event digest still authenticates the complete post-commit tree.
  std::unordered_map<TreeKey, MCTSNodeSnapshot64, TreeKeyHash> recording_nodes;

  uint64_t initial_chain_digest() const noexcept {
    uint64_t chain = 1469598103934665603ULL;
    trace_detail::hash_integer(chain, version);
    trace_detail::hash_integer(chain, root_key.position_hash);
    trace_detail::hash_integer(chain, root_key.key_version);
    trace_detail::hash_integer(chain, root_key.observer);
    trace_detail::hash_integer(chain, static_cast<uint8_t>(root_key.domain));
    trace_detail::hash_integer(chain, root_key.mode_bits);
    trace_detail::hash_integer(chain, master_seed);
    trace_detail::hash_integer(chain, search_nonce);
    trace_detail::hash_integer(chain, rng_version);
    trace_detail::hash_integer(chain, config_digest);
    trace_detail::hash_integer(chain, evaluator_version);
    trace_detail::hash_integer(chain, static_cast<uint8_t>(has_root_noise));
    trace_detail::hash_integer(chain, trace_detail::policy_digest(root_noise));
    trace_detail::hash_integer(chain, initial_tree_digest);
    return chain;
  }

  void capture_initial(const ConcurrentTree &tree) {
    initial_nodes = tree.snapshot_all();
    std::sort(
        initial_nodes.begin(), initial_nodes.end(),
        [](const MCTSNodeSnapshot64 &left, const MCTSNodeSnapshot64 &right) {
          return trace_detail::key_less(left.key, right.key);
        });
    if (initial_nodes.size() > MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS)
      throw std::length_error("deterministic initial snapshot is too large");
    aggregate_node_snapshots = initial_nodes.size();
    initial_tree_digest = trace_detail::snapshot_digest(initial_nodes);
    previous_chain = initial_chain_digest();
    recording_nodes.clear();
    recording_nodes.reserve(initial_nodes.size());
    for (const auto &node : initial_nodes) {
      if (!recording_nodes.emplace(node.key, node).second)
        throw std::logic_error(
            "deterministic initial snapshot contains duplicate keys");
    }
  }

  // Bound trace memory before logical simulations mutate the tree.  A commit
  // can change at most MAX_DEPTH path nodes plus one newly published leaf.
  // An epoch's first snapshot can additionally observe reservations made for
  // later tickets, whose release is recorded again, hence the factor of two.
  // The conservative check guarantees record() cannot discover the aggregate
  // limit only after the corresponding search commit has succeeded.
  void ensure_record_capacity(uint64_t event_count,
                              uint64_t setup_nodes = 0) const {
    if (aggregate_node_snapshots > MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS ||
        setup_nodes >
            MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS - aggregate_node_snapshots)
      throw std::length_error("deterministic trace snapshot limit reached");
    const uint64_t remaining = MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS -
                               aggregate_node_snapshots - setup_nodes;
    constexpr uint64_t per_event = static_cast<uint64_t>(MAX_DEPTH) * 2 + 1;
    if (event_count > remaining / per_event)
      throw std::length_error(
          "deterministic trace simulation budget exceeds safe limit");
  }

  void record(uint64_t simulation_id, uint64_t determinization_seed,
              CompletionKind completion,
              const std::vector<ReservedPathStep> &path, const Value &value,
              const DeterministicTraceTicketData &ticket,
              uint64_t ledger_digest, const ConcurrentTree &tree) {
    DeterministicTraceEvent event;
    event.simulation_id = simulation_id;
    event.determinization_seed = determinization_seed;
    event.completion = completion;
    event.path = path;
    event.value = value;
    event.ticket = ticket;
    event.ledger_digest = ledger_digest;
    auto current_nodes = tree.snapshot_all();
    std::sort(
        current_nodes.begin(), current_nodes.end(),
        [](const MCTSNodeSnapshot64 &left, const MCTSNodeSnapshot64 &right) {
          return trace_detail::key_less(left.key, right.key);
        });
    event.tree_digest = trace_detail::snapshot_digest(current_nodes);
    event.nodes.reserve(
        std::min(current_nodes.size(), path.size() + static_cast<size_t>(1)));
    for (const auto &node : current_nodes) {
      const auto previous = recording_nodes.find(node.key);
      if (previous == recording_nodes.end() ||
          !trace_detail::snapshot_equal(previous->second, node))
        event.nodes.push_back(node);
    }
    if (event.nodes.size() >
        MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS - aggregate_node_snapshots)
      throw std::length_error(
          "deterministic trace aggregate snapshot limit reached");
    aggregate_node_snapshots += event.nodes.size();
    recording_nodes.clear();
    recording_nodes.reserve(current_nodes.size());
    for (auto &node : current_nodes)
      recording_nodes.emplace(node.key, std::move(node));
    uint64_t chain = previous_chain;
    trace_detail::hash_integer(chain, simulation_id);
    trace_detail::hash_integer(chain, determinization_seed);
    trace_detail::hash_integer(chain, static_cast<uint8_t>(completion));
    trace_detail::hash_integer(chain, static_cast<uint64_t>(path.size()));
    for (const auto &step : path) {
      trace_detail::hash_integer(chain, step.key.position_hash);
      trace_detail::hash_integer(chain, step.key.key_version);
      trace_detail::hash_integer(chain, step.key.observer);
      trace_detail::hash_integer(chain, static_cast<uint8_t>(step.key.domain));
      trace_detail::hash_integer(chain, step.key.mode_bits);
      trace_detail::hash_integer(chain, step.action);
      trace_detail::hash_integer(chain, step.player);
    }
    for (double element : value) {
      uint64_t bits = 0;
      std::memcpy(&bits, &element, sizeof(bits));
      trace_detail::hash_integer(chain, bits);
    }
    trace_detail::hash_integer(chain, static_cast<uint8_t>(ticket.leaf_role));
    trace_detail::hash_integer(chain, ticket.leaf_key.position_hash);
    trace_detail::hash_integer(chain, ticket.leaf_key.key_version);
    trace_detail::hash_integer(chain, ticket.leaf_key.observer);
    trace_detail::hash_integer(chain,
                               static_cast<uint8_t>(ticket.leaf_key.domain));
    trace_detail::hash_integer(chain, ticket.leaf_key.mode_bits);
    trace_detail::hash_integer(chain, ticket.pending_id);
    trace_detail::hash_integer(chain, ticket.feature_digest);
    trace_detail::hash_integer(chain, ticket.world_mask_digest);
    trace_detail::hash_integer(chain, ticket.inference_digest);
    trace_detail::hash_integer(chain, ledger_digest);
    trace_detail::hash_integer(chain, event.tree_digest);
    event.chain_digest = chain;
    previous_chain = chain;
    events.push_back(std::move(event));
  }

  void verify() const;

  void write(std::ostream &stream) const;

  static DeterministicTrace read(std::istream &stream);
};

uint64_t replay_deterministic_trace(const DeterministicTrace &trace);

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_PARALLEL_TRACE_H
