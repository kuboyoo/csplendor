#include "mcts_root_parallel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace mcts_parallel;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Exception, typename Function>
bool throws_as(Function &&function) {
  try {
    function();
  } catch (const Exception &) {
    return true;
  }
  return false;
}

template <typename Function> bool throws_std_exception(Function &&function) {
  try {
    function();
  } catch (const std::exception &) {
    return true;
  }
  return false;
}

template <typename Function>
void run_named(const char *name, Function &&function) {
  try {
    function();
  } catch (const std::exception &error) {
    throw std::runtime_error(std::string(name) + ": " + error.what());
  }
}

ParallelInferenceResult
evaluate_request(const ParallelInferenceRequest &request) {
  ParallelInferenceResult result;
  uint64_t available = 0;
  for (uint8_t entry : request.owner_world_mask)
    available += entry != 0 ? 1U : 0U;
  if (available != 0) {
    const float probability = 1.0f / static_cast<float>(available);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      result.policy[action] =
          request.owner_world_mask[action] ? probability : 0.0f;
  }
  // The value depends only on the immutable request identity and is exactly
  // representable. This makes a replay mismatch easier to diagnose than a
  // policy-only evaluator while remaining independent of completion order.
  const float value =
      static_cast<float>(
          static_cast<int64_t>(request.key.position_hash & 7ULL) - 3) /
      8.0f;
  result.value = {value, -value};
  return result;
}

std::vector<ParallelInferenceResult>
evaluate_batch(const std::vector<ParallelInferenceRequest> &requests) {
  std::vector<ParallelInferenceResult> results;
  results.reserve(requests.size());
  for (const auto &request : requests)
    results.push_back(evaluate_request(request));
  return results;
}

MCTSConfig replay_config() {
  MCTSConfig config;
  config.use_determinization = false;
  config.use_dirichlet_noise = true;
  config.forced_playouts = true;
  config.num_simulations = 41;
  return config;
}

ParallelSearchOptions
replay_options(uint32_t threads, TreeBackend backend = TreeBackend::Sharded) {
  ParallelSearchOptions options;
  options.mode = ParallelSearchMode::DeterministicEpoch;
  options.tree_backend = backend;
  options.shard_count = 8;
  options.num_threads = threads;
  options.batch_size = 5;
  options.deterministic_epoch_size = 7;
  options.max_inflight = 16;
  options.num_simulations = 41;
  options.master_seed = 0x123456789abcdef0ULL;
  options.search_nonce = 0x1020304050607080ULL;
  options.simulation_id_base = 700;
  options.evaluator_version = 23;
  return options;
}

std::string serialize_trace(const DeterministicTrace &trace) {
  std::ostringstream stream(std::ios::out | std::ios::binary);
  trace.write(stream);
  return stream.str();
}

uint64_t trace_bytes_digest(const std::string &bytes) {
  uint64_t digest = 1469598103934665603ULL;
  for (unsigned char byte : bytes) {
    digest ^= byte;
    digest *= 1099511628211ULL;
  }
  return digest;
}

DeterministicTrace deserialize_trace(const std::string &bytes) {
  std::istringstream stream(bytes, std::ios::in | std::ios::binary);
  return DeterministicTrace::read(stream);
}

struct ReplayRun {
  ParallelSearchResult result;
  DeterministicTrace trace;
  std::string bytes;
};

ReplayRun run_replay(uint32_t threads,
                     TreeBackend backend = TreeBackend::Sharded) {
  MCTS mcts(replay_config());
  // A fresh MCTS owns a generation-0 coarse tree. Selecting sharded replaces
  // it and advances to generation 1, whereas selecting coarse reuses it. Give
  // the coarse oracle the same logical generation so the authenticated trace
  // comparison measures backend behavior rather than setup history.
  if (backend == TreeBackend::Coarse)
    mcts.clear();
  Game root(424242);
  ParallelMCTSSearcher searcher;
  ReplayRun run;
  run.result =
      searcher.run(mcts, root, replay_options(threads, backend),
                   ParallelInferenceFunction(evaluate_batch), 1.0f, &run.trace);
  run.bytes = serialize_trace(run.trace);
  return run;
}

