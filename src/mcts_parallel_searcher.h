#ifndef CSPLENDOR_MCTS_PARALLEL_SEARCHER_H
#define CSPLENDOR_MCTS_PARALLEL_SEARCHER_H

#include "mcts_bounded_queue.h"
#include "mcts_config_validator.h"
#include "mcts_game_adapter.h"
#include "mcts_parallel_session.h"
#include "mcts_parallel_trace.h"
#include "mcts_tree.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcts_parallel {

using ParallelInferenceFunction =
    std::function<std::vector<ParallelInferenceResult>(
        const std::vector<ParallelInferenceRequest> &)>;

namespace detail {

inline uint64_t
feature_digest(const std::array<float, FEATURE_SIZE> &features) noexcept {
  uint64_t hash = 1469598103934665603ULL;
  for (float value : features) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= static_cast<uint8_t>(bits >> shift);
      hash *= 1099511628211ULL;
    }
  }
  return hash;
}

inline double uniform_open(PortableRng &rng) noexcept {
  // 53 random bits mapped strictly inside (0, 1).
  constexpr double scale = 1.0 / 9007199254740992.0;
  return (static_cast<double>(rng.next_u64() >> 11) + 0.5) * scale;
}

inline double standard_normal(PortableRng &rng) noexcept {
  constexpr double two_pi = 6.283185307179586476925286766559;
  const double radius = std::sqrt(-2.0 * std::log(uniform_open(rng)));
  return radius * std::cos(two_pi * uniform_open(rng));
}

inline double gamma_sample(double shape, PortableRng &rng) {
  if (!(shape > 0.0) || !std::isfinite(shape))
    throw std::invalid_argument("Dirichlet alpha must be finite and positive");
  if (shape < 1.0) {
    return gamma_sample(shape + 1.0, rng) *
           std::pow(uniform_open(rng), 1.0 / shape);
  }
  const double d = shape - 1.0 / 3.0;
  const double c = 1.0 / std::sqrt(9.0 * d);
  for (;;) {
    const double normal = standard_normal(rng);
    const double factor = 1.0 + c * normal;
    if (factor <= 0.0)
      continue;
    const double cube = factor * factor * factor;
    const double uniform = uniform_open(rng);
    if (uniform < 1.0 - 0.0331 * normal * normal * normal * normal ||
        std::log(uniform) <
            0.5 * normal * normal + d * (1.0 - cube + std::log(cube)))
      return d * cube;
  }
}

inline Policy generate_root_noise(const ActionMask &mask, double alpha,
                                  uint64_t seed) {
  Policy noise{};
  std::array<double, MAX_ACTIONS> samples{};
  PortableRng rng(seed);
  double sum = 0.0;
  size_t legal_count = 0;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (!mask[action])
      continue;
    const double sample = gamma_sample(alpha, rng);
    samples[action] = sample;
    sum += sample;
    ++legal_count;
  }
  if (legal_count == 0)
    return noise;
  if (!(sum > 0.0) || !std::isfinite(sum)) {
    const float uniform = 1.0f / static_cast<float>(legal_count);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      noise[action] = mask[action] ? uniform : 0.0f;
    return noise;
  }

  double float_sum = 0.0;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (!mask[action])
      continue;
    noise[action] = static_cast<float>(samples[action] / sum);
    float_sum += static_cast<double>(noise[action]);
  }
  if (!(float_sum > 0.0) || !std::isfinite(float_sum))
    throw std::logic_error("Dirichlet normalization produced invalid noise");
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (mask[action])
      noise[action] = static_cast<float>(noise[action] / float_sum);
  }
  return noise;
}

inline std::array<float, MAX_ACTIONS>
probabilities_from_visits(const std::array<uint64_t, MAX_ACTIONS> &visits,
                          float temperature) {
  std::array<float, MAX_ACTIONS> probabilities{};
  if (temperature < EPS) {
    const uint64_t best = *std::max_element(visits.begin(), visits.end());
    if (best == 0)
      return probabilities;
    size_t count = 0;
    for (uint64_t visit : visits)
      count += visit == best ? 1 : 0;
    const float probability = 1.0f / static_cast<float>(count);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      probabilities[action] = visits[action] == best ? probability : 0.0f;
    return probabilities;
  }

  long double sum = 0.0;
  const long double exponent = 1.0L / static_cast<long double>(temperature);
  std::array<long double, MAX_ACTIONS> weights{};
  const uint64_t max_visits = *std::max_element(visits.begin(), visits.end());
  if (max_visits == 0)
    return probabilities;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (visits[action] == 0)
      continue;
    // Scaling by the maximum keeps the base in (0, 1], so even an extremely
    // small positive temperature can only underflow losing actions to zero;
    // it cannot overflow winning actions to inf and produce inf/inf NaNs.
    weights[action] = std::pow(static_cast<long double>(visits[action]) /
                                   static_cast<long double>(max_visits),
                               exponent);
    sum += weights[action];
  }
  if (sum > 0.0) {
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      probabilities[action] = static_cast<float>(weights[action] / sum);
  }
  return probabilities;
}

struct RootSnapshot {
  RootSnapshot(Game game, MCTSConfig frozen_config, uint8_t root_observer,
               TreeKey key, uint64_t generation,
               SearchRandomContext random_context)
      : root(std::move(game)), config(frozen_config), observer(root_observer),
        root_key(key), tree_generation(generation),
        random(std::move(random_context)) {}

  Game root;
  const MCTSConfig config;
  const uint8_t observer;
  const TreeKey root_key;
  const uint64_t tree_generation;
  const SearchRandomContext random;
  Policy root_noise{};
  bool has_root_noise = false;
};

inline uint64_t root_dirichlet_seed(const RootSnapshot &root) {
  // Noise belongs to the public decision point, not to the exact/observable
  // storage-domain choice. This keeps the root-noise stream invariant when
  // determinization is toggled for the same root snapshot.
  const TreeKey public_key =
      mcts_internal::GameAdapter::tree_key(root.root, root.observer, true);
  return derive_search_seed(root.random.resolved_master_seed, public_key,
                            root.random.search_nonce,
                            SearchRandomDomain::RootDirichlet, 0);
}

inline Policy probabilities_from_root_prior(const MCTSNodeSnapshot64 &snapshot,
                                            const RootSnapshot &root) {
  Policy probabilities{};
  size_t legal_count = 0;
  double prior_sum = 0.0;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (!snapshot.valid_actions[action])
      continue;
    ++legal_count;
    prior_sum += static_cast<double>(snapshot.base_policy[action]);
  }
  if (legal_count == 0)
    return probabilities;

  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (!snapshot.valid_actions[action])
      continue;
    const double prior =
        prior_sum > 0.0
            ? static_cast<double>(snapshot.base_policy[action]) / prior_sum
            : 1.0 / static_cast<double>(legal_count);
    probabilities[action] = static_cast<float>(
        root.has_root_noise
            ? (1.0 - root.config.dirichlet_epsilon) * prior +
                  root.config.dirichlet_epsilon *
                      static_cast<double>(root.root_noise[action])
            : prior);
  }

  double probability_sum = 0.0;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (snapshot.valid_actions[action])
      probability_sum += static_cast<double>(probabilities[action]);
  }
  if (!(probability_sum > 0.0) || !std::isfinite(probability_sum)) {
    const float uniform = 1.0f / static_cast<float>(legal_count);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      probabilities[action] = snapshot.valid_actions[action] ? uniform : 0.0f;
    return probabilities;
  }
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (snapshot.valid_actions[action])
      probabilities[action] =
          static_cast<float>(probabilities[action] / probability_sum);
  }
  return probabilities;
}

struct TimeoutReached final {};
struct CancellationReached final {};

