#ifndef CSPLENDOR_MCTS_PARALLEL_TRACE_H
#define CSPLENDOR_MCTS_PARALLEL_TRACE_H

#include "mcts_concurrent_tree.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
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

inline bool valid_tree_domain(TreeDomain domain) noexcept {
  return domain == TreeDomain::Exact || domain == TreeDomain::Observable ||
         domain == TreeDomain::LegacyExact;
}

inline bool valid_expansion_state(ExpansionState state) noexcept {
  return state == ExpansionState::Unexpanded ||
         state == ExpansionState::Evaluating ||
         state == ExpansionState::Expanded || state == ExpansionState::Terminal;
}

inline bool valid_completion(CompletionKind completion) noexcept {
  return completion == CompletionKind::EvaluatedLeaf ||
         completion == CompletionKind::Terminal ||
         completion == CompletionKind::MaxDepth;
}

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

inline void validate_snapshot(const MCTSNodeSnapshot64 &node) {
  if (!valid_tree_domain(node.key.domain) || !valid_expansion_state(node.state))
    throw std::logic_error("trace node enum is invalid");
  validate_policy(node.base_policy);
  if (!finite_value(node.stats.value))
    throw std::logic_error("trace node value is not finite");
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (node.valid_actions[action] > 1 || !std::isfinite(node.stats.Q[action]))
      throw std::logic_error("trace node statistics are invalid");
  }
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

template <typename Integer>
inline void write_integer(std::ostream &stream, Integer value) {
  using Unsigned = typename std::make_unsigned<Integer>::type;
  Unsigned bits = static_cast<Unsigned>(value);
  for (size_t index = 0; index < sizeof(bits); ++index)
    stream.put(static_cast<char>(bits >> (index * 8)));
  if (!stream)
    throw std::runtime_error("failed to write MCTS trace");
}

template <typename Integer> inline Integer read_integer(std::istream &stream) {
  using Unsigned = typename std::make_unsigned<Integer>::type;
  Unsigned bits = 0;
  for (size_t index = 0; index < sizeof(bits); ++index) {
    const int byte = stream.get();
    if (byte == std::char_traits<char>::eof())
      throw std::runtime_error("truncated MCTS trace");
    bits |= static_cast<Unsigned>(static_cast<uint8_t>(byte)) << (index * 8);
  }
  return static_cast<Integer>(bits);
}

inline void write_snapshot(std::ostream &stream,
                           const MCTSNodeSnapshot64 &node) {
  write_integer(stream, node.key.position_hash);
  write_integer(stream, node.key.key_version);
  write_integer(stream, node.key.observer);
  write_integer(stream, static_cast<uint8_t>(node.key.domain));
  write_integer(stream, node.key.mode_bits);
  write_integer(stream, node.generation);
  write_integer(stream, static_cast<uint8_t>(node.state));
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    write_integer(stream, node.valid_actions[action]);
    uint32_t prior = 0;
    std::memcpy(&prior, &node.base_policy[action], sizeof(prior));
    write_integer(stream, prior);
    write_integer(stream, node.stats.N[action]);
    write_integer(stream, node.stats.virtual_loss[action]);
    uint64_t q = 0;
    std::memcpy(&q, &node.stats.Q[action], sizeof(q));
    write_integer(stream, q);
    write_integer(stream, node.availability_count[action]);
  }
  write_integer(stream, node.stats.total_visits);
  write_integer(stream, node.live_reservation_count);
  write_integer(stream, static_cast<uint8_t>(node.has_pending_evaluation));
  for (double value : node.stats.value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_integer(stream, bits);
  }
}