void require_same_result(const ParallelSearchResult &left,
                         const ParallelSearchResult &right) {
  require(left.visits == right.visits,
          "deterministic visit arrays differ by thread count");
  require(left.q_values == right.q_values,
          "deterministic Q arrays differ by thread count");
  require(left.probabilities == right.probabilities,
          "deterministic probabilities differ by thread count");
  require(left.resolved_seed == right.resolved_seed &&
              left.search_nonce == right.search_nonce &&
              left.tree_generation == right.tree_generation &&
              left.tree_size == right.tree_size &&
              left.stop_reason == right.stop_reason &&
              left.partial == right.partial,
          "deterministic result metadata differs by thread count");
  require(left.ledger.issued == right.ledger.issued &&
              left.ledger.selected == right.ledger.selected &&
              left.ledger.evaluation_owner == right.ledger.evaluation_owner &&
              left.ledger.evaluation_waiter == right.ledger.evaluation_waiter &&
              left.ledger.evaluated_boards == right.ledger.evaluated_boards &&
              left.ledger.completed() == right.ledger.completed() &&
              left.ledger.virtual_loss_added ==
                  right.ledger.virtual_loss_added &&
              left.ledger.virtual_loss_released ==
                  right.ledger.virtual_loss_released,
          "deterministic ledger differs by thread count");
}

void test_deterministic_epoch_trace_is_thread_count_and_backend_invariant() {
  const ReplayRun reference = run_replay(1, TreeBackend::Coarse);
  require(reference.result.stop_reason == SearchStopReason::Completed &&
              !reference.result.partial &&
              reference.result.ledger.completed() == 41,
          "deterministic reference search did not complete");
  require(reference.trace.events.size() == 41,
          "deterministic trace omitted simulations");
  require(reference.trace.events.front().simulation_id == 700 &&
              reference.trace.events.back().simulation_id == 740,
          "deterministic trace simulation range is incorrect");
  const uint64_t digest = replay_deterministic_trace(reference.trace);
  require(digest == reference.trace.events.back().tree_digest,
          "deterministic replay returned the wrong final digest");

  for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
    const ReplayRun backend_reference = run_replay(1, backend);
    require_same_result(reference.result, backend_reference.result);
    require(backend_reference.trace.initial_tree_digest ==
                    reference.trace.initial_tree_digest &&
                backend_reference.trace.previous_chain ==
                    reference.trace.previous_chain &&
                backend_reference.trace.events.size() ==
                    reference.trace.events.size(),
            "deterministic trace header differs by backend");
    require(backend_reference.bytes == reference.bytes,
            "canonical binary trace differs by backend");

    for (uint32_t threads : {1U, 2U, 4U, 8U}) {
      const ReplayRun candidate = run_replay(threads, backend);
      require_same_result(reference.result, candidate.result);
      require(candidate.bytes == reference.bytes,
              "canonical binary trace differs by thread count/backend");
      require(replay_deterministic_trace(candidate.trace) == digest,
              "deterministic tree digest differs by thread count/backend");
    }
  }
}

uint64_t append_manifest_seed(uint64_t digest, uint64_t seed) {
  constexpr uint64_t fnv_prime = 1099511628211ULL;
  for (uint32_t byte = 0; byte < 8; ++byte) {
    digest ^= (seed >> (byte * 8U)) & 0xffU;
    digest *= fnv_prime;
  }
  return digest;
}

SearchRandomDomain manifest_domain(size_t index) {
  constexpr std::array<SearchRandomDomain, 6> domains = {
      SearchRandomDomain::RootDeterminization,
      SearchRandomDomain::ExtraWorld,
      SearchRandomDomain::RootDirichlet,
      SearchRandomDomain::PuctTieBreak,
      SearchRandomDomain::FinalTemperature,
      SearchRandomDomain::StressScheduler,
  };
  return domains[index % domains.size()];
}

uint64_t manifest_seed(const SearchRandomContext &context, size_t index) {
  return context.seed_for(manifest_domain(index),
                          context.simulation_id_base + index,
                          static_cast<uint32_t>((index * 17U) % 257U),
                          static_cast<uint32_t>((index * 29U) % 509U));
}