class InferenceCallbackFailure final {
public:
  explicit InferenceCallbackFailure(std::exception_ptr error) noexcept
      : error_(std::move(error)) {}

  const std::exception_ptr &error() const noexcept { return error_; }

private:
  std::exception_ptr error_;
};

inline std::vector<ParallelInferenceResult>
call_inference_callback(const ParallelInferenceFunction &inference,
                        const std::vector<ParallelInferenceRequest> &requests) {
  try {
    return inference(requests);
  } catch (const CancellationReached &) {
    throw;
  } catch (const TimeoutReached &) {
    throw;
  } catch (...) {
    // TreeCapacityReachedError is part of the public callback surface as well
    // as the internal tree surface. Keep the callback boundary explicit so a
    // user exception can never be converted into a partial-capacity result.
    throw InferenceCallbackFailure(std::current_exception());
  }
}

} // namespace detail

class ParallelMCTSSearcher {
public:
  static void validate_config_snapshot(const MCTSConfig &config) {
    mcts_internal::MCTSConfigValidator::validate_parallel(config);
  }

  static void
  validate_entry_options(const ParallelSearchOptions &options,
                         const ParallelInferenceFunction &inference,
                         float temperature,
                         const DeterministicTrace *trace = nullptr) {
    if (!inference)
      throw std::invalid_argument("parallel inference callback is empty");
    if (options.num_threads == 0 || options.num_threads > 256 ||
        options.batch_size == 0 || options.batch_size > 1048576)
      throw std::invalid_argument("thread and batch counts must be positive");
    if (options.batch_wait_us > 1000000)
      throw std::invalid_argument("parallel batch wait is out of range");
    if (options.max_inflight > 1048576 ||
        options.deterministic_epoch_size > 1048576)
      throw std::invalid_argument("parallel search window is out of range");
    if (options.shard_count == 0 || options.shard_count > 4096)
      throw std::invalid_argument("parallel shard count is out of range");
    if (!std::isfinite(temperature) || temperature < 0.0f)
      throw std::invalid_argument(
          "parallel search temperature must be finite and non-negative");
    if (options.timeout_ms >
        static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      throw std::invalid_argument("parallel search timeout is out of range");
    if (options.max_tree_nodes == 0 ||
        options.max_tree_nodes > static_cast<uint64_t>(MAX_TREE_SIZE))
      throw std::invalid_argument("parallel tree capacity is out of range");
    if (options.mode == ParallelSearchMode::RootParallel)
      throw std::invalid_argument(
          "root-parallel mode uses the dedicated root-parallel runner");
    if (options.mode != ParallelSearchMode::Throughput &&
        options.mode != ParallelSearchMode::DeterministicEpoch)
      throw std::invalid_argument("unknown parallel search mode");
    if (options.tree_backend != TreeBackend::Coarse &&
        options.tree_backend != TreeBackend::Sharded)
      throw std::invalid_argument("unknown parallel tree backend");
    if (options.mode == ParallelSearchMode::DeterministicEpoch &&
        options.deterministic_epoch_size == 0)
      throw std::invalid_argument("deterministic epoch size must be positive");
    if (trace && options.mode != ParallelSearchMode::DeterministicEpoch)
      throw std::invalid_argument(
          "event trace is available only in deterministic-epoch mode");
  }

  ParallelSearchResult run(MCTS &mcts, const Game &root_game,
                           ParallelSearchOptions options,
                           const ParallelInferenceFunction &inference,
                           float temperature = 1.0f,
                           DeterministicTrace *trace = nullptr) const {
    validate_entry_options(options, inference, temperature, trace);
    validate_config_snapshot(mcts.get_config_snapshot());
    if (options.num_simulations > 0 && !options.cancellation_requested() &&
        mcts_internal::GameAdapter::requires_forced_pass(root_game))
      throw std::invalid_argument(
          "MCTS root requires a forced pass; apply it before searching");
    auto guard = mcts.begin_parallel_search();
    return run_with_guard(mcts, std::move(guard), root_game, options, inference,
                          temperature, trace);
  }

  // The binding acquires this guard while it still owns the GIL. That makes
  // the config/generation snapshot linearize at Python API entry instead of
  // after the GIL has been released.
  ParallelSearchResult run_with_guard(
      MCTS &mcts, MCTS::SearchGuard guard, const Game &root_game,
      ParallelSearchOptions options, const ParallelInferenceFunction &inference,
      float temperature = 1.0f, DeterministicTrace *trace = nullptr) const {
    validate_entry_options(options, inference, temperature, trace);
    const MCTSConfig config = guard.config();
    mcts_internal::MCTSConfigValidator::validate_parallel(config);
    normalize_and_validate_options(options, config);
    if (trace && options.num_simulations > MCTS_PARALLEL_TRACE_MAX_EVENTS)
      throw std::length_error(
          "deterministic trace simulation budget exceeds safe limit");
    if (options.num_simulations == 0)
      return run_zero_budget(mcts, std::move(guard), root_game, options,
                             temperature, trace, SearchStopReason::Completed,
                             false);
    if (options.cancellation_requested())
      return run_zero_budget(mcts, std::move(guard), root_game, options,
                             temperature, trace, SearchStopReason::Cancelled,
                             true);
    if (mcts_internal::GameAdapter::requires_forced_pass(root_game))
      throw std::invalid_argument(
          "MCTS root requires a forced pass; apply it before searching");
    if (options.mode == ParallelSearchMode::DeterministicEpoch)
      return run_deterministic_epoch(mcts, std::move(guard), root_game, options,
                                     inference, temperature, trace);
    if (options.num_threads == 1) {
      // `num_threads == 1` is the documented low-overhead serial mode: it uses
      // the caller as traversal/coordinator instead of spawning one traversal
      // worker plus the coordinator. Throughput scheduling is intentionally
      // enabled only from two traversal workers; both modes preserve the
      // configured bounded in-flight window and callback batch limit.
      options.deterministic_epoch_size = options.max_inflight;
      return run_deterministic_epoch(mcts, std::move(guard), root_game, options,
                                     inference, temperature, nullptr);
    }
    const auto started = std::chrono::steady_clock::now();
    auto &tree = mcts.prepare_parallel_tree(
        guard, options.tree_backend, options.shard_count,
        static_cast<size_t>(options.max_tree_nodes));
    tree.bind_evaluator(options.evaluator_version);
    tree.validate_quiescent_for_search();

    const uint8_t observer = static_cast<uint8_t>(
        mcts_internal::GameAdapter::current_player(root_game));
    Game root_copy = mcts_internal::GameAdapter::clone_light(root_game);
    const TreeKey root_key = mcts_internal::GameAdapter::tree_key(
        root_copy, observer, config.use_determinization);
    const uint64_t nonce =
        options.search_nonce == std::numeric_limits<uint64_t>::max()
            ? guard.search_nonce()
            : options.search_nonce;
    const uint64_t master_seed =
        options.master_seed.value_or(guard.resolved_master_seed());
    SearchRandomContext random{master_seed, root_key, nonce,
                               options.simulation_id_base, MCTS_RNG_VERSION};
    detail::RootSnapshot root(std::move(root_copy), config, observer, root_key,
                              guard.tree_generation(), random);
    std::exception_ptr failure;
    SearchStopReason stop_reason = SearchStopReason::Completed;
    bool partial = false;

    const size_t queue_capacity =
        std::max<size_t>(1, static_cast<size_t>(options.max_inflight));
    detail::ParallelSessionController session(queue_capacity);
    const auto ledger = session.ledger();
    auto &active_tickets = session.active_tickets();
    auto &work_queue = session.work_queue();
    auto &event_queue = session.event_queue();
    auto &registry = session.registry();
    auto &worker_failure = session.worker_failure();
    auto &stop_requested = session.stop_requested();

    uint64_t issued = 0;
    uint64_t terminalized = 0;
    const auto deadline = search_deadline(started, options.timeout_ms);

    try {
      throw_if_cancelled_or_timed_out(options, deadline);
      if (options.num_simulations == 0) {
        auto result = make_result(tree, root, *ledger, stop_reason, partial,
                                  temperature, started);
        guard.finish();
        return result;
      }
      auto root_node = tree.find_or_create(root.root_key);
      bootstrap_root(tree, root, root_node, inference);
      throw_if_cancelled_or_timed_out(options, deadline);

      const auto root_snapshot = tree.snapshot(root_node);
      if (root_snapshot.state == ExpansionState::Terminal) {
        auto result = make_result(tree, root, *ledger, stop_reason, partial,
                                  temperature, started);
        guard.finish();
        return result;
      }

      if (config.use_dirichlet_noise) {
        root.root_noise = detail::generate_root_noise(
            root_snapshot.valid_actions, config.dirichlet_alpha,
            detail::root_dirichlet_seed(root));
        root.has_root_noise = true;
      }

      const uint32_t worker_count = static_cast<uint32_t>(std::min<uint64_t>(
          options.num_threads, std::max<uint64_t>(1, options.num_simulations)));
      session.reserve_workers(worker_count);
      for (uint32_t worker = 0; worker < worker_count; ++worker) {
        session.start_worker([&, worker] {
          worker_loop(worker, root, tree, work_queue, event_queue, registry,
                      ledger, options, stop_requested, worker_failure);
        });
      }

      auto issue_available = [&] {
        while (issued < options.num_simulations &&
               active_tickets.size() < options.max_inflight &&
               !stop_requested.load(std::memory_order_acquire) &&
               !options.cancellation_requested()) {
          const uint64_t simulation_id = options.simulation_id_base + issued;
          auto ticket = std::make_shared<detail::SimulationTicket>(
              simulation_id, root.tree_generation);
          const auto [ticket_entry, inserted] =
              active_tickets.emplace(simulation_id, ticket);
          if (!inserted)
            throw std::logic_error("parallel ticket identifier was reused");
          if (!work_queue.push(ticket)) {
            // A cooperative stop may close the queue while this producer is
            // blocked. The ticket was never issued, so it must not enter the
            // shutdown ledger as a cancelled ticket.
            active_tickets.erase(ticket_entry);
            break;
          }
          ++issued;
          ledger->issued.fetch_add(1, std::memory_order_relaxed);
          SearchLedger::update_max(ledger->max_inflight_observed,
                                   active_tickets.size());
        }
      };
      issue_available();

      while (terminalized < options.num_simulations) {
        throw_if_cancelled_or_timed_out(options, deadline);

        detail::WorkerEvent first;
        const auto first_wait = std::min(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::milliseconds(10)),
            deadline - std::chrono::steady_clock::now());
        if (!event_queue.pop_for(first, first_wait)) {
          if (options.cancellation_requested())
            throw detail::CancellationReached{};
          if (stop_requested.load(std::memory_order_acquire)) {
            if (auto worker_error = worker_failure.snapshot())
              std::rethrow_exception(worker_error);
            throw std::runtime_error("parallel worker stopped without event");
          }
          continue;
        }
        throw_if_cancelled_or_timed_out(options, deadline);
        std::vector<detail::WorkerEvent> owners;
        process_or_collect(first, owners, active_tickets, root, *ledger,
                           terminalized);

        const auto batch_deadline = std::min(
            deadline, std::chrono::steady_clock::now() +
                          std::chrono::microseconds(options.batch_wait_us));
        while (owners.size() < options.batch_size) {
          detail::WorkerEvent next;
          const auto now = std::chrono::steady_clock::now();
          if (now >= batch_deadline)
            break;
          const auto slice_deadline =
              std::min(batch_deadline, now + std::chrono::milliseconds(10));
          if (!event_queue.pop_for(next, slice_deadline - now)) {
            throw_if_cancelled_or_timed_out(options, deadline);
            if (stop_requested.load(std::memory_order_acquire)) {
              if (auto worker_error = worker_failure.snapshot())
                std::rethrow_exception(worker_error);
              break;
            }
            continue;
          }
          throw_if_cancelled_or_timed_out(options, deadline);
          process_or_collect(next, owners, active_tickets, root, *ledger,
                             terminalized);
        }

        if (!owners.empty()) {
          std::vector<ParallelInferenceRequest> requests;
          requests.reserve(owners.size());
          for (const auto &owner : owners)
            requests.push_back(owner.pending->request);

          // No tree, node, pending, or queue lock is held across this call.
          throw_if_cancelled_or_timed_out(options, deadline);
          auto results = detail::call_inference_callback(inference, requests);
          throw_if_cancelled_or_timed_out(options, deadline);
          if (results.size() != requests.size())
            throw std::invalid_argument(
                "parallel inference result count does not match requests");
          std::vector<Value> values;
          values.reserve(results.size());
          for (const auto &result : results) {
            validate_policy(result.policy);
            values.push_back(checked_value(result.value));
          }
          throw_if_cancelled_or_timed_out(options, deadline);

          for (size_t index = 0; index < owners.size(); ++index) {
            throw_if_cancelled_or_timed_out(options, deadline);
            auto ready = tree.publish_evaluation(
                owners[index].node, owners[index].pending,
                results[index].policy, values[index], ledger);
            registry.erase_pending(owners[index].pending->id);
            // Publication only makes tickets ready. Each path is committed
            // separately, which is also required by deterministic replay.
            for (uint64_t simulation_id : ready) {
              auto ticket = lookup_ticket(
                  active_tickets, options.simulation_id_base, simulation_id);
              complete_ticket(ticket, values[index],
                              CompletionKind::EvaluatedLeaf, root, *ledger);
              active_tickets.erase(simulation_id);
              ++terminalized;
            }
          }
        }
        issue_available();
      }
    } catch (const detail::CancellationReached &) {
      stop_reason = SearchStopReason::Cancelled;
      partial = true;
    } catch (const detail::TimeoutReached &) {
      stop_reason = SearchStopReason::TimedOut;
      partial = true;
    } catch (const detail::InferenceCallbackFailure &callback_failure) {
      failure = callback_failure.error();
      stop_reason = SearchStopReason::WorkerError;
      partial = true;
    } catch (const TreeCapacityReachedError &) {
      stop_reason = SearchStopReason::TreeCapacityReached;
      partial = true;
    } catch (...) {
      failure = std::current_exception();
      stop_reason = SearchStopReason::WorkerError;
      partial = true;
    }

    session.request_stop(failure != nullptr || partial);
    session.join_workers();

    cleanup_pending(tree, registry);
    cleanup_tickets(active_tickets, failure != nullptr, *ledger);

    try {
      tree.validate_quiescent_for_search();
      const auto snapshot = ledger->snapshot();
      if (!snapshot.virtual_loss_balanced())
        throw std::logic_error("parallel virtual-loss ledger is unbalanced");
      if (snapshot.issued !=
          snapshot.completed() + snapshot.cancelled + snapshot.failed)
        throw std::logic_error("parallel ticket ledger is unbalanced");
    } catch (...) {
      if (!failure)
        failure = std::current_exception();
    }

    auto result = make_result(tree, root, *ledger, stop_reason, partial,
                              temperature, started);
    guard.finish();
    if (failure)
      std::rethrow_exception(failure);
    return result;
  }

private:
  static ParallelSearchResult
  run_zero_budget(MCTS &mcts, MCTS::SearchGuard guard, const Game &root_game,
                  const ParallelSearchOptions &options, float temperature,
                  DeterministicTrace *trace, SearchStopReason stop_reason,
                  bool partial) {
    const auto started = std::chrono::steady_clock::now();
    const MCTSConfig config = guard.config();
    const uint8_t observer = static_cast<uint8_t>(
        mcts_internal::GameAdapter::current_player(root_game));
    Game root_copy = mcts_internal::GameAdapter::clone_light(root_game);
    const TreeKey root_key = mcts_internal::GameAdapter::tree_key(
        root_copy, observer, config.use_determinization);
    const uint64_t nonce =
        options.search_nonce == std::numeric_limits<uint64_t>::max()
            ? guard.search_nonce()
            : options.search_nonce;
    const uint64_t master_seed =
        options.master_seed.value_or(guard.resolved_master_seed());
    SearchRandomContext random{master_seed, root_key, nonce,
                               options.simulation_id_base, MCTS_RNG_VERSION};

    ParallelSearchResult result;
    if (const auto root = mcts.get_parallel_node_snapshot(root_key)) {
      result.visits = root->stats.N;
      result.q_values = root->stats.Q;
      result.probabilities =
          detail::probabilities_from_visits(result.visits, temperature);
    }
    auto snapshots = mcts.get_parallel_tree_snapshot();
    result.stop_reason = stop_reason;
    result.resolved_seed = random.resolved_master_seed;
    result.search_nonce = nonce;
    result.tree_generation = guard.tree_generation();
    result.tree_size = snapshots.size();
    result.elapsed_microseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    result.partial = partial;

    if (trace) {
      trace->version = MCTS_PARALLEL_TRACE_VERSION;
      trace->root_key = root_key;
      trace->master_seed = random.resolved_master_seed;
      trace->search_nonce = nonce;
      trace->rng_version = MCTS_RNG_VERSION;
      trace->config_digest = trace_detail::config_digest(config);
      trace->evaluator_version = options.evaluator_version;
      trace->has_root_noise = false;
      trace->root_noise.fill(0.0f);
      trace->initial_nodes = std::move(snapshots);
      std::sort(
          trace->initial_nodes.begin(), trace->initial_nodes.end(),
          [](const MCTSNodeSnapshot64 &left, const MCTSNodeSnapshot64 &right) {
            return trace_detail::key_less(left.key, right.key);
          });
      trace->initial_tree_digest =
          trace_detail::snapshot_digest(trace->initial_nodes);
      trace->aggregate_node_snapshots = trace->initial_nodes.size();
      trace->events.clear();
      trace->previous_chain = trace->initial_chain_digest();
      trace->verify();
    }
    guard.finish();
    return result;
  }

