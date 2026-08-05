#include "mcts_parallel_trace.h"

#include <cmath>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcts_parallel {
namespace trace_detail {

bool valid_tree_domain(TreeDomain domain) noexcept {
  return domain == TreeDomain::Exact || domain == TreeDomain::Observable ||
         domain == TreeDomain::LegacyExact;
}

bool valid_expansion_state(ExpansionState state) noexcept {
  return state == ExpansionState::Unexpanded ||
         state == ExpansionState::Evaluating ||
         state == ExpansionState::Expanded || state == ExpansionState::Terminal;
}

bool valid_completion(CompletionKind completion) noexcept {
  return completion == CompletionKind::EvaluatedLeaf ||
         completion == CompletionKind::Terminal ||
         completion == CompletionKind::MaxDepth;
}

void validate_snapshot(const MCTSNodeSnapshot64 &node) {
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

void write_snapshot(std::ostream &stream, const MCTSNodeSnapshot64 &node) {
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

MCTSNodeSnapshot64 read_snapshot(std::istream &stream) {
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

void DeterministicTrace::verify() const {
  if (version != MCTS_PARALLEL_TRACE_VERSION)
    throw std::invalid_argument("unsupported deterministic trace version");
  if (rng_version != MCTS_RNG_VERSION)
    throw std::invalid_argument("unsupported deterministic trace RNG version");
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
      if (step.action < 0 || step.action >= static_cast<int32_t>(MAX_ACTIONS) ||
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
    trace_detail::hash_integer(chain, static_cast<uint64_t>(event.path.size()));
    for (const auto &step : event.path) {
      trace_detail::hash_integer(chain, step.key.position_hash);
      trace_detail::hash_integer(chain, step.key.key_version);
      trace_detail::hash_integer(chain, step.key.observer);
      trace_detail::hash_integer(chain, static_cast<uint8_t>(step.key.domain));
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

void DeterministicTrace::write(std::ostream &stream) const {
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
    trace_detail::write_integer(stream, static_cast<uint8_t>(event.completion));
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

DeterministicTrace DeterministicTrace::read(std::istream &stream) {
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
      step.key.domain =
          static_cast<TreeDomain>(trace_detail::read_integer<uint8_t>(stream));
      step.key.mode_bits = trace_detail::read_integer<uint8_t>(stream);
      step.action = trace_detail::read_integer<int32_t>(stream);
      step.player = trace_detail::read_integer<uint8_t>(stream);
      event.path.push_back(step);
    }
    for (double &value : event.value) {
      const uint64_t bits = trace_detail::read_integer<uint64_t>(stream);
      std::memcpy(&value, &bits, sizeof(bits));
    }
    event.ticket.leaf_role =
        static_cast<TraceLeafRole>(trace_detail::read_integer<uint8_t>(stream));
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
    event.ticket.feature_digest = trace_detail::read_integer<uint64_t>(stream);
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

uint64_t replay_deterministic_trace(const DeterministicTrace &trace) {
  // verify() is the replay interpreter: it reconstructs the tree from the
  // initial snapshot plus event deltas and independently reapplies every
  // reverse-path N/Q/visit update before accepting the chained tree digest.
  trace.verify();
  return trace.events.empty() ? 0 : trace.events.back().tree_digest;
}

} // namespace mcts_parallel
