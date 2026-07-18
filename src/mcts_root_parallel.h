#ifndef CSPLENDOR_MCTS_ROOT_PARALLEL_H
#define CSPLENDOR_MCTS_ROOT_PARALLEL_H

#include "mcts_parallel_searcher.h"

#include <cmath>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace mcts_parallel {

using ParallelEvaluatorFactory =
    std::function<ParallelInferenceFunction(uint32_t worker_id)>;

struct RootParallelResult {
  ParallelSearchResult merged{};
  std::vector<ParallelSearchResult> workers;
  uint64_t duplicate_root_evaluations_avoided = 0;
};

inline void add_ledger(SearchLedgerSnapshot &target,
                       const SearchLedgerSnapshot &source) noexcept {
#define CSPLENDOR_ADD_LEDGER(name) target.name += source.name
  CSPLENDOR_ADD_LEDGER(issued);
  CSPLENDOR_ADD_LEDGER(selected);
  CSPLENDOR_ADD_LEDGER(evaluation_owner);
  CSPLENDOR_ADD_LEDGER(evaluation_waiter);
  CSPLENDOR_ADD_LEDGER(evaluation_requested);
  CSPLENDOR_ADD_LEDGER(evaluated_boards);
  CSPLENDOR_ADD_LEDGER(completed_evaluated);
  CSPLENDOR_ADD_LEDGER(completed_terminal);
  CSPLENDOR_ADD_LEDGER(completed_max_depth);
  CSPLENDOR_ADD_LEDGER(cancelled);
  CSPLENDOR_ADD_LEDGER(failed);
  CSPLENDOR_ADD_LEDGER(virtual_loss_added);
  CSPLENDOR_ADD_LEDGER(virtual_loss_released);
  CSPLENDOR_ADD_LEDGER(reservations_committed);
  CSPLENDOR_ADD_LEDGER(reservations_aborted);
  CSPLENDOR_ADD_LEDGER(expansion_claimed);
  CSPLENDOR_ADD_LEDGER(expansion_published);
  CSPLENDOR_ADD_LEDGER(expansion_waited);
  CSPLENDOR_ADD_LEDGER(stale_result);
  CSPLENDOR_ADD_LEDGER(duplicate_result);
  CSPLENDOR_ADD_LEDGER(invalid_replay);
  CSPLENDOR_ADD_LEDGER(integrity_errors);
  target.max_inflight_observed =
      std::max(target.max_inflight_observed, source.max_inflight_observed);
#undef CSPLENDOR_ADD_LEDGER
}

inline int root_stop_reason_priority(SearchStopReason reason) noexcept {
  switch (reason) {
  case SearchStopReason::Completed:
    return 0;
  case SearchStopReason::Cancelled:
    return 1;
  case SearchStopReason::TimedOut:
    return 2;
  case SearchStopReason::TreeCapacityReached:
    return 3;
  case SearchStopReason::CallbackError:
    return 4;
  case SearchStopReason::WorkerError:
    return 5;
  }
  return 5;
}