  static std::chrono::steady_clock::time_point
  search_deadline(std::chrono::steady_clock::time_point started,
                  uint64_t timeout_ms) noexcept {
    if (timeout_ms == 0)
      return std::chrono::steady_clock::time_point::max();
    const auto remaining =
        std::chrono::steady_clock::time_point::max() - started;
    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
    const auto requested =
        std::chrono::milliseconds(static_cast<int64_t>(timeout_ms));
    if (requested >= remaining_ms)
      return std::chrono::steady_clock::time_point::max();
    return started +
           std::chrono::duration_cast<std::chrono::steady_clock::duration>(
               requested);
  }

  static void throw_if_cancelled_or_timed_out(
      const ParallelSearchOptions &options,
      std::chrono::steady_clock::time_point deadline) {
    if (options.cancellation_requested())
      throw detail::CancellationReached{};
    if (std::chrono::steady_clock::now() >= deadline)
      throw detail::TimeoutReached{};
  }

  static void normalize_and_validate_options(ParallelSearchOptions &options,
                                             const MCTSConfig &config) {
    if (options.num_simulations == 0 && config.num_simulations > 0)
      options.num_simulations = static_cast<uint64_t>(config.num_simulations);
    if (options.num_simulations >
        std::numeric_limits<uint64_t>::max() - options.simulation_id_base)
      throw std::overflow_error("parallel simulation identifier range wraps");
    if (options.max_inflight == 0) {
      const uint64_t batch_window =
          static_cast<uint64_t>(options.batch_size) * 2;
      const uint64_t thread_window =
          static_cast<uint64_t>(options.num_threads) * 4;
      const uint64_t preferred = std::max<uint64_t>(
          options.num_threads, std::min(batch_window, thread_window));
      options.max_inflight = static_cast<uint32_t>(
          std::min<uint64_t>(preferred, std::numeric_limits<uint32_t>::max()));
    }
    options.max_inflight = std::max<uint32_t>(1, options.max_inflight);
  }