void test_derive_seed_10000_tuple_manifest_is_stable_and_collision_free() {
  constexpr size_t tuple_count = 10000;
  SearchRandomContext context;
  context.resolved_master_seed = 0x8b8b8b8b12345678ULL;
  context.root_key = {0x123456789abcdef0ULL, MCTS_TREE_KEY_VERSION, 1,
                      TreeDomain::Observable, MCTS_MODE_SIMPLE_PAYMENT};
  context.search_nonce = 0xfedcba9876543210ULL;
  context.simulation_id_base = 9000;

  std::vector<uint64_t> expected(tuple_count);
  std::unordered_set<uint64_t> unique;
  unique.reserve(tuple_count * 2);
  uint64_t digest = 1469598103934665603ULL;
  for (size_t index = 0; index < tuple_count; ++index) {
    expected[index] = manifest_seed(context, index);
    require(unique.insert(expected[index]).second,
            "10,000-tuple seed manifest contains a collision");
    digest = append_manifest_seed(digest, expected[index]);
  }
  // This golden authenticates the tuple field order, domain tags and RNG
  // version, rather than merely checking repeatability within one process.
  constexpr uint64_t expected_manifest_digest = 0x3f030b175c40b745ULL;
  require(digest == expected_manifest_digest,
          "10,000-tuple seed manifest golden changed");

  // Derivation is stateless. Enumerating logical tickets in reverse order is
  // therefore a valid seed-manifest reorder test; it does not pretend that the
  // current synchronous evaluator API can complete callbacks out of order.
  for (size_t offset = 0; offset < tuple_count; ++offset) {
    const size_t index = tuple_count - 1 - offset;
    require(manifest_seed(context, index) == expected[index],
            "reverse enumeration changed a logical tuple seed");
  }

  // Simulate strided worker assignment for every supported deterministic
  // worker count. Worker/thread identity is intentionally absent from the
  // derivation tuple.
  for (size_t worker_count : {1U, 2U, 4U, 8U, 16U}) {
    std::vector<uint64_t> assigned(tuple_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
      for (size_t index = worker; index < tuple_count; index += worker_count)
        assigned[index] = manifest_seed(context, index);
    }
    require(assigned == expected,
            "worker assignment changed the logical seed manifest");
  }

  // A seeded permutation covers random enumeration without depending on the
  // host standard library's shuffle implementation.
  std::vector<size_t> order(tuple_count);
  std::iota(order.begin(), order.end(), size_t{0});
  PortableRng scheduler(
      context.seed_for(SearchRandomDomain::StressScheduler, 0, 0, 0));
  portable_shuffle(order.begin(), order.end(), scheduler);
  for (size_t index : order)
    require(manifest_seed(context, index) == expected[index],
            "seeded-random enumeration changed a logical tuple seed");

  const uint64_t baseline =
      context.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7, 11);
  auto changed = context;
  changed.resolved_master_seed ^= 1;
  require(changed.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7,
                           11) != baseline,
          "master seed is missing from seed identity");
  changed = context;
  changed.root_key.position_hash ^= 1;
  require(changed.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7,
                           11) != baseline,
          "root hash is missing from seed identity");
  changed = context;
  changed.root_key.observer = 0;
  require(changed.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7,
                           11) != baseline,
          "observer is missing from seed identity");
  changed = context;
  changed.root_key.mode_bits = 0;
  require(changed.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7,
                           11) != baseline,
          "mode bits are missing from seed identity");
  changed = context;
  ++changed.search_nonce;
  require(changed.seed_for(SearchRandomDomain::RootDeterminization, 12345, 7,
                           11) != baseline,
          "search nonce is missing from seed identity");
  require(context.seed_for(SearchRandomDomain::RootDirichlet, 12345, 7, 11) !=
                  baseline &&
              context.seed_for(SearchRandomDomain::RootDeterminization, 12346,
                               7, 11) != baseline &&
              context.seed_for(SearchRandomDomain::RootDeterminization, 12345,
                               8, 11) != baseline &&
              context.seed_for(SearchRandomDomain::RootDeterminization, 12345,
                               7, 12) != baseline,
          "domain/simulation/world/sub-index separation is incomplete");
}