inline MCTSNodeSnapshot64 read_snapshot(std::istream &stream) {
  MCTSNodeSnapshot64 node;
  node.key.position_hash = read_integer<uint64_t>(stream);
  node.key.key_version = read_integer<uint32_t>(stream);
  node.key.observer = read_integer<uint8_t>(stream);
  node.key.domain = static_cast<TreeDomain>(read_integer<uint8_t>(stream));
  node.key.mode_bits = read_integer<uint8_t>(stream);
  node.generation = read_integer<uint64_t>(stream);
  node.state = static_cast<ExpansionState>(read_integer<uint8_t>(stream));
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    node.valid_actions[action] = read_integer<uint8_t>(stream);
    const uint32_t prior = read_integer<uint32_t>(stream);
    std::memcpy(&node.base_policy[action], &prior, sizeof(prior));
    node.stats.N[action] = read_integer<uint64_t>(stream);
    node.stats.virtual_loss[action] = read_integer<uint64_t>(stream);
    const uint64_t q = read_integer<uint64_t>(stream);
    std::memcpy(&node.stats.Q[action], &q, sizeof(q));
    node.availability_count[action] = read_integer<uint64_t>(stream);
  }
  node.stats.total_visits = read_integer<uint64_t>(stream);
  node.live_reservation_count = read_integer<uint64_t>(stream);
  const uint8_t has_pending = read_integer<uint8_t>(stream);
  if (has_pending > 1)
    throw std::logic_error("trace pending flag is invalid");
  node.has_pending_evaluation = has_pending != 0;
  for (double &value : node.stats.value) {
    const uint64_t bits = read_integer<uint64_t>(stream);
    std::memcpy(&value, &bits, sizeof(bits));
  }
  return node;
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

  void verify() const {
    if (version != MCTS_PARALLEL_TRACE_VERSION)
      throw std::invalid_argument("unsupported deterministic trace version");
    if (rng_version != MCTS_RNG_VERSION)
      throw std::invalid_argument(
          "unsupported deterministic trace RNG version");
    if (!trace_detail::valid_tree_domain(root_key.domain))
      throw std::logic_error("trace root domain is invalid");
    validate_policy(root_noise);
    if (!has_root_noise) {
      for (float value : root_noise) {
        if (value != 0.0f)
          throw std::logic_error("trace stores noise while noise is disabled");
      }
    }
    if (initial_nodes.size() > MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS)
      throw std::length_error("deterministic initial snapshot is too large");
    std::unordered_map<TreeKey, MCTSNodeSnapshot64, TreeKeyHash> replay;
    replay.reserve(initial_nodes.size());
    for (const auto &node : initial_nodes) {
      trace_detail::validate_snapshot(node);
      if (!replay.emplace(node.key, node).second)
        throw std::logic_error("trace initial tree contains duplicate keys");
    }
    if (trace_detail::snapshot_digest(initial_nodes) != initial_tree_digest)
      throw std::logic_error("trace initial tree digest mismatch");
    uint64_t aggregate = initial_nodes.size();
    uint64_t chain = initial_chain_digest();
    uint64_t previous_simulation = 0;
    bool first = true;
    for (const auto &event : events) {
      if (event.nodes.size() > MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS - aggregate)
        throw std::length_error(
            "deterministic trace aggregate snapshot limit exceeded");
      aggregate += event.nodes.size();
      if (!first && event.simulation_id <= previous_simulation)
        throw std::logic_error("trace simulation IDs are not increasing");
      first = false;
      previous_simulation = event.simulation_id;
      const uint64_t expected_seed = derive_search_seed(
          master_seed, root_key, search_nonce,
          SearchRandomDomain::RootDeterminization, event.simulation_id);
      if (event.determinization_seed != expected_seed)
        throw std::logic_error("trace determinization seed mismatch");
      if (!trace_detail::valid_completion(event.completion))
        throw std::logic_error("trace completion kind is invalid");
      if (!finite_value(event.value))
        throw std::logic_error("trace completion value is not finite");
      for (const auto &step : event.path) {
        if (step.action < 0 ||
            step.action >= static_cast<int32_t>(MAX_ACTIONS) ||
            step.player >= NUM_PLAYERS ||
            !trace_detail::valid_tree_domain(step.key.domain))
          throw std::logic_error("trace path step is invalid");
      }
      if (event.ticket.leaf_role != TraceLeafRole::None &&
          event.ticket.leaf_role != TraceLeafRole::Owner &&
          event.ticket.leaf_role != TraceLeafRole::Waiter)
        throw std::logic_error("trace leaf role is invalid");
      if (event.completion == CompletionKind::EvaluatedLeaf &&
          (event.ticket.leaf_role == TraceLeafRole::None ||
           event.ticket.pending_id == 0 || event.ticket.inference_digest == 0))
        throw std::logic_error("trace evaluated leaf metadata is incomplete");
      if (!trace_detail::valid_tree_domain(event.ticket.leaf_key.domain))
        throw std::logic_error("trace leaf domain is invalid");

      std::unordered_map<TreeKey, const MCTSNodeSnapshot64 *, TreeKeyHash>
          changed;
      changed.reserve(event.nodes.size());
      for (const auto &node : event.nodes) {
        trace_detail::validate_snapshot(node);
        if (!changed.emplace(node.key, &node).second)
          throw std::logic_error("trace event contains duplicate node deltas");
        if (replay.find(node.key) == replay.end()) {
          MCTSNodeSnapshot64 fresh = node;
          fresh.stats.N.fill(0);
          fresh.stats.Q.fill(0.0);
          fresh.stats.total_visits = 0;
          replay.emplace(fresh.key, std::move(fresh));
        }
      }

      // Independently apply the committed path's online-mean update.  Other
      // fields (publication, availability and outstanding reservations from
      // the same epoch) remain authenticated by the chained node deltas.
      for (auto iterator = event.path.rbegin(); iterator != event.path.rend();
           ++iterator) {
        if (changed.find(iterator->key) == changed.end())
          throw std::logic_error("trace path node is missing from event delta");
        auto expected = replay.find(iterator->key);
        if (expected == replay.end())
          throw std::logic_error("trace path references an unknown node");
        const size_t action = static_cast<size_t>(iterator->action);
        auto &stats = expected->second.stats;
        if (stats.N[action] == std::numeric_limits<uint64_t>::max() ||
            stats.total_visits == std::numeric_limits<uint64_t>::max())
          throw std::overflow_error("trace replay visit count overflow");
        ++stats.N[action];
        ++stats.total_visits;
        const double count = static_cast<double>(stats.N[action]);
        const double backed_up = event.value[iterator->player];
        stats.Q[action] += (backed_up - stats.Q[action]) / count;
      }
      for (const auto &actual : event.nodes) {
        auto expected = replay.find(actual.key);
        if (expected == replay.end())
          throw std::logic_error("trace replay lost an event node");
        if (expected->second.stats.N != actual.stats.N ||
            expected->second.stats.Q != actual.stats.Q ||
            expected->second.stats.total_visits != actual.stats.total_visits)
          throw std::logic_error("trace replay statistics diverged");
        expected->second = actual;
      }
      std::vector<MCTSNodeSnapshot64> reconstructed;
      reconstructed.reserve(replay.size());
      for (const auto &entry : replay)
        reconstructed.push_back(entry.second);
      const uint64_t digest = trace_detail::snapshot_digest(reconstructed);
      if (digest != event.tree_digest)
        throw std::logic_error("trace tree digest mismatch");
      trace_detail::hash_integer(chain, event.simulation_id);
      trace_detail::hash_integer(chain, event.determinization_seed);
      trace_detail::hash_integer(chain, static_cast<uint8_t>(event.completion));
      trace_detail::hash_integer(chain,
                                 static_cast<uint64_t>(event.path.size()));
      for (const auto &step : event.path) {
        trace_detail::hash_integer(chain, step.key.position_hash);
        trace_detail::hash_integer(chain, step.key.key_version);
        trace_detail::hash_integer(chain, step.key.observer);
        trace_detail::hash_integer(chain,
                                   static_cast<uint8_t>(step.key.domain));
        trace_detail::hash_integer(chain, step.key.mode_bits);
        trace_detail::hash_integer(chain, step.action);
        trace_detail::hash_integer(chain, step.player);
      }
      for (double element : event.value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &element, sizeof(bits));
        trace_detail::hash_integer(chain, bits);
      }
      trace_detail::hash_integer(chain,
                                 static_cast<uint8_t>(event.ticket.leaf_role));
      trace_detail::hash_integer(chain, event.ticket.leaf_key.position_hash);
      trace_detail::hash_integer(chain, event.ticket.leaf_key.key_version);
      trace_detail::hash_integer(chain, event.ticket.leaf_key.observer);
      trace_detail::hash_integer(
          chain, static_cast<uint8_t>(event.ticket.leaf_key.domain));
      trace_detail::hash_integer(chain, event.ticket.leaf_key.mode_bits);
      trace_detail::hash_integer(chain, event.ticket.pending_id);
      trace_detail::hash_integer(chain, event.ticket.feature_digest);
      trace_detail::hash_integer(chain, event.ticket.world_mask_digest);
      trace_detail::hash_integer(chain, event.ticket.inference_digest);
      trace_detail::hash_integer(chain, event.ledger_digest);
      trace_detail::hash_integer(chain, event.tree_digest);
      if (chain != event.chain_digest)
        throw std::logic_error("trace event chain mismatch");
    }
    if (chain != previous_chain)
      throw std::logic_error("trace final chain mismatch");
    if (aggregate != aggregate_node_snapshots)
      throw std::logic_error("trace aggregate snapshot count mismatch");
  }

  void write(std::ostream &stream) const {
    static constexpr uint64_t magic = 0x4353504c54524345ULL;
    trace_detail::write_integer(stream, magic);
    trace_detail::write_integer(stream, version);
    trace_detail::write_integer(stream, root_key.position_hash);
    trace_detail::write_integer(stream, root_key.key_version);
    trace_detail::write_integer(stream, root_key.observer);
    trace_detail::write_integer(stream, static_cast<uint8_t>(root_key.domain));
    trace_detail::write_integer(stream, root_key.mode_bits);
    trace_detail::write_integer(stream, master_seed);
    trace_detail::write_integer(stream, search_nonce);
    trace_detail::write_integer(stream, rng_version);
    trace_detail::write_integer(stream, config_digest);
    trace_detail::write_integer(stream, evaluator_version);
    trace_detail::write_integer(stream, static_cast<uint8_t>(has_root_noise));
    for (float value : root_noise) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      trace_detail::write_integer(stream, bits);
    }
    trace_detail::write_integer(stream, initial_tree_digest);
    trace_detail::write_integer(stream,
                                static_cast<uint64_t>(initial_nodes.size()));
    for (const auto &node : initial_nodes)
      trace_detail::write_snapshot(stream, node);
    trace_detail::write_integer(stream, previous_chain);
    trace_detail::write_integer(stream, static_cast<uint64_t>(events.size()));
    for (const auto &event : events) {
      trace_detail::write_integer(stream, event.simulation_id);
      trace_detail::write_integer(stream, event.determinization_seed);
      trace_detail::write_integer(stream,
                                  static_cast<uint8_t>(event.completion));
      trace_detail::write_integer(stream,
                                  static_cast<uint64_t>(event.path.size()));
      for (const auto &step : event.path) {
        trace_detail::write_integer(stream, step.key.position_hash);
        trace_detail::write_integer(stream, step.key.key_version);
        trace_detail::write_integer(stream, step.key.observer);
        trace_detail::write_integer(stream,
                                    static_cast<uint8_t>(step.key.domain));
        trace_detail::write_integer(stream, step.key.mode_bits);
        trace_detail::write_integer(stream, step.action);
        trace_detail::write_integer(stream, step.player);
      }
      for (double value : event.value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        trace_detail::write_integer(stream, bits);
      }
      trace_detail::write_integer(stream,
                                  static_cast<uint8_t>(event.ticket.leaf_role));
      trace_detail::write_integer(stream, event.ticket.leaf_key.position_hash);
      trace_detail::write_integer(stream, event.ticket.leaf_key.key_version);
      trace_detail::write_integer(stream, event.ticket.leaf_key.observer);
      trace_detail::write_integer(
          stream, static_cast<uint8_t>(event.ticket.leaf_key.domain));
      trace_detail::write_integer(stream, event.ticket.leaf_key.mode_bits);
      trace_detail::write_integer(stream, event.ticket.pending_id);
      trace_detail::write_integer(stream, event.ticket.feature_digest);
      trace_detail::write_integer(stream, event.ticket.world_mask_digest);
      trace_detail::write_integer(stream, event.ticket.inference_digest);
      trace_detail::write_integer(stream, event.ledger_digest);
      trace_detail::write_integer(stream, event.tree_digest);
      trace_detail::write_integer(stream, event.chain_digest);
      trace_detail::write_integer(stream,
                                  static_cast<uint64_t>(event.nodes.size()));
      for (const auto &node : event.nodes)
        trace_detail::write_snapshot(stream, node);
    }
  }

  static DeterministicTrace read(std::istream &stream) {
    static constexpr uint64_t magic = 0x4353504c54524345ULL;
    if (trace_detail::read_integer<uint64_t>(stream) != magic)
      throw std::invalid_argument("invalid deterministic trace magic");
    DeterministicTrace trace;
    trace.version = trace_detail::read_integer<uint32_t>(stream);
    if (trace.version != MCTS_PARALLEL_TRACE_VERSION)
      throw std::invalid_argument("unsupported deterministic trace version");
    trace.root_key.position_hash = trace_detail::read_integer<uint64_t>(stream);
    trace.root_key.key_version = trace_detail::read_integer<uint32_t>(stream);
    trace.root_key.observer = trace_detail::read_integer<uint8_t>(stream);
    trace.root_key.domain =
        static_cast<TreeDomain>(trace_detail::read_integer<uint8_t>(stream));
    trace.root_key.mode_bits = trace_detail::read_integer<uint8_t>(stream);
    trace.master_seed = trace_detail::read_integer<uint64_t>(stream);
    trace.search_nonce = trace_detail::read_integer<uint64_t>(stream);
    trace.rng_version = trace_detail::read_integer<uint32_t>(stream);
    trace.config_digest = trace_detail::read_integer<uint64_t>(stream);
    trace.evaluator_version = trace_detail::read_integer<uint64_t>(stream);
    const uint8_t has_root_noise = trace_detail::read_integer<uint8_t>(stream);
    if (has_root_noise > 1)
      throw std::logic_error("trace root-noise flag is invalid");
    trace.has_root_noise = has_root_noise != 0;
    for (float &value : trace.root_noise) {
      const uint32_t bits = trace_detail::read_integer<uint32_t>(stream);
      std::memcpy(&value, &bits, sizeof(bits));
    }
    trace.initial_tree_digest = trace_detail::read_integer<uint64_t>(stream);
    const uint64_t initial_node_count =
        trace_detail::read_integer<uint64_t>(stream);
    if (initial_node_count > MAX_TREE_SIZE)
      throw std::length_error("deterministic initial snapshot is too large");
    trace.initial_nodes.reserve(static_cast<size_t>(initial_node_count));
    for (uint64_t node = 0; node < initial_node_count; ++node)
      trace.initial_nodes.push_back(trace_detail::read_snapshot(stream));
    trace.aggregate_node_snapshots = initial_node_count;
    trace.previous_chain = trace_detail::read_integer<uint64_t>(stream);
    const uint64_t event_count = trace_detail::read_integer<uint64_t>(stream);
    // The writer rejects larger searches before mutation. Mirror that bound
    // before reserve() so an untrusted trace cannot force excessive event,
    // path and replay work even when every event claims zero node deltas.
    if (event_count > MCTS_PARALLEL_TRACE_MAX_EVENTS)
      throw std::length_error("deterministic trace has too many events");
    trace.events.reserve(static_cast<size_t>(event_count));
    for (uint64_t index = 0; index < event_count; ++index) {
      DeterministicTraceEvent event;
      event.simulation_id = trace_detail::read_integer<uint64_t>(stream);
      event.determinization_seed = trace_detail::read_integer<uint64_t>(stream);
      event.completion = static_cast<CompletionKind>(
          trace_detail::read_integer<uint8_t>(stream));
      const uint64_t path_count = trace_detail::read_integer<uint64_t>(stream);
      if (path_count > MAX_DEPTH)
        throw std::length_error("deterministic trace path is too long");
      event.path.reserve(static_cast<size_t>(path_count));
      for (uint64_t path = 0; path < path_count; ++path) {
        ReservedPathStep step;
        step.key.position_hash = trace_detail::read_integer<uint64_t>(stream);
        step.key.key_version = trace_detail::read_integer<uint32_t>(stream);
        step.key.observer = trace_detail::read_integer<uint8_t>(stream);
        step.key.domain = static_cast<TreeDomain>(
            trace_detail::read_integer<uint8_t>(stream));
        step.key.mode_bits = trace_detail::read_integer<uint8_t>(stream);
        step.action = trace_detail::read_integer<int32_t>(stream);
        step.player = trace_detail::read_integer<uint8_t>(stream);
        event.path.push_back(step);
      }
      for (double &value : event.value) {
        const uint64_t bits = trace_detail::read_integer<uint64_t>(stream);
        std::memcpy(&value, &bits, sizeof(bits));
      }
      event.ticket.leaf_role = static_cast<TraceLeafRole>(
          trace_detail::read_integer<uint8_t>(stream));
      event.ticket.leaf_key.position_hash =
          trace_detail::read_integer<uint64_t>(stream);
      event.ticket.leaf_key.key_version =
          trace_detail::read_integer<uint32_t>(stream);
      event.ticket.leaf_key.observer =
          trace_detail::read_integer<uint8_t>(stream);
      event.ticket.leaf_key.domain =
          static_cast<TreeDomain>(trace_detail::read_integer<uint8_t>(stream));
      event.ticket.leaf_key.mode_bits =
          trace_detail::read_integer<uint8_t>(stream);
      event.ticket.pending_id = trace_detail::read_integer<uint64_t>(stream);
      event.ticket.feature_digest =
          trace_detail::read_integer<uint64_t>(stream);
      event.ticket.world_mask_digest =
          trace_detail::read_integer<uint64_t>(stream);
      event.ticket.inference_digest =
          trace_detail::read_integer<uint64_t>(stream);
      event.ledger_digest = trace_detail::read_integer<uint64_t>(stream);
      event.tree_digest = trace_detail::read_integer<uint64_t>(stream);
      event.chain_digest = trace_detail::read_integer<uint64_t>(stream);
      const uint64_t node_count = trace_detail::read_integer<uint64_t>(stream);
      if (node_count > MAX_TREE_SIZE)
        throw std::length_error("deterministic trace snapshot is too large");
      if (node_count >
          MCTS_PARALLEL_TRACE_MAX_SNAPSHOTS - trace.aggregate_node_snapshots)
        throw std::length_error(
            "deterministic trace aggregate snapshot limit exceeded");
      trace.aggregate_node_snapshots += node_count;
      event.nodes.reserve(static_cast<size_t>(node_count));
      for (uint64_t node = 0; node < node_count; ++node)
        event.nodes.push_back(trace_detail::read_snapshot(stream));
      trace.events.push_back(std::move(event));
    }
    trace.verify();
    return trace;
  }
};

inline uint64_t replay_deterministic_trace(const DeterministicTrace &trace) {
  // verify() is the replay interpreter: it reconstructs the tree from the
  // initial snapshot plus event deltas and independently reapplies every
  // reverse-path N/Q/visit update before accepting the chained tree digest.
  trace.verify();
  return trace.events.empty() ? 0 : trace.events.back().tree_digest;
}

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_PARALLEL_TRACE_H