  static void register_pending_or_rollback(
      ConcurrentTree &tree, detail::SessionRegistry &registry,
      const NodeHandle &node,
      const std::shared_ptr<PendingEvaluation> &pending) {
    try {
      registry.register_pending(node, pending);
    } catch (...) {
      const std::exception_ptr original = std::current_exception();
      try {
        tree.fail_evaluation(node, pending, PendingState::Failed);
      } catch (...) {
        // Preserve the allocation/registry failure that triggered rollback.
        // validate_quiescent() will surface a secondary integrity failure.
      }
      std::rethrow_exception(original);
    }
  }

  static void bootstrap_root(ConcurrentTree &tree, detail::RootSnapshot &root,
                             const NodeHandle &root_node,
                             const ParallelInferenceFunction &inference) {
    const auto snapshot = tree.snapshot(root_node);
    if (snapshot.state == ExpansionState::Expanded ||
        snapshot.state == ExpansionState::Terminal)
      return;
    if (snapshot.state == ExpansionState::Evaluating)
      throw std::logic_error("root retains an unfinished evaluation");
    if (mcts_internal::GameAdapter::is_terminal(root.root)) {
      const auto value =
          mcts_internal::GameAdapter::terminal_value(root.root, 0.0f);
      tree.set_terminal(root_node, checked_value(value));
      return;
    }

    const auto features =
        mcts_internal::GameAdapter::native_features(root.root, root.observer);
    const auto mask =
        mcts_internal::GameAdapter::native_action_mask_bits(root.root);
    const uint64_t bootstrap_id = std::numeric_limits<uint64_t>::max();
    auto claim = tree.claim_or_attach_bits(root_node, bootstrap_id,
                                           detail::feature_digest(features),
                                           features, mask, nullptr);
    if (claim.kind != ExpansionClaimKind::Owner)
      throw std::logic_error("root bootstrap did not own expansion");
    try {
      std::vector<ParallelInferenceRequest> requests{claim.pending->request};
      auto results = detail::call_inference_callback(inference, requests);
      if (results.size() != 1)
        throw std::invalid_argument(
            "root bootstrap inference must return one result");
      validate_policy(results[0].policy);
      const Value value = checked_value(results[0].value);
      const auto ready = tree.publish_evaluation(
          root_node, claim.pending, results[0].policy, value, nullptr);
      if (ready.size() != 1 || ready[0] != bootstrap_id)
        throw std::logic_error("root bootstrap ticket ownership is corrupt");
    } catch (...) {
      const std::exception_ptr original = std::current_exception();
      try {
        PendingState state;
        {
          std::lock_guard<std::mutex> lock(claim.pending->mutex);
          state = claim.pending->state;
        }
        if (state == PendingState::Open)
          tree.fail_evaluation(root_node, claim.pending, PendingState::Failed);
      } catch (...) {
        // Preserve the evaluator/tree failure that entered rollback. The final
        // quiescence check reports a secondary cleanup integrity failure.
      }
      std::rethrow_exception(original);
    }
  }