void test_trace_binary_roundtrip_and_rejection() {
  const ReplayRun original = run_replay(4);
  constexpr size_t expected_trace_size = 196981;
  constexpr uint64_t expected_trace_digest = 13940474573569027194ULL;
  if (original.bytes.size() != expected_trace_size ||
      trace_bytes_digest(original.bytes) != expected_trace_digest) {
    throw std::runtime_error(
        "canonical trace golden changed: size=" +
        std::to_string(original.bytes.size()) +
        ", digest=" + std::to_string(trace_bytes_digest(original.bytes)));
  }
  const DeterministicTrace decoded = deserialize_trace(original.bytes);
  require(serialize_trace(decoded) == original.bytes,
          "trace binary roundtrip is not canonical");
  require(replay_deterministic_trace(decoded) ==
              replay_deterministic_trace(original.trace),
          "trace binary roundtrip changed the digest");

  std::string invalid_magic = original.bytes;
  invalid_magic.front() ^= 0x01;
  require(throws_as<std::invalid_argument>(
              [&] { (void)deserialize_trace(invalid_magic); }),
          "trace reader accepted an invalid magic value");

  std::string invalid_version = original.bytes;
  require(invalid_version.size() > 12, "trace fixture is unexpectedly short");
  const uint32_t unsupported_version = MCTS_PARALLEL_TRACE_VERSION + 1;
  for (size_t byte = 0; byte < sizeof(unsupported_version); ++byte)
    invalid_version[8 + byte] =
        static_cast<char>(unsupported_version >> (byte * 8));
  require(throws_as<std::invalid_argument>(
              [&] { (void)deserialize_trace(invalid_version); }),
          "trace reader accepted an unsupported schema version");

  std::string truncated = original.bytes;
  truncated.pop_back();
  require(throws_as<std::runtime_error>(
              [&] { (void)deserialize_trace(truncated); }),
          "trace reader accepted a truncated payload");

  std::string tampered = original.bytes;
  tampered.back() ^= 0x01;
  require(
      throws_as<std::logic_error>([&] { (void)deserialize_trace(tampered); }),
      "trace reader accepted a tampered snapshot");

  DeterministicTrace invalid_completion = original.trace;
  invalid_completion.events.front().completion = CompletionKind::None;
  require(throws_as<std::logic_error>([&] { invalid_completion.verify(); }),
          "trace verifier accepted a missing completion kind");

  invalid_completion = original.trace;
  invalid_completion.events.front().completion =
      static_cast<CompletionKind>(255);
  require(throws_as<std::logic_error>([&] { invalid_completion.verify(); }),
          "trace verifier accepted an unknown completion kind");

  DeterministicTrace empty;
  empty.initial_tree_digest = trace_detail::snapshot_digest({});
  empty.aggregate_node_snapshots = 0;
  empty.previous_chain = empty.initial_chain_digest();
  std::string excessive_event_count = serialize_trace(empty);
  require(excessive_event_count.size() >= sizeof(uint64_t),
          "empty trace fixture is unexpectedly short");
  constexpr uint64_t parser_event_limit_plus_one = 1000001;
  for (size_t byte = 0; byte < sizeof(uint64_t); ++byte) {
    excessive_event_count[excessive_event_count.size() - sizeof(uint64_t) +
                          byte] =
        static_cast<char>(parser_event_limit_plus_one >> (byte * 8));
  }
  require(throws_as<std::length_error>(
              [&] { (void)deserialize_trace(excessive_event_count); }),
          "trace reader reserved an excessive event count");

  // Exercise truncation throughout the framing, rather than only at the final
  // digest. This catches unchecked reads after variable-length path/node
  // sections without turning the unit test into a quadratic parser fuzz run.
  const size_t truncation_step =
      std::max<size_t>(1, original.bytes.size() / 128);
  for (size_t cut = 0; cut < original.bytes.size(); cut += truncation_step) {
    const std::string prefix = original.bytes.substr(0, cut);
    require(throws_std_exception([&] { (void)deserialize_trace(prefix); }),
            "trace reader accepted a truncated interior prefix");
  }

  // Header enum/flag validation must happen before any large variable-length
  // allocation. The offsets below are composed from the fixed-width schema
  // fields so a schema edit fails visibly instead of mutating an arbitrary
  // payload byte.
  constexpr size_t root_domain_offset = sizeof(uint64_t) + sizeof(uint32_t) +
                                        sizeof(uint64_t) + sizeof(uint32_t) +
                                        sizeof(uint8_t);
  constexpr size_t root_noise_flag_offset =
      sizeof(uint64_t) + sizeof(uint32_t) + sizeof(uint64_t) +
      sizeof(uint32_t) + sizeof(uint8_t) * 3 + sizeof(uint64_t) * 2 +
      sizeof(uint32_t) + sizeof(uint64_t) * 2;
  require(original.bytes.size() > root_noise_flag_offset,
          "trace header fixture is unexpectedly short");
  std::string invalid_domain = original.bytes;
  invalid_domain[root_domain_offset] = static_cast<char>(0xff);
  require(throws_as<std::logic_error>(
              [&] { (void)deserialize_trace(invalid_domain); }),
          "trace reader accepted an invalid root domain");
  std::string invalid_noise_flag = original.bytes;
  invalid_noise_flag[root_noise_flag_offset] = static_cast<char>(2);
  require(throws_as<std::logic_error>(
              [&] { (void)deserialize_trace(invalid_noise_flag); }),
          "trace reader accepted a non-boolean root-noise flag");

  DeterministicTrace invalid_structure = original.trace;
  ++invalid_structure.rng_version;
  require(throws_as<std::invalid_argument>([&] { invalid_structure.verify(); }),
          "trace verifier accepted an unsupported RNG version");

  invalid_structure = original.trace;
  invalid_structure.root_key.domain = static_cast<TreeDomain>(255);
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted an invalid root domain");

  require(original.trace.events.size() > 1,
          "trace structure fixture has too few events");
  invalid_structure = original.trace;
  invalid_structure.events[1].simulation_id =
      invalid_structure.events[0].simulation_id;
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted non-increasing simulation IDs");

  invalid_structure = original.trace;
  invalid_structure.events.front().value[0] =
      std::numeric_limits<double>::quiet_NaN();
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted a non-finite completion value");

  invalid_structure = original.trace;
  invalid_structure.events.front().ticket.leaf_role =
      static_cast<TraceLeafRole>(255);
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted an invalid leaf role");

  invalid_structure = original.trace;
  auto path_event = std::find_if(
      invalid_structure.events.begin(), invalid_structure.events.end(),
      [](const DeterministicTraceEvent &event) { return !event.path.empty(); });
  require(path_event != invalid_structure.events.end(),
          "trace structure fixture has no reserved path");
  path_event->path.front().action = static_cast<int32_t>(MAX_ACTIONS);
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted an out-of-range path action");

  invalid_structure = original.trace;
  ++invalid_structure.aggregate_node_snapshots;
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted inconsistent aggregate metadata");

  require(!original.trace.initial_nodes.empty(),
          "trace structure fixture has no initial snapshot");
  invalid_structure = original.trace;
  invalid_structure.initial_nodes.push_back(
      invalid_structure.initial_nodes.front());
  ++invalid_structure.aggregate_node_snapshots;
  require(throws_as<std::logic_error>([&] { invalid_structure.verify(); }),
          "trace verifier accepted duplicate initial node keys");

  invalid_structure = original.trace;
  invalid_structure.events.front().path.resize(static_cast<size_t>(MAX_DEPTH) +
                                               1);
  const std::string excessive_path = serialize_trace(invalid_structure);
  require(throws_as<std::length_error>(
              [&] { (void)deserialize_trace(excessive_path); }),
          "trace reader allocated an excessive reserved path");
}