inline RootParallelResult
run_root_parallel(const MCTSConfig &config, const Game &root_game,
                  uint64_t simulation_budget, uint32_t num_workers,
                  ParallelSearchOptions options,
                  const ParallelEvaluatorFactory &evaluator_factory,
                  float temperature = 1.0f) {
  const auto started = std::chrono::steady_clock::now();
  options.reset_session_stop();
  if (num_workers == 0 || num_workers > 256)
    throw std::invalid_argument(
        "root-parallel worker count must be in [1, 256]");
  if (!evaluator_factory)
    throw std::invalid_argument("root-parallel evaluator factory is empty");
  const MCTSConfig config_snapshot = config;
  if (!std::isfinite(temperature) || temperature < 0.0f)
    throw std::invalid_argument(
        "root-parallel temperature must be finite and non-negative");
  if (options.batch_size == 0 || options.batch_size > 1048576 ||
      options.deterministic_epoch_size == 0 ||
      options.deterministic_epoch_size > 1048576 ||
      options.max_inflight > 1048576)
    throw std::invalid_argument("root-parallel search window is out of range");
  if (options.batch_wait_us > 1000000)
    throw std::invalid_argument("root-parallel batch wait is out of range");
  if (options.shard_count == 0 || options.shard_count > 4096)
    throw std::invalid_argument("root-parallel shard count is out of range");
  if (options.timeout_ms >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    throw std::invalid_argument("root-parallel timeout is out of range");
  const uint64_t maximum_aggregate_nodes =
      static_cast<uint64_t>(MAX_TREE_SIZE) * num_workers;
  if (options.max_tree_nodes == 0 ||
      options.max_tree_nodes > maximum_aggregate_nodes)
    throw std::invalid_argument(
        "root-parallel aggregate tree capacity is out of range");
  if (options.tree_backend != TreeBackend::Coarse &&
      options.tree_backend != TreeBackend::Sharded)
    throw std::invalid_argument("unknown root-parallel tree backend");
  if (simulation_budget >
      std::numeric_limits<uint64_t>::max() - options.simulation_id_base)
    throw std::overflow_error("root-parallel simulation ID range wraps");
  if (simulation_budget > 0 &&
      options.search_nonce == std::numeric_limits<uint64_t>::max())
    throw std::invalid_argument(
        "root-parallel requires an explicit search nonce");
  if (simulation_budget > 0 &&
      mcts_internal::GameAdapter::requires_forced_pass(root_game))
    throw std::invalid_argument(
        "MCTS root requires a forced pass; apply it before searching");
  ParallelMCTSSearcher::validate_config_snapshot(config_snapshot);
  const uint64_t resolved_master_seed =
      options.master_seed ? *options.master_seed : resolve_mcts_entropy_seed();
  // Every worker must share the coordinator-resolved identity; workers never
  // consult random_device independently.
  options.master_seed = resolved_master_seed;

  const auto deadline = [&] {
    if (options.timeout_ms == 0)
      return std::chrono::steady_clock::time_point::max();
    const auto remaining =
        std::chrono::steady_clock::time_point::max() - started;
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    const auto requested =
        std::chrono::milliseconds(static_cast<int64_t>(options.timeout_ms));
    if (requested >= remaining_ms)
      return std::chrono::steady_clock::time_point::max();
    return started +
           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
               requested);
  }();

  RootParallelResult output;
  output.workers.resize(num_workers);
  auto finish_setup_stop = [&](SearchStopReason reason) {
    output.merged.stop_reason = reason;
    output.merged.partial = true;
    output.merged.resolved_seed = resolved_master_seed;
    output.merged.search_nonce = options.search_nonce;
    output.merged.elapsed_microseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    return std::move(output);
  };
  if (simulation_budget == 0) {
    output.merged.stop_reason = SearchStopReason::Completed;
    output.merged.resolved_seed = resolved_master_seed;
    output.merged.search_nonce =
        options.search_nonce == std::numeric_limits<uint64_t>::max()
            ? 0
            : options.search_nonce;
    output.merged.elapsed_microseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    return output;
  }
  if (options.cancellation_requested())
    return finish_setup_stop(SearchStopReason::Cancelled);
  Game root_snapshot = mcts_internal::GameAdapter::clone_light(root_game);
  if (mcts_internal::GameAdapter::is_terminal(root_snapshot)) {
    output.merged.stop_reason = SearchStopReason::Completed;
    output.merged.resolved_seed = resolved_master_seed;
    output.merged.search_nonce =
        options.search_nonce == std::numeric_limits<uint64_t>::max()
            ? 0
            : options.search_nonce;
    output.merged.elapsed_microseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    return output;
  }
  const uint32_t active_workers =
      static_cast<uint32_t>(std::min<uint64_t>(num_workers, simulation_budget));
  if (options.max_tree_nodes < active_workers)
    throw std::invalid_argument(
        "root-parallel aggregate tree capacity cannot hold every active root");
  const uint64_t effective_aggregate_nodes =
      std::min<uint64_t>(options.max_tree_nodes,
                         static_cast<uint64_t>(MAX_TREE_SIZE) * active_workers);
  const uint64_t capacity_quotient = effective_aggregate_nodes / active_workers;
  const uint64_t capacity_remainder =
      effective_aggregate_nodes % active_workers;
  std::vector<uint64_t> worker_tree_capacities(num_workers, 0);
  for (uint32_t worker = 0; worker < active_workers; ++worker)
    worker_tree_capacities[worker] =
        capacity_quotient + (worker < capacity_remainder ? 1 : 0);
  std::vector<std::unique_ptr<MCTS>> trees;
  trees.reserve(num_workers);
  for (uint32_t worker = 0; worker < num_workers; ++worker)
    trees.emplace_back(
        worker < active_workers
            ? std::make_unique<MCTS>(config_snapshot, resolved_master_seed)
            : nullptr);

  // Factories are user code and are not required to be thread-safe. Build
  // every active evaluator serially before launching any worker, and reuse
  // worker zero's evaluator for the common root bootstrap.
  std::vector<ParallelInferenceFunction> evaluators(num_workers);
  for (uint32_t worker = 0; worker < active_workers; ++worker) {
    evaluators[worker] = evaluator_factory(worker);
    if (std::chrono::steady_clock::now() >= deadline)
      return finish_setup_stop(SearchStopReason::TimedOut);
    if (options.cancellation_requested())
      return finish_setup_stop(SearchStopReason::Cancelled);
    if (!evaluators[worker])
      throw std::invalid_argument("root-parallel evaluator is empty");
  }

  // Bootstrap is setup, not a logical simulation. Evaluate it once and feed
  // the same unmasked policy/value to every independent worker tree.
  const uint8_t observer = static_cast<uint8_t>(
      mcts_internal::GameAdapter::current_player(root_snapshot));
  const TreeKey root_key = mcts_internal::GameAdapter::tree_key(
      root_snapshot, observer, config_snapshot.use_determinization);
  ParallelInferenceRequest bootstrap_request;
  bootstrap_request.pending_id = std::numeric_limits<uint64_t>::max();
  bootstrap_request.owner_simulation_id = std::numeric_limits<uint64_t>::max();
  bootstrap_request.key = root_key;
  bootstrap_request.features =
      mcts_internal::GameAdapter::native_features(root_snapshot, observer);
  bootstrap_request.owner_world_mask =
      mcts_internal::GameAdapter::native_action_mask(root_snapshot);
  auto bootstrap_results =
      evaluators[0](std::vector<ParallelInferenceRequest>{bootstrap_request});
  if (std::chrono::steady_clock::now() >= deadline)
    return finish_setup_stop(SearchStopReason::TimedOut);
  if (options.cancellation_requested())
    return finish_setup_stop(SearchStopReason::Cancelled);
  if (bootstrap_results.size() != 1)
    throw std::invalid_argument(
        "root-parallel bootstrap must return one result");
  validate_policy(bootstrap_results[0].policy);
  checked_value(bootstrap_results[0].value);
  const ParallelInferenceResult bootstrap = bootstrap_results[0];

  // Root-parallel timeout is an end-to-end budget.  Factory construction and
  // the shared bootstrap consume it; workers receive only the remaining
  // whole milliseconds.  Callback execution remains cooperatively timed, as
  // in the shared-tree runner, and is checked immediately after returning.
  if (options.timeout_ms != 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      return finish_setup_stop(SearchStopReason::TimedOut);
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();
    if (remaining_ms <= 0)
      return finish_setup_stop(SearchStopReason::TimedOut);
    options.timeout_ms = static_cast<uint64_t>(remaining_ms);
  }

  std::vector<std::thread> threads;
  threads.reserve(num_workers);
  std::mutex error_mutex;
  std::mutex serialized_callback_mutex;
  std::exception_ptr first_error;
  uint64_t range_begin = options.simulation_id_base;
  const uint64_t quotient = simulation_budget / num_workers;
  const uint64_t remainder = simulation_budget % num_workers;

  try {
    for (uint32_t worker = 0; worker < num_workers; ++worker) {
      const uint64_t worker_budget = quotient + (worker < remainder ? 1 : 0);
      const uint64_t worker_base = range_begin;
      range_begin += worker_budget;
      if (worker_budget == 0)
        continue;
      threads.emplace_back(
          [&, worker, worker_budget, worker_base,
           local_evaluator = std::move(evaluators[worker])]() mutable {
            try {
              auto evaluator =
                  [&, local_evaluator = std::move(local_evaluator)](
                      const std::vector<ParallelInferenceRequest> &requests) {
                    if (requests.size() == 1 &&
                        requests[0].owner_simulation_id ==
                            std::numeric_limits<uint64_t>::max() &&
                        requests[0].key == root_key)
                      return std::vector<ParallelInferenceResult>{bootstrap};
                    if (options.serialize_root_callbacks) {
                      std::unique_lock<std::mutex> callback_lock(
                          serialized_callback_mutex);
                      // A worker may have waited behind a slow Python callback.
                      // Re-check after the lock, otherwise every stale waiter
                      // would still enter Python after timeout/cancel/failure.
                      if (options.cancellation_requested())
                        throw detail::CancellationReached{};
                      if (std::chrono::steady_clock::now() >= deadline)
                        throw detail::TimeoutReached{};
                      try {
                        return local_evaluator(requests);
                      } catch (...) {
                        // Publish failure before releasing the serialization
                        // mutex so queued Python workers observe the session
                        // stop instead of entering a stale callback one by one.
                        options.request_session_stop();
                        throw;
                      }
                    }
                    return local_evaluator(requests);
                  };
              ParallelSearchOptions local = options;
              local.mode = ParallelSearchMode::DeterministicEpoch;
              local.num_threads = 1;
              local.num_simulations = worker_budget;
              local.max_tree_nodes = worker_tree_capacities[worker];
              local.simulation_id_base = worker_base;
              // Every worker uses the same search nonce/noise, while disjoint
              // logical simulation IDs keep all determinization streams unique.
              if (local.search_nonce == std::numeric_limits<uint64_t>::max())
                local.search_nonce = 0;
              ParallelMCTSSearcher searcher;
              auto worker_result = searcher.run(*trees[worker], root_snapshot,
                                                local, evaluator, temperature);
              if (worker_result.stop_reason == SearchStopReason::TimedOut ||
                  worker_result.stop_reason == SearchStopReason::Cancelled)
                options.request_session_stop();
              output.workers[worker] = std::move(worker_result);
            } catch (...) {
              options.request_session_stop();
              std::lock_guard<std::mutex> lock(error_mutex);
              if (!first_error)
                first_error = std::current_exception();
            }
          });
    }
  } catch (...) {
    options.request_session_stop();
    for (auto &thread : threads) {
      if (thread.joinable())
        thread.join();
    }
    throw;
  }
  for (auto &thread : threads)
    thread.join();
  if (first_error)
    std::rethrow_exception(first_error);

  output.duplicate_root_evaluations_avoided = active_workers - 1;
  output.merged.stop_reason = SearchStopReason::Completed;
  output.merged.resolved_seed = resolved_master_seed;
  output.merged.search_nonce =
      options.search_nonce == std::numeric_limits<uint64_t>::max()
          ? 0
          : options.search_nonce;
  bool has_generation = false;
  bool generation_mismatch = false;
  for (uint32_t worker_index = 0; worker_index < active_workers;
       ++worker_index) {
    const auto &worker = output.workers[worker_index];
    if (!has_generation) {
      output.merged.tree_generation = worker.tree_generation;
      has_generation = true;
    } else if (output.merged.tree_generation != worker.tree_generation) {
      generation_mismatch = true;
      // A worker that observes cancellation before preparing the requested
      // backend legitimately retains generation zero, while a sibling that
      // already prepared a sharded tree reports generation one. Preserve the
      // most advanced generation in a partial result; fully completed workers
      // must still agree below.
      output.merged.tree_generation =
          std::max(output.merged.tree_generation, worker.tree_generation);
    }
    add_ledger(output.merged.ledger, worker.ledger);
    output.merged.tree_size += worker.tree_size;
    output.merged.elapsed_microseconds = std::max(
        output.merged.elapsed_microseconds, worker.elapsed_microseconds);
    output.merged.partial = output.merged.partial || worker.partial;
    if (root_stop_reason_priority(worker.stop_reason) >
        root_stop_reason_priority(output.merged.stop_reason))
      output.merged.stop_reason = worker.stop_reason;
    for (size_t action = 0; action < MAX_ACTIONS; ++action) {
      const uint64_t old_visits = output.merged.visits[action];
      const uint64_t added_visits = worker.visits[action];
      if (added_visits > std::numeric_limits<uint64_t>::max() - old_visits)
        throw std::overflow_error("root-parallel visit merge overflow");
      const uint64_t total_visits = old_visits + added_visits;
      if (total_visits > 0) {
        const long double weighted =
            static_cast<long double>(output.merged.q_values[action]) *
                static_cast<long double>(old_visits) +
            static_cast<long double>(worker.q_values[action]) *
                static_cast<long double>(added_visits);
        output.merged.q_values[action] = static_cast<double>(
            weighted / static_cast<long double>(total_visits));
      }
      output.merged.visits[action] = total_visits;
    }
  }
  if (generation_mismatch && !output.merged.partial &&
      output.merged.stop_reason == SearchStopReason::Completed)
    throw std::logic_error(
        "root-parallel workers reported different tree generations");
  output.merged.probabilities =
      detail::probabilities_from_visits(output.merged.visits, temperature);
  output.merged.elapsed_microseconds = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started)
          .count());
  return output;
}

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_ROOT_PARALLEL_H