  static void worker_loop(
      uint32_t /*worker_id*/, const detail::RootSnapshot &root,
      ConcurrentTree &tree,
      BoundedQueue<std::shared_ptr<detail::SimulationTicket>> &work_queue,
      BoundedQueue<detail::WorkerEvent> &event_queue,
      detail::SessionRegistry &registry,
      const std::shared_ptr<SearchLedger> &ledger,
      const ParallelSearchOptions &options, std::atomic<bool> &stop_requested,
      detail::WorkerFailureState &worker_failure) noexcept {
    std::shared_ptr<detail::SimulationTicket> ticket;
    while (work_queue.pop(ticket)) {
      if (stop_requested.load(std::memory_order_acquire) ||
          options.cancellation_requested()) {
        stop_requested.store(true, std::memory_order_release);
        work_queue.close();
        break;
      }
      try {
        traverse_ticket(root, tree, ticket, event_queue, registry, ledger,
                        stop_requested);
      } catch (...) {
        const std::exception_ptr error = std::current_exception();
        worker_failure.publish(error);
        stop_requested.store(true, std::memory_order_release);
        work_queue.close();
        // This is a noexcept worker boundary. Closing the queue wakes the
        // coordinator without constructing/pushing an error event, which
        // could allocate, block behind a full queue, or terminate this thread.
        event_queue.close();
        return;
      }
      ticket.reset();
    }
  }

  static void
  traverse_ticket(const detail::RootSnapshot &root, ConcurrentTree &tree,
                  const std::shared_ptr<detail::SimulationTicket> &ticket,
                  BoundedQueue<detail::WorkerEvent> &event_queue,
                  detail::SessionRegistry &registry,
                  const std::shared_ptr<SearchLedger> &ledger,
                  const std::atomic<bool> &stop_requested) {
    ticket->state.store(TicketState::Traversing, std::memory_order_release);
    Game game =
        root.config.use_determinization
            ? mcts_internal::GameAdapter::determinize_portable(
                  root.root, root.observer,
                  root.random.seed_for(SearchRandomDomain::RootDeterminization,
                                       ticket->simulation_id))
            : mcts_internal::GameAdapter::clone_light(root.root);
    TreeKey current_key = root.root_key;

    for (int depth = 0; depth < MAX_DEPTH; ++depth) {
      if (stop_requested.load(std::memory_order_acquire))
        return;
      auto node = tree.find_or_create(current_key);

      if (mcts_internal::GameAdapter::is_terminal(game)) {
        const auto terminal =
            mcts_internal::GameAdapter::terminal_value(game, 0.0f);
        const Value value = checked_value(terminal);
        const auto node_state = tree.state_view(node);
        if (node_state.state != ExpansionState::Terminal)
          tree.set_terminal(node, value);
        emit_completion(event_queue, ticket, value, CompletionKind::Terminal);
        return;
      }

      if (mcts_internal::GameAdapter::requires_forced_pass(game)) {
        mcts_internal::GameAdapter::resolve_forced_pass(game);
        current_key = mcts_internal::GameAdapter::tree_key(
            game, root.observer, root.config.use_determinization);
        continue;
      }

      const auto node_state = tree.state_view(node);
      if (node_state.state == ExpansionState::Terminal) {
        emit_completion(event_queue, ticket, node_state.value,
                        CompletionKind::Terminal);
        return;
      }
      if (node_state.state != ExpansionState::Expanded) {
        const auto features =
            mcts_internal::GameAdapter::native_features(game, root.observer);
        const auto mask =
            mcts_internal::GameAdapter::native_action_mask_bits(game);
        const uint64_t public_feature_digest = detail::feature_digest(features);
        ticket->trace_data.leaf_key = current_key;
        ticket->trace_data.feature_digest = public_feature_digest;
        ticket->trace_data.world_mask_digest = trace_detail::mask_digest(mask);
        // This write occurs before claim_or_attach releases the pending lock,
        // which publishes the complete path to the coordinator.
        ticket->state.store(TicketState::WaitingInference,
                            std::memory_order_release);
        auto claim = tree.claim_or_attach_bits(node, ticket->simulation_id,
                                               public_feature_digest, features,
                                               mask, ledger);
        if (claim.kind == ExpansionClaimKind::Owner) {
          ticket->trace_data.leaf_role = TraceLeafRole::Owner;
          ticket->trace_data.pending_id = claim.pending->id;
          ticket->pending_id.store(claim.pending->id,
                                   std::memory_order_release);
          register_pending_or_rollback(tree, registry, node, claim.pending);
          detail::WorkerEvent event;
          event.kind = detail::WorkerEventKind::EvaluationOwner;
          event.ticket = ticket;
          event.node = std::move(node);
          event.pending = std::move(claim.pending);
          if (!event_queue.push(std::move(event)))
            return;
          return;
        }
        if (claim.kind == ExpansionClaimKind::Waiter) {
          ticket->trace_data.leaf_role = TraceLeafRole::Waiter;
          ticket->trace_data.pending_id = claim.pending->id;
          ticket->pending_id.store(claim.pending->id,
                                   std::memory_order_release);
          return;
        }
        ticket->state.store(TicketState::Traversing, std::memory_order_release);
        if (claim.kind == ExpansionClaimKind::Terminal) {
          emit_completion(event_queue, ticket, claim.cached_value,
                          CompletionKind::Terminal);
          return;
        }
        // Expanded or a late-attach retry: re-read the fully published node.
        continue;
      }

      const auto world_mask =
          mcts_internal::GameAdapter::native_action_mask_bits(game);
      SelectionContext context;
      context.cpuct = root.config.cpuct;
      context.fpu = root.config.fpu;
      context.forced_playouts_k = root.config.forced_playouts_k;
      context.dirichlet_epsilon = root.config.dirichlet_epsilon;
      context.forced_playouts = root.config.forced_playouts;
      context.is_root = depth == 0;
      context.simulation_ordinal = ticket->simulation_id;
      context.tree_generation = root.tree_generation;
      context.root_noise =
          context.is_root && root.has_root_noise ? &root.root_noise : nullptr;
      auto reservation = tree.select_and_reserve_bits(
          node, world_mask, context,
          static_cast<uint8_t>(
              mcts_internal::GameAdapter::current_player(game)),
          ledger);
      if (!reservation)
        throw std::runtime_error(
            "non-terminal parallel node has no world-local action");
      const int action = reservation->action();
      ticket->path.append(std::move(*reservation));
      if (!mcts_internal::GameAdapter::decode_and_apply_native(game, action)) {
        ledger->invalid_replay.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error(
            "parallel selection produced an unavailable action");
      }
      current_key = mcts_internal::GameAdapter::tree_key(
          game, root.observer, root.config.use_determinization);
    }

    const Value max_depth_value{};
    emit_completion(event_queue, ticket, max_depth_value,
                    CompletionKind::MaxDepth);
  }