void test_root_parallel_budget_merge_and_seed_ranges() {
  constexpr uint32_t workers = 4;
  constexpr uint64_t budget = 33;
  constexpr uint64_t base = 1000;
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  options.mode = ParallelSearchMode::RootParallel;
  options.num_simulations = 0;
  options.simulation_id_base = base;
  options.batch_size = 3;
  options.deterministic_epoch_size = 4;

  std::array<std::vector<uint64_t>, workers> request_ids;
  std::mutex ids_mutex;
  std::atomic<uint64_t> bootstrap_callbacks{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t worker) -> ParallelInferenceFunction {
    return [&, worker](const std::vector<ParallelInferenceRequest> &requests) {
      {
        std::lock_guard<std::mutex> lock(ids_mutex);
        for (const auto &request : requests) {
          if (request.owner_simulation_id ==
              std::numeric_limits<uint64_t>::max())
            bootstrap_callbacks.fetch_add(1, std::memory_order_relaxed);
          else
            request_ids[worker].push_back(request.owner_simulation_id);
        }
      }
      return evaluate_batch(requests);
    };
  };

  const RootParallelResult result = run_root_parallel(
      config, Game(2025), budget, workers, options, factory, 1.0f);
  require(result.workers.size() == workers,
          "root-parallel omitted worker results");
  require(result.duplicate_root_evaluations_avoided == workers - 1,
          "root-parallel bootstrap de-dup metric is wrong");
  require(bootstrap_callbacks.load(std::memory_order_relaxed) == 1,
          "root-parallel evaluated the root more than once");
  require(result.merged.stop_reason == SearchStopReason::Completed &&
              !result.merged.partial && result.merged.ledger.issued == budget &&
              result.merged.ledger.completed() == budget,
          "root-parallel merged ledger does not match the exact budget");
  const uint64_t merged_visits = std::accumulate(
      result.merged.visits.begin(), result.merged.visits.end(), uint64_t{0});
  require(merged_visits == budget,
          "root-parallel merged visits do not match the exact budget");

  uint64_t range_begin = base;
  for (uint32_t worker = 0; worker < workers; ++worker) {
    const uint64_t worker_budget =
        budget / workers + (worker < budget % workers ? 1 : 0);
    const uint64_t range_end = range_begin + worker_budget;
    require(result.workers[worker].ledger.issued == worker_budget &&
                result.workers[worker].ledger.completed() == worker_budget,
            "root-parallel worker budget split is incorrect");
    require(!request_ids[worker].empty(),
            "root-parallel worker produced no inference request IDs");
    for (uint64_t simulation_id : request_ids[worker])
      require(simulation_id >= range_begin && simulation_id < range_end,
              "root-parallel worker used another worker's seed range");
    range_begin = range_end;
  }

  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    uint64_t visits = 0;
    long double weighted_q = 0.0L;
    for (const auto &worker : result.workers) {
      visits += worker.visits[action];
      weighted_q += static_cast<long double>(worker.q_values[action]) *
                    static_cast<long double>(worker.visits[action]);
    }
    require(result.merged.visits[action] == visits,
            "root-parallel action visits were not summed");
    const double expected =
        visits == 0 ? 0.0
                    : static_cast<double>(weighted_q /
                                          static_cast<long double>(visits));
    require(std::abs(result.merged.q_values[action] - expected) <= 1e-15,
            "root-parallel Q was not visit-weighted");
  }
}

void test_root_parallel_joins_every_worker_before_rethrow() {
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  options.batch_size = 1;
  options.deterministic_epoch_size = 1;
  std::atomic<bool> slow_started{false};
  std::atomic<bool> slow_finished{false};
  std::atomic<bool> worker_failed{false};

  ParallelEvaluatorFactory factory =
      [&](uint32_t worker) -> ParallelInferenceFunction {
    return [&, worker](const std::vector<ParallelInferenceRequest> &requests) {
      const bool bootstrap =
          requests.size() == 1 && requests.front().owner_simulation_id ==
                                      std::numeric_limits<uint64_t>::max();
      if (!bootstrap && worker == 0) {
        slow_started.store(true, std::memory_order_release);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!worker_failed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        slow_finished.store(true, std::memory_order_release);
      }
      if (!bootstrap && worker == 1) {
        // Ensure the sibling has actually entered user code before injecting
        // the failure. Without this rendezvous worker 1 can cancel the session
        // before worker 0 is scheduled, which tests launch order rather than
        // the promised join-before-rethrow behavior.
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (!slow_started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::yield();
        if (!slow_started.load(std::memory_order_acquire))
          throw std::runtime_error("root worker rendezvous timed out");
        worker_failed.store(true, std::memory_order_release);
        throw std::runtime_error("intentional root worker failure");
      }
      return evaluate_batch(requests);
    };
  };

  bool preserved = false;
  try {
    (void)run_root_parallel(config, Game(777), 24, 3, options, factory);
  } catch (const std::runtime_error &error) {
    preserved = std::string(error.what()) == "intentional root worker failure";
  }
  require(preserved, "root-parallel did not preserve the worker exception");
  require(slow_started.load(std::memory_order_acquire) &&
              slow_finished.load(std::memory_order_acquire),
          "root-parallel rethrew before joining a slow sibling worker");
}

void test_root_parallel_budget_smaller_than_worker_count() {
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  config.num_simulations = 800;
  ParallelSearchOptions options = replay_options(1);
  options.num_simulations = 0;
  std::atomic<uint64_t> callback_count{0};
  std::atomic<uint64_t> factory_count{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t) -> ParallelInferenceFunction {
    factory_count.fetch_add(1, std::memory_order_relaxed);
    return [&](const std::vector<ParallelInferenceRequest> &requests) {
      callback_count.fetch_add(1, std::memory_order_relaxed);
      return evaluate_batch(requests);
    };
  };

  const auto zero = run_root_parallel(config, Game(12), 0, 4, options, factory);
  require(zero.merged.ledger.issued == 0 &&
              zero.merged.ledger.completed() == 0 &&
              callback_count.load(std::memory_order_relaxed) == 0 &&
              factory_count.load(std::memory_order_relaxed) == 0,
          "zero-budget root-parallel search performed setup or simulations");

  const auto one = run_root_parallel(config, Game(12), 1, 4, options, factory);
  require(one.merged.ledger.issued == 1 && one.merged.ledger.completed() == 1 &&
              std::accumulate(one.merged.visits.begin(),
                              one.merged.visits.end(), uint64_t{0}) == 1,
          "zero-budget workers fell back to config.num_simulations");
  require(one.workers[0].ledger.issued == 1 &&
              one.workers[1].ledger.issued == 0 &&
              one.workers[2].ledger.issued == 0 &&
              one.workers[3].ledger.issued == 0 &&
              one.duplicate_root_evaluations_avoided == 0 &&
              factory_count.load(std::memory_order_relaxed) == 1,
          "root-parallel assigned work to a zero-budget worker");
}

void test_root_parallel_enforces_aggregate_tree_capacity() {
  constexpr uint32_t workers = 4;
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  options.max_tree_nodes = workers;
  options.deterministic_epoch_size = 4;
  std::atomic<uint64_t> factory_calls{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t) -> ParallelInferenceFunction {
    factory_calls.fetch_add(1, std::memory_order_relaxed);
    return evaluate_batch;
  };

  const auto bounded =
      run_root_parallel(config, Game(1212), 64, workers, options, factory);
  require(bounded.merged.partial &&
              bounded.merged.stop_reason ==
                  SearchStopReason::TreeCapacityReached &&
              bounded.merged.tree_size <= options.max_tree_nodes,
          "root-parallel exceeded its aggregate tree capacity");
  uint64_t summed_tree_size = 0;
  for (const auto &worker : bounded.workers) {
    summed_tree_size += worker.tree_size;
    require(worker.tree_size <= 1 && worker.ledger.virtual_loss_balanced(),
            "root worker exceeded its divided capacity or leaked VL");
  }
  require(summed_tree_size == bounded.merged.tree_size &&
              factory_calls.load(std::memory_order_relaxed) == workers,
          "root aggregate capacity accounting is inconsistent");

  options.max_tree_nodes = workers - 1;
  factory_calls.store(0, std::memory_order_relaxed);
  require(throws_as<std::invalid_argument>([&] {
            (void)run_root_parallel(config, Game(1213), 64, workers, options,
                                    factory);
          }),
          "root-parallel accepted less than one node per active worker");
  require(factory_calls.load(std::memory_order_relaxed) == 0,
          "invalid root aggregate capacity invoked user code");
}

void test_root_parallel_terminal_and_budget_overflow_are_side_effect_free() {
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  std::atomic<uint64_t> factory_calls{0};
  std::atomic<uint64_t> callback_calls{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t) -> ParallelInferenceFunction {
    factory_calls.fetch_add(1, std::memory_order_relaxed);
    return [&](const std::vector<ParallelInferenceRequest> &requests) {
      callback_calls.fetch_add(1, std::memory_order_relaxed);
      return evaluate_batch(requests);
    };
  };

  Game terminal(13);
  terminal.board.winner = 0;
  const auto terminal_result =
      run_root_parallel(config, terminal, 16, 4, options, factory);
  require(terminal_result.workers.size() == 4 &&
              terminal_result.merged.stop_reason ==
                  SearchStopReason::Completed &&
              terminal_result.merged.ledger.issued == 0 &&
              terminal_result.merged.tree_size == 0 &&
              factory_calls.load(std::memory_order_relaxed) == 0 &&
              callback_calls.load(std::memory_order_relaxed) == 0,
          "terminal root invoked a factory/callback or issued work");

  options.simulation_id_base = std::numeric_limits<uint64_t>::max() - 1;
  require(throws_as<std::overflow_error>([&] {
            (void)run_root_parallel(config, Game(14), 2, 4, options, factory);
          }),
          "root-parallel accepted a wrapping simulation ID budget");
  require(factory_calls.load(std::memory_order_relaxed) == 0 &&
              callback_calls.load(std::memory_order_relaxed) == 0,
          "overflowing root budget invoked user code");
}

void test_root_parallel_factory_time_counts_toward_end_to_end_timeout() {
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  options.timeout_ms = 1;
  std::atomic<uint64_t> factory_calls{0};
  std::atomic<uint64_t> callback_calls{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t) -> ParallelInferenceFunction {
    factory_calls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    return [&](const std::vector<ParallelInferenceRequest> &requests) {
      callback_calls.fetch_add(1, std::memory_order_relaxed);
      return evaluate_batch(requests);
    };
  };

  const RootParallelResult result =
      run_root_parallel(config, Game(1616), 16, 4, options, factory);
  require(result.merged.stop_reason == SearchStopReason::TimedOut &&
              result.merged.partial && result.merged.ledger.issued == 0 &&
              result.merged.ledger.completed() == 0 &&
              result.merged.elapsed_microseconds >= 1000,
          "root-parallel factory timeout metadata is inconsistent");
  require(factory_calls.load(std::memory_order_relaxed) == 1 &&
              callback_calls.load(std::memory_order_relaxed) == 0,
          "root-parallel continued setup after its factory timeout");
  for (const auto &worker : result.workers)
    require(worker.ledger.issued == 0 && worker.ledger.completed() == 0,
            "root-parallel factory timeout issued logical work");
}

void test_root_parallel_factory_serialization_and_sharded_generation() {
  constexpr uint32_t workers = 4;
  MCTSConfig config = replay_config();
  config.use_dirichlet_noise = false;
  ParallelSearchOptions options = replay_options(1);
  options.tree_backend = TreeBackend::Sharded;
  options.shard_count = 16;
  options.batch_size = 2;
  options.deterministic_epoch_size = 2;

  std::array<uint32_t, workers> factory_calls{};
  std::array<std::atomic<uint64_t>, workers> callback_calls{};
  std::atomic<int> active_factories{0};
  std::atomic<int> max_active_factories{0};
  std::atomic<bool> caller_thread_only{true};
  const std::thread::id caller_thread = std::this_thread::get_id();
  ParallelEvaluatorFactory factory =
      [&](uint32_t worker) -> ParallelInferenceFunction {
    const int active =
        active_factories.fetch_add(1, std::memory_order_acq_rel) + 1;
    int observed = max_active_factories.load(std::memory_order_relaxed);
    while (observed < active && !max_active_factories.compare_exchange_weak(
                                    observed, active, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
    }
    if (std::this_thread::get_id() != caller_thread)
      caller_thread_only.store(false, std::memory_order_release);
    ++factory_calls.at(worker);
    // Make an accidental concurrent factory implementation observable.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    active_factories.fetch_sub(1, std::memory_order_acq_rel);
    return [&, worker](const std::vector<ParallelInferenceRequest> &requests) {
      callback_calls[worker].fetch_add(1, std::memory_order_relaxed);
      return evaluate_batch(requests);
    };
  };

  const auto result =
      run_root_parallel(config, Game(15), 12, workers, options, factory);
  require(max_active_factories.load(std::memory_order_relaxed) == 1 &&
              active_factories.load(std::memory_order_relaxed) == 0 &&
              caller_thread_only.load(std::memory_order_acquire),
          "root-parallel evaluator factories were not serialized");
  for (uint32_t worker = 0; worker < workers; ++worker) {
    require(factory_calls[worker] == 1 &&
                callback_calls[worker].load(std::memory_order_relaxed) >= 1,
            "root-parallel did not construct/use one evaluator per worker");
    require(result.workers[worker].tree_generation == 1,
            "sharded root worker reported an unexpected generation");
  }
  require(result.merged.tree_generation == 1 &&
              result.duplicate_root_evaluations_avoided == workers - 1 &&
              result.merged.ledger.issued == 12 &&
              result.merged.ledger.completed() == 12,
          "sharded root-parallel merge lost generation or budget metadata");
}

} // namespace

int main() {
  try {
    run_named("deterministic backend invariant", [] {
      test_deterministic_epoch_trace_is_thread_count_and_backend_invariant();
    });
    run_named("10,000 tuple seed manifest", [] {
      test_derive_seed_10000_tuple_manifest_is_stable_and_collision_free();
    });
    run_named("trace binary roundtrip",
              [] { test_trace_binary_roundtrip_and_rejection(); });
    run_named("root budget and seed ranges",
              [] { test_root_parallel_budget_merge_and_seed_ranges(); });
    run_named("root worker join",
              [] { test_root_parallel_joins_every_worker_before_rethrow(); });
    run_named("root small budget",
              [] { test_root_parallel_budget_smaller_than_worker_count(); });
    run_named("root aggregate capacity",
              [] { test_root_parallel_enforces_aggregate_tree_capacity(); });
    run_named("root side-effect-free errors", [] {
      test_root_parallel_terminal_and_budget_overflow_are_side_effect_free();
    });
    run_named("root factory timeout", [] {
      test_root_parallel_factory_time_counts_toward_end_to_end_timeout();
    });
    run_named("root factory serialization", [] {
      test_root_parallel_factory_serialization_and_sharded_generation();
    });
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mcts_parallel_replay: " << error.what() << '\n';
    return 1;
  }
}