  static detail::WorkerEvent traverse_ticket_synchronously(
      const detail::RootSnapshot &root, ConcurrentTree &tree,
      const std::shared_ptr<detail::SimulationTicket> &ticket,
      detail::SessionRegistry &registry,
      const std::shared_ptr<SearchLedger> &ledger) {
    ticket->state.store(TicketState::Traversing, std::memory_order_release);
    Game game =
        root.config.use_determinization
            ? mcts_internal::GameAdapter::determinize_portable(
                  root.root, root.observer,
                  root.random.seed_for(SearchRandomDomain::RootDeterminization,
                                       ticket->simulation_id))
            : mcts_internal::GameAdapter::clone_light(root.root);
    TreeKey current_key = root.root_key;

    for (int depth = 0; depth < MAX_DEPTH; ++depth) {
      auto node = tree.find_or_create(current_key);
      if (mcts_internal::GameAdapter::is_terminal(game)) {
        const Value value = checked_value(
            mcts_internal::GameAdapter::terminal_value(game, 0.0f));
        const auto node_state = tree.state_view(node);
        if (node_state.state != ExpansionState::Terminal)
          tree.set_terminal(node, value);
        detail::WorkerEvent event;
        event.kind = detail::WorkerEventKind::DirectCompletion;
        event.ticket = ticket;
        event.value = value;
        event.completion = CompletionKind::Terminal;
        return event;
      }

      if (mcts_internal::GameAdapter::requires_forced_pass(game)) {
        mcts_internal::GameAdapter::resolve_forced_pass(game);
        current_key = mcts_internal::GameAdapter::tree_key(
            game, root.observer, root.config.use_determinization);
        continue;
      }

      const auto node_state = tree.state_view(node);
      if (node_state.state == ExpansionState::Terminal) {
        detail::WorkerEvent event;
        event.kind = detail::WorkerEventKind::DirectCompletion;
        event.ticket = ticket;
        event.value = node_state.value;
        event.completion = CompletionKind::Terminal;
        return event;
      }
      if (node_state.state != ExpansionState::Expanded) {
        const auto features =
            mcts_internal::GameAdapter::native_features(game, root.observer);
        const auto mask =
            mcts_internal::GameAdapter::native_action_mask_bits(game);
        const uint64_t public_feature_digest = detail::feature_digest(features);
        ticket->trace_data.leaf_key = current_key;
        ticket->trace_data.feature_digest = public_feature_digest;
        ticket->trace_data.world_mask_digest = trace_detail::mask_digest(mask);
        ticket->state.store(TicketState::WaitingInference,
                            std::memory_order_release);
        auto claim = tree.claim_or_attach_bits(node, ticket->simulation_id,
                                               public_feature_digest, features,
                                               mask, ledger);
        if (claim.kind == ExpansionClaimKind::Owner) {
          ticket->trace_data.leaf_role = TraceLeafRole::Owner;
          ticket->trace_data.pending_id = claim.pending->id;
          ticket->pending_id.store(claim.pending->id,
                                   std::memory_order_release);
          register_pending_or_rollback(tree, registry, node, claim.pending);
          detail::WorkerEvent event;
          event.kind = detail::WorkerEventKind::EvaluationOwner;
          event.ticket = ticket;
          event.node = std::move(node);
          event.pending = std::move(claim.pending);
          return event;
        }
        if (claim.kind == ExpansionClaimKind::Waiter) {
          ticket->trace_data.leaf_role = TraceLeafRole::Waiter;
          ticket->trace_data.pending_id = claim.pending->id;
          ticket->pending_id.store(claim.pending->id,
                                   std::memory_order_release);
          detail::WorkerEvent event;
          event.kind = detail::WorkerEventKind::Waiting;
          event.ticket = ticket;
          event.pending = std::move(claim.pending);
          return event;
        }
        ticket->state.store(TicketState::Traversing, std::memory_order_release);
        if (claim.kind == ExpansionClaimKind::Terminal) {
          detail::WorkerEvent event;
          event.kind = detail::WorkerEventKind::DirectCompletion;
          event.ticket = ticket;
          event.value = claim.cached_value;
          event.completion = CompletionKind::Terminal;
          return event;
        }
        continue;
      }

      const auto world_mask =
          mcts_internal::GameAdapter::native_action_mask_bits(game);
      SelectionContext context;
      context.cpuct = root.config.cpuct;
      context.fpu = root.config.fpu;
      context.forced_playouts_k = root.config.forced_playouts_k;
      context.dirichlet_epsilon = root.config.dirichlet_epsilon;
      context.forced_playouts = root.config.forced_playouts;
      context.is_root = depth == 0;
      context.simulation_ordinal = ticket->simulation_id;
      context.tree_generation = root.tree_generation;
      context.root_noise =
          context.is_root && root.has_root_noise ? &root.root_noise : nullptr;
      auto reservation = tree.select_and_reserve_bits(
          node, world_mask, context,
          static_cast<uint8_t>(
              mcts_internal::GameAdapter::current_player(game)),
          ledger);
      if (!reservation)
        throw std::runtime_error(
            "non-terminal deterministic node has no world-local action");
      const int action = reservation->action();
      ticket->path.append(std::move(*reservation));
      if (!mcts_internal::GameAdapter::decode_and_apply_native(game, action)) {
        ledger->invalid_replay.fetch_add(1, std::memory_order_relaxed);
        throw std::runtime_error(
            "deterministic selection produced an unavailable action");
      }
      current_key = mcts_internal::GameAdapter::tree_key(
          game, root.observer, root.config.use_determinization);
    }

    detail::WorkerEvent event;
    event.kind = detail::WorkerEventKind::DirectCompletion;
    event.ticket = ticket;
    event.value = Value{};
    event.completion = CompletionKind::MaxDepth;
    return event;
  }

  static void
  emit_completion(BoundedQueue<detail::WorkerEvent> &event_queue,
                  const std::shared_ptr<detail::SimulationTicket> &ticket,
                  const Value &value, CompletionKind completion) {
    detail::WorkerEvent event;
    event.kind = detail::WorkerEventKind::DirectCompletion;
    event.ticket = ticket;
    event.value = value;
    event.completion = completion;
    event_queue.push(std::move(event));
  }

  static std::shared_ptr<detail::SimulationTicket>
  lookup_ticket(const detail::ActiveTicketRegistry &tickets,
                uint64_t simulation_id_base, uint64_t simulation_id) {
    if (simulation_id < simulation_id_base)
      throw std::logic_error("pending result has an invalid simulation ID");
    const auto found = tickets.find(simulation_id);
    if (found == tickets.end() || !found->second)
      throw std::logic_error("pending result references an unknown ticket");
    return found->second;
  }

  static void
  complete_ticket(const std::shared_ptr<detail::SimulationTicket> &ticket,
                  const Value &value, CompletionKind completion,
                  const detail::RootSnapshot &root, SearchLedger &ledger) {
    const TicketState previous = ticket->state.exchange(
        TicketState::Committing, std::memory_order_acq_rel);
    if (previous == TicketState::Completed ||
        previous == TicketState::Cancelled || previous == TicketState::Failed ||
        previous == TicketState::Committing) {
      ledger.duplicate_result.fetch_add(1, std::memory_order_relaxed);
      throw std::logic_error("parallel ticket was completed twice");
    }
    ticket->path.commit(value, root.tree_generation);
    ticket->completion = completion;
    ticket->state.store(TicketState::Completed, std::memory_order_release);
    switch (completion) {
    case CompletionKind::EvaluatedLeaf:
      ledger.completed_evaluated.fetch_add(1, std::memory_order_relaxed);
      break;
    case CompletionKind::Terminal:
      ledger.completed_terminal.fetch_add(1, std::memory_order_relaxed);
      break;
    case CompletionKind::MaxDepth:
      ledger.completed_max_depth.fetch_add(1, std::memory_order_relaxed);
      break;
    case CompletionKind::None:
      throw std::logic_error("ticket completion kind is missing");
    }
  }

  static void process_or_collect(detail::WorkerEvent &event,
                                 std::vector<detail::WorkerEvent> &owners,
                                 detail::ActiveTicketRegistry &active_tickets,
                                 const detail::RootSnapshot &root,
                                 SearchLedger &ledger, uint64_t &terminalized) {
    if (event.kind == detail::WorkerEventKind::EvaluationOwner) {
      owners.push_back(std::move(event));
      return;
    }
    if (event.kind != detail::WorkerEventKind::DirectCompletion)
      throw std::logic_error("unexpected parallel worker event");
    if (!event.ticket)
      throw std::logic_error("parallel worker returned an empty ticket");
    const auto registered =
        lookup_ticket(active_tickets, 0, event.ticket->simulation_id);
    if (registered != event.ticket)
      throw std::logic_error("parallel worker returned a foreign ticket");
    complete_ticket(event.ticket, event.value, event.completion, root, ledger);
    active_tickets.erase(event.ticket->simulation_id);
    ++terminalized;
  }

  static void cleanup_pending(ConcurrentTree &tree,
                              detail::SessionRegistry &registry) noexcept {
    registry.drain_pending([&](detail::SessionRegistry::PendingEntry &entry) {
      try {
        PendingState state;
        {
          std::lock_guard<std::mutex> lock(entry.pending->mutex);
          state = entry.pending->state;
        }
        if (state == PendingState::Open)
          tree.fail_evaluation(entry.node, entry.pending,
                               PendingState::Cancelled);
      } catch (...) {
        // Quiescent validation reports a retained Evaluating node. Cleanup
        // itself must keep progressing so every reservation can be released.
      }
    });
  }

  static void cleanup_tickets(
      const std::vector<std::shared_ptr<detail::SimulationTicket>> &tickets,
      bool failed, SearchLedger &ledger) noexcept {
    for (const auto &ticket : tickets) {
      if (!ticket)
        continue;
      const TicketState state = ticket->state.load(std::memory_order_acquire);
      if (state == TicketState::Completed || state == TicketState::Cancelled ||
          state == TicketState::Failed)
        continue;
      try {
        if (ticket->path.state() == ReservedPathState::Open)
          ticket->path.abort();
      } catch (...) {
        ledger.integrity_errors.fetch_add(1, std::memory_order_relaxed);
      }
      ticket->state.store(failed ? TicketState::Failed : TicketState::Cancelled,
                          std::memory_order_release);
      if (failed)
        ledger.failed.fetch_add(1, std::memory_order_relaxed);
      else
        ledger.cancelled.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static void cleanup_tickets(const detail::ActiveTicketRegistry &tickets,
                              bool failed, SearchLedger &ledger) noexcept {
    for (const auto &[simulation_id, ticket] : tickets) {
      (void)simulation_id;
      if (!ticket)
        continue;
      const TicketState state = ticket->state.load(std::memory_order_acquire);
      if (state == TicketState::Completed || state == TicketState::Cancelled ||
          state == TicketState::Failed)
        continue;
      try {
        if (ticket->path.state() == ReservedPathState::Open)
          ticket->path.abort();
      } catch (...) {
        ledger.integrity_errors.fetch_add(1, std::memory_order_relaxed);
      }
      ticket->state.store(failed ? TicketState::Failed : TicketState::Cancelled,
                          std::memory_order_release);
      if (failed)
        ledger.failed.fetch_add(1, std::memory_order_relaxed);
      else
        ledger.cancelled.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static ParallelSearchResult
  make_result(const ConcurrentTree &tree, const detail::RootSnapshot &root,
              const SearchLedger &ledger, SearchStopReason stop_reason,
              bool partial, float temperature,
              const std::chrono::steady_clock::time_point &started) {
    ParallelSearchResult result;
    const auto root_snapshot = tree.snapshot(root.root_key);
    if (stop_reason == SearchStopReason::TreeCapacityReached &&
        (!root_snapshot || (root_snapshot->state != ExpansionState::Expanded &&
                            root_snapshot->state != ExpansionState::Terminal)))
      throw TreeCapacityReachedError(
          "parallel tree capacity reached before root expansion");
    if (root_snapshot) {
      result.visits = root_snapshot->stats.N;
      result.q_values = root_snapshot->stats.Q;
      const bool has_visits =
          std::any_of(result.visits.begin(), result.visits.end(),
                      [](uint64_t visits) { return visits != 0; });
      result.probabilities =
          root_snapshot->state == ExpansionState::Expanded && !has_visits
              ? detail::probabilities_from_root_prior(*root_snapshot, root)
              : detail::probabilities_from_visits(result.visits, temperature);
    }
    result.ledger = ledger.snapshot();
    result.stop_reason = stop_reason;
    result.resolved_seed = root.random.resolved_master_seed;
    result.search_nonce = root.random.search_nonce;
    result.tree_generation = root.tree_generation;
    result.tree_size = tree.size();
    result.elapsed_microseconds = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    result.partial = partial;
    return result;
  }

  ParallelSearchResult
  run_deterministic_epoch(MCTS &mcts, MCTS::SearchGuard guard,
                          const Game &root_game, ParallelSearchOptions options,
                          const ParallelInferenceFunction &inference,
                          float temperature, DeterministicTrace *trace) const {
    const auto started = std::chrono::steady_clock::now();
    auto &tree = mcts.prepare_parallel_tree(
        guard, options.tree_backend, options.shard_count,
        static_cast<size_t>(options.max_tree_nodes));
    tree.bind_evaluator(options.evaluator_version);
    tree.validate_quiescent_for_search();
    const MCTSConfig config = guard.config();

    const uint8_t observer = static_cast<uint8_t>(
        mcts_internal::GameAdapter::current_player(root_game));
    Game root_copy = mcts_internal::GameAdapter::clone_light(root_game);
    const TreeKey root_key = mcts_internal::GameAdapter::tree_key(
        root_copy, observer, config.use_determinization);
    const uint64_t nonce =
        options.search_nonce == std::numeric_limits<uint64_t>::max()
            ? guard.search_nonce()
            : options.search_nonce;
    const uint64_t master_seed =
        options.master_seed.value_or(guard.resolved_master_seed());
    SearchRandomContext random{master_seed, root_key, nonce,
                               options.simulation_id_base, MCTS_RNG_VERSION};
    detail::RootSnapshot root(std::move(root_copy), config, observer, root_key,
                              guard.tree_generation(), random);
    if (trace) {
      trace->version = MCTS_PARALLEL_TRACE_VERSION;
      trace->root_key = root_key;
      trace->master_seed = random.resolved_master_seed;
      trace->search_nonce = nonce;
      trace->rng_version = MCTS_RNG_VERSION;
      trace->config_digest = trace_detail::config_digest(config);
      trace->evaluator_version = options.evaluator_version;
      trace->has_root_noise = false;
      trace->root_noise.fill(0.0f);
      trace->events.clear();
      trace->capture_initial(tree);
      trace->ensure_record_capacity(options.num_simulations, 1);
    }
    auto ledger = std::make_shared<SearchLedger>();
    std::vector<std::shared_ptr<detail::SimulationTicket>> epoch_tickets;
    detail::SessionRegistry registry;
    std::exception_ptr failure;
    SearchStopReason stop_reason = SearchStopReason::Completed;
    bool partial = false;
    uint64_t issued = 0;
    const auto deadline = search_deadline(started, options.timeout_ms);

    try {
      throw_if_cancelled_or_timed_out(options, deadline);
      if (options.num_simulations == 0) {
        auto result = make_result(tree, root, *ledger, stop_reason, partial,
                                  temperature, started);
        guard.finish();
        return result;
      }
      auto root_node = tree.find_or_create(root.root_key);
      bootstrap_root(tree, root, root_node, inference);
      throw_if_cancelled_or_timed_out(options, deadline);
      const auto root_snapshot = tree.snapshot(root_node);
      if (root_snapshot.state == ExpansionState::Terminal) {
        if (trace)
          trace->capture_initial(tree);
        auto result = make_result(tree, root, *ledger, stop_reason, partial,
                                  temperature, started);
        guard.finish();
        return result;
      }
      if (config.use_dirichlet_noise) {
        root.root_noise = detail::generate_root_noise(
            root_snapshot.valid_actions, config.dirichlet_alpha,
            detail::root_dirichlet_seed(root));
        root.has_root_noise = true;
        if (trace) {
          trace->has_root_noise = true;
          trace->root_noise = root.root_noise;
        }
      }
      if (trace)
        trace->capture_initial(tree);

      while (issued < options.num_simulations) {
        throw_if_cancelled_or_timed_out(options, deadline);
        const uint64_t epoch_begin = issued;
        const uint64_t epoch_count =
            std::min<uint64_t>(options.deterministic_epoch_size,
                               options.num_simulations - epoch_begin);
        epoch_tickets.assign(static_cast<size_t>(epoch_count), nullptr);
        std::vector<detail::WorkerEvent> owners;
        owners.reserve(static_cast<size_t>(epoch_count));
        struct ReadyValue {
          Value value{};
          CompletionKind completion = CompletionKind::None;
          uint64_t inference_digest = 0;
          bool ready = false;
        };
        std::vector<ReadyValue> ready_values(static_cast<size_t>(epoch_count));
        const uint64_t epoch_simulation_begin =
            options.simulation_id_base + epoch_begin;
        auto ready_slot = [&](uint64_t simulation_id) -> ReadyValue & {
          if (simulation_id < epoch_simulation_begin ||
              simulation_id >= epoch_simulation_begin + epoch_count)
            throw std::logic_error("deterministic pending escaped its epoch");
          return ready_values[static_cast<size_t>(simulation_id -
                                                  epoch_simulation_begin)];
        };

        // Full traversal/reservation is deliberately serialized by logical
        // simulation ID. Worker count and inference completion order therefore
        // cannot change selection within an epoch.
        for (uint64_t offset = 0; offset < epoch_count; ++offset) {
          throw_if_cancelled_or_timed_out(options, deadline);
          const uint64_t index = epoch_begin + offset;
          const uint64_t simulation_id = options.simulation_id_base + index;
          auto ticket = std::make_shared<detail::SimulationTicket>(
              simulation_id, root.tree_generation);
          epoch_tickets[static_cast<size_t>(offset)] = ticket;
          ++issued;
          ledger->issued.fetch_add(1, std::memory_order_relaxed);
          SearchLedger::update_max(ledger->max_inflight_observed, offset + 1);
          auto event = traverse_ticket_synchronously(root, tree, ticket,
                                                     registry, ledger);
          if (event.kind == detail::WorkerEventKind::EvaluationOwner)
            owners.push_back(std::move(event));
          else if (event.kind == detail::WorkerEventKind::DirectCompletion) {
            auto &slot = ready_slot(simulation_id);
            if (slot.ready)
              throw std::logic_error(
                  "deterministic ticket became ready more than once");
            slot.value = event.value;
            slot.completion = event.completion;
            slot.ready = true;
          } else if (event.kind != detail::WorkerEventKind::Waiting)
            throw std::logic_error("invalid deterministic traversal outcome");
        }
        throw_if_cancelled_or_timed_out(options, deadline);

        std::vector<ParallelInferenceResult> evaluated;
        evaluated.reserve(owners.size());
        for (size_t begin = 0; begin < owners.size();
             begin += options.batch_size) {
          const size_t end =
              std::min(owners.size(), begin + options.batch_size);
          std::vector<ParallelInferenceRequest> requests;
          requests.reserve(end - begin);
          for (size_t index = begin; index < end; ++index)
            requests.push_back(owners[index].pending->request);
          throw_if_cancelled_or_timed_out(options, deadline);
          auto batch = detail::call_inference_callback(inference, requests);
          throw_if_cancelled_or_timed_out(options, deadline);
          if (batch.size() != requests.size())
            throw std::invalid_argument(
                "deterministic inference result count does not match batch");
          evaluated.insert(evaluated.end(), batch.begin(), batch.end());
        }

        std::vector<Value> evaluated_values;
        evaluated_values.reserve(evaluated.size());
        for (const auto &result : evaluated) {
          validate_policy(result.policy);
          evaluated_values.push_back(checked_value(result.value));
        }
        throw_if_cancelled_or_timed_out(options, deadline);

        // Node publication is separate from ticket commit. A pending may make
        // non-contiguous waiter IDs ready, but all paths below still commit in
        // strict simulation-ID order.
        for (size_t index = 0; index < owners.size(); ++index) {
          throw_if_cancelled_or_timed_out(options, deadline);
          auto ready = tree.publish_evaluation(
              owners[index].node, owners[index].pending,
              evaluated[index].policy, evaluated_values[index], ledger);
          const uint64_t evaluated_digest = trace_detail::inference_digest(
              evaluated[index].policy, evaluated_values[index]);
          registry.erase_pending(owners[index].pending->id);
          for (uint64_t simulation_id : ready) {
            auto &slot = ready_slot(simulation_id);
            if (slot.ready)
              throw std::logic_error(
                  "deterministic ticket became ready more than once");
            slot.value = evaluated_values[index];
            slot.completion = CompletionKind::EvaluatedLeaf;
            slot.inference_digest = evaluated_digest;
            slot.ready = true;
          }
        }

        for (uint64_t offset = 0; offset < epoch_count; ++offset) {
          const uint64_t index = epoch_begin + offset;
          const uint64_t simulation_id = options.simulation_id_base + index;
          const auto &ready = ready_slot(simulation_id);
          if (!ready.ready)
            throw std::logic_error(
                "deterministic epoch did not resolve every ticket");
          throw_if_cancelled_or_timed_out(options, deadline);
          auto &ticket = epoch_tickets[static_cast<size_t>(offset)];
          std::vector<ReservedPathStep> trace_path;
          if (trace)
            trace_path = ticket->path.snapshot_steps();
          ticket->trace_data.inference_digest = ready.inference_digest;
          complete_ticket(ticket, ready.value, ready.completion, root, *ledger);
          if (trace) {
            trace->record(
                simulation_id,
                root.random.seed_for(SearchRandomDomain::RootDeterminization,
                                     simulation_id),
                ready.completion, trace_path, ready.value, ticket->trace_data,
                trace_detail::ledger_digest(ledger->snapshot()), tree);
          }
        }
        epoch_tickets.clear();
#ifndef NDEBUG
        // Debug builds retain the full-tree oracle after every deterministic
        // epoch. Release builds use only the O(1) counters at search
        // boundaries.
        tree.validate_quiescent_for_search();
#endif
      }
      if (trace)
        trace->verify();
    } catch (const detail::CancellationReached &) {
      stop_reason = SearchStopReason::Cancelled;
      partial = true;
    } catch (const detail::TimeoutReached &) {
      stop_reason = SearchStopReason::TimedOut;
      partial = true;
    } catch (const detail::InferenceCallbackFailure &callback_failure) {
      failure = callback_failure.error();
      stop_reason = SearchStopReason::WorkerError;
      partial = true;
    } catch (const TreeCapacityReachedError &) {
      stop_reason = SearchStopReason::TreeCapacityReached;
      partial = true;
    } catch (...) {
      failure = std::current_exception();
      stop_reason = SearchStopReason::WorkerError;
      partial = true;
    }

    cleanup_pending(tree, registry);
    cleanup_tickets(epoch_tickets, failure != nullptr, *ledger);
    try {
      tree.validate_quiescent_for_search();
      const auto snapshot = ledger->snapshot();
      if (!snapshot.virtual_loss_balanced() ||
          snapshot.issued !=
              snapshot.completed() + snapshot.cancelled + snapshot.failed)
        throw std::logic_error("deterministic epoch ledger is unbalanced");
    } catch (...) {
      if (!failure)
        failure = std::current_exception();
    }
    auto result = make_result(tree, root, *ledger, stop_reason, partial,
                              temperature, started);
    guard.finish();
    if (failure)
      std::rethrow_exception(failure);
    return result;
  }
};

} // namespace mcts_parallel

#endif // CSPLENDOR_MCTS_PARALLEL_SEARCHER_H
