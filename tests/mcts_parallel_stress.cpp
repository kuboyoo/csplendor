#include "mcts_bounded_queue.h"
#include "mcts_parallel_searcher.h"
#include "mcts_root_parallel.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace mcts_parallel;

TreeKey exact_key(uint64_t position_hash) {
  return {position_hash, MCTS_TREE_KEY_VERSION, MCTS_NO_OBSERVER,
          TreeDomain::Exact, 0};
}

ActionMask one_action_mask(size_t action) {
  ActionMask mask{};
  mask.at(action) = 1;
  return mask;
}

Policy one_action_policy(size_t action) {
  Policy policy{};
  policy.at(action) = 1.0f;
  return policy;
}

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

void spin_until(const std::atomic<bool> &flag) {
  while (!flag.load(std::memory_order_acquire))
    std::this_thread::yield();
}

void test_shared_edge_reserve_commit(TreeBackend backend) {
  constexpr uint64_t generation = 71;
  constexpr size_t action = 12;
  // Keep the operation count fixed while oversubscribing the small CI runners
  // with 16 software threads. Lower thread counts are covered by the scheduler
  // matrix; this fixture is specifically the PS-8/PS-10 high-contention case.
  constexpr int thread_count = 16;
  constexpr int iterations = 6250;
  static_assert(thread_count * iterations == 100000);

  ConcurrentTree tree(generation, backend, MAX_TREE_SIZE, 8);
  const NodeHandle node = tree.find_or_create(exact_key(0x1001));
  const ActionMask mask = one_action_mask(action);
  Value value{};
  tree.expand(node, one_action_policy(action), value, mask);

  auto ledger = std::make_shared<SearchLedger>();
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      try {
        SelectionContext context;
        context.cpuct = 1.5;
        context.tree_generation = generation;
        ready.fetch_add(1, std::memory_order_release);
        spin_until(start);
        for (int iteration = 0; iteration < iterations; ++iteration) {
          auto reservation = tree.select_and_reserve(
              node, mask, context,
              static_cast<uint8_t>(worker % static_cast<int>(NUM_PLAYERS)),
              ledger);
          if (!reservation || reservation->action() != static_cast<int>(action))
            throw std::runtime_error("shared edge selection failed");
          reservation->commit(0.25, generation);
        }
      } catch (...) {
        failed.store(true, std::memory_order_release);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto &worker : workers)
    worker.join();

  require(!failed.load(std::memory_order_acquire), "shared edge worker failed");
  const auto snapshot = tree.snapshot(node);
  const uint64_t expected = static_cast<uint64_t>(thread_count) * iterations;
  require(snapshot.stats.N[action] == expected,
          "shared edge lost committed visits");
  require(snapshot.stats.total_visits == expected,
          "shared edge total visit count is inconsistent");
  require(snapshot.stats.virtual_loss[action] == 0,
          "shared edge leaked virtual loss");
  require(std::abs(snapshot.stats.Q[action] - 0.25) < 1e-12,
          "shared edge incremental mean is inconsistent");

  const SearchLedgerSnapshot counters = ledger->snapshot();
  require(counters.selected == expected,
          "shared edge selection ledger is inconsistent");
  require(counters.reservations_committed == expected,
          "shared edge commit ledger is inconsistent");
  require(counters.reservations_aborted == 0,
          "shared edge unexpectedly aborted reservations");
  require(counters.virtual_loss_balanced(),
          "shared edge virtual-loss ledger is unbalanced");
  tree.validate_quiescent();
}

void test_same_leaf_claim_dedup(TreeBackend backend) {
  constexpr uint64_t generation = 72;
  constexpr int thread_count = 16;
  ConcurrentTree tree(generation, backend, MAX_TREE_SIZE, 8);
  const NodeHandle node = tree.find_or_create(exact_key(0x2002));
  auto ledger = std::make_shared<SearchLedger>();

  std::array<float, FEATURE_SIZE> features{};
  features[3] = 1.0f;
  features[17] = -0.5f;
  const ActionMask mask = one_action_mask(4);
  std::vector<ExpansionClaim> claims(thread_count);
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      try {
        ready.fetch_add(1, std::memory_order_release);
        spin_until(start);
        claims[static_cast<size_t>(worker)] =
            tree.claim_or_attach(node, static_cast<uint64_t>(1000 + worker),
                                 0xabcdef, features, mask, ledger);
      } catch (...) {
        failed.store(true, std::memory_order_release);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto &worker : workers)
    worker.join();
  require(!failed.load(std::memory_order_acquire),
          "same-leaf claim worker failed");

  size_t owner_index = claims.size();
  size_t owners = 0;
  size_t waiters = 0;
  std::shared_ptr<PendingEvaluation> shared_pending;
  for (size_t index = 0; index < claims.size(); ++index) {
    const ExpansionClaim &claim = claims[index];
    if (claim.kind == ExpansionClaimKind::Owner) {
      ++owners;
      owner_index = index;
      shared_pending = claim.pending;
    } else if (claim.kind == ExpansionClaimKind::Waiter) {
      ++waiters;
    }
  }
  require(owners == 1 && waiters == thread_count - 1,
          "same leaf did not produce exactly one owner");
  require(owner_index < claims.size() && shared_pending,
          "same-leaf owner has no pending evaluation");
  for (const ExpansionClaim &claim : claims)
    require(claim.pending.get() == shared_pending.get(),
            "same-leaf waiter did not share the pending evaluation");

  Value value{};
  value[0] = 0.5;
  const auto attached = tree.publish_evaluation(
      node, shared_pending, one_action_policy(4), value, ledger);
  require(attached.size() == static_cast<size_t>(thread_count),
          "same-leaf publication lost attached tickets");
  std::unordered_set<uint64_t> unique(attached.begin(), attached.end());
  require(unique.size() == static_cast<size_t>(thread_count),
          "same-leaf publication duplicated a ticket");
  for (int worker = 0; worker < thread_count; ++worker)
    require(unique.count(static_cast<uint64_t>(1000 + worker)) == 1,
            "same-leaf publication omitted a ticket");

  const SearchLedgerSnapshot counters = ledger->snapshot();
  require(counters.evaluation_owner == 1,
          "same-leaf owner ledger is inconsistent");
  require(counters.evaluation_waiter == thread_count - 1,
          "same-leaf waiter ledger is inconsistent");
  require(counters.evaluation_requested == 1 && counters.evaluated_boards == 1,
          "same-leaf dedup requested more than one evaluation");
  require(tree.snapshot(node).state == ExpansionState::Expanded,
          "same-leaf node was not published");
  tree.validate_quiescent();
}

uint32_t stress_multiplier() {
  static const uint32_t multiplier = [] {
#if defined(_MSC_VER)
    char *raw_buffer = nullptr;
    size_t raw_size = 0;
    const auto error = _dupenv_s(
        &raw_buffer, &raw_size, "CSPLENDOR_NATIVE_STRESS_MULTIPLIER");
    const std::unique_ptr<char, decltype(&std::free)> raw_owner(raw_buffer,
                                                                &std::free);
    if (error != 0)
      throw std::runtime_error(
          "failed to read CSPLENDOR_NATIVE_STRESS_MULTIPLIER");
    const char *raw = raw_owner.get();
#else
    const char *raw = std::getenv("CSPLENDOR_NATIVE_STRESS_MULTIPLIER");
#endif
    if (!raw || *raw == '\0')
      return uint32_t{1};
    char *end = nullptr;
    const unsigned long parsed = std::strtoul(raw, &end, 10);
    if (!end || *end != '\0' || parsed == 0 || parsed > 20)
      throw std::invalid_argument(
          "CSPLENDOR_NATIVE_STRESS_MULTIPLIER must be in [1, 20]");
    return static_cast<uint32_t>(parsed);
  }();
  return multiplier;
}

void test_mixed_reservation_commit_abort(TreeBackend backend) {
  constexpr uint64_t generation = 73;
  constexpr int thread_count = 32;
  const int iterations = static_cast<int>(1000U * stress_multiplier());
  const uint64_t attempts = static_cast<uint64_t>(thread_count) * iterations;
  const std::array<size_t, 4> actions{3, 7, 11, 17};

  ConcurrentTree tree(generation, backend, MAX_TREE_SIZE, 16);
  const NodeHandle node = tree.find_or_create(exact_key(0x3003));
  ActionMask mask{};
  Policy policy{};
  for (size_t action : actions) {
    mask[action] = 1;
    policy[action] = 0.25f;
  }
  Value value{};
  tree.expand(node, policy, value, mask);

  auto ledger = std::make_shared<SearchLedger>();
  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> committed{0};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      try {
        SelectionContext context;
        context.cpuct = 1.5;
        context.forced_playouts = true;
        context.forced_playouts_k = 0.5;
        context.tree_generation = generation;
        ready.fetch_add(1, std::memory_order_release);
        spin_until(start);
        for (int iteration = 0; iteration < iterations; ++iteration) {
          auto reservation = tree.select_and_reserve(
              node, mask, context,
              static_cast<uint8_t>(worker % static_cast<int>(NUM_PLAYERS)),
              ledger);
          if (!reservation)
            throw std::runtime_error("mixed reservation selection failed");
          const uint64_t operation =
              static_cast<uint64_t>(worker) * iterations + iteration;
          if (operation % 5U == 0) {
            reservation->abort();
          } else if (operation % 5U == 1) {
            // Exercise the noexcept RAII rollback path.
          } else {
            const double backed_up =
                static_cast<double>(reservation->action()) / 100.0;
            reservation->commit(backed_up, generation);
            committed.fetch_add(1, std::memory_order_relaxed);
          }
        }
      } catch (...) {
        failed.store(true, std::memory_order_release);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto &worker : workers)
    worker.join();

  require(!failed.load(std::memory_order_acquire),
          "mixed reservation worker failed");
  const auto snapshot = tree.snapshot(node);
  const uint64_t committed_count = committed.load(std::memory_order_relaxed);
  require(snapshot.stats.total_visits == committed_count,
          "mixed reservation total visits lost a commit");
  require(std::accumulate(snapshot.stats.N.begin(), snapshot.stats.N.end(),
                          uint64_t{0}) == committed_count,
          "mixed reservation per-action visits are inconsistent");
  for (size_t action : actions) {
    require(snapshot.availability_count[action] == attempts,
            "mixed reservation availability count lost an observation");
    if (snapshot.stats.N[action] != 0) {
      const double expected = static_cast<double>(action) / 100.0;
      require(std::abs(snapshot.stats.Q[action] - expected) < 1e-12,
              "mixed reservation action mean is inconsistent");
    }
  }

  const auto counters = ledger->snapshot();
  require(counters.selected == attempts &&
              counters.reservations_committed == committed_count &&
              counters.reservations_aborted == attempts - committed_count &&
              counters.virtual_loss_added == attempts &&
              counters.virtual_loss_balanced() &&
              counters.integrity_errors == 0,
          "mixed reservation ledger is inconsistent");
  tree.validate_quiescent();
}

void test_bounded_queue_mpmc_contention() {
  constexpr uint64_t producer_count = 8;
  constexpr uint64_t consumer_count = 8;
  const uint64_t items_per_producer = 1000U * stress_multiplier();
  const uint64_t item_count = producer_count * items_per_producer;
  BoundedQueue<uint64_t> queue(7);
  auto seen = std::make_unique<std::atomic<uint8_t>[]>(item_count);
  for (uint64_t index = 0; index < item_count; ++index)
    seen[index].store(0, std::memory_order_relaxed);

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> consumed{0};
  std::vector<std::thread> consumers;
  std::vector<std::thread> producers;
  consumers.reserve(consumer_count);
  producers.reserve(producer_count);
  for (uint64_t consumer = 0; consumer < consumer_count; ++consumer) {
    consumers.emplace_back([&] {
      spin_until(start);
      uint64_t item = 0;
      while (queue.pop(item)) {
        if (item >= item_count ||
            seen[item].fetch_add(1, std::memory_order_relaxed) != 0)
          failed.store(true, std::memory_order_release);
        consumed.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (uint64_t producer = 0; producer < producer_count; ++producer) {
    producers.emplace_back([&, producer] {
      spin_until(start);
      const uint64_t begin = producer * items_per_producer;
      for (uint64_t offset = 0; offset < items_per_producer; ++offset) {
        if (!queue.push(begin + offset)) {
          failed.store(true, std::memory_order_release);
          return;
        }
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &producer : producers)
    producer.join();
  queue.close();
  for (auto &consumer : consumers)
    consumer.join();

  require(!failed.load(std::memory_order_acquire) &&
              consumed.load(std::memory_order_relaxed) == item_count &&
              queue.closed() && queue.size() == 0,
          "bounded MPMC queue lost or duplicated an item");
  for (uint64_t index = 0; index < item_count; ++index)
    require(seen[index].load(std::memory_order_relaxed) == 1,
            "bounded MPMC queue omitted an item");
}

void test_concurrent_tree_capacity_is_exact(TreeBackend backend) {
  constexpr uint64_t generation = 74;
  constexpr size_t capacity = 257;
  constexpr uint64_t candidate_count = 2048;
  constexpr int thread_count = 32;
  ConcurrentTree tree(generation, backend, capacity, 32);
  const TreeKey root_key = exact_key(0x4004);
  const NodeHandle root = tree.find_or_create(root_key);
  (void)root;

  std::atomic<int> ready{0};
  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> inserted{0};
  std::atomic<uint64_t> rejected{0};
  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (int worker = 0; worker < thread_count; ++worker) {
    workers.emplace_back([&, worker] {
      ready.fetch_add(1, std::memory_order_release);
      spin_until(start);
      for (uint64_t index = static_cast<uint64_t>(worker);
           index < candidate_count; index += thread_count) {
        try {
          (void)tree.find_or_create(exact_key(0x500000 + index));
          inserted.fetch_add(1, std::memory_order_relaxed);
        } catch (const TreeCapacityReachedError &) {
          rejected.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
          failed.store(true, std::memory_order_release);
        }
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != thread_count)
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  for (auto &worker : workers)
    worker.join();

  require(!failed.load(std::memory_order_acquire) && tree.size() == capacity &&
              inserted.load(std::memory_order_relaxed) == capacity - 1 &&
              rejected.load(std::memory_order_relaxed) ==
                  candidate_count - (capacity - 1),
          "concurrent tree capacity was underfilled or exceeded");
  require(tree.find_or_create(root_key).get() == root.get(),
          "a full tree rejected an existing key");
  std::unordered_set<uint64_t> hashes;
  for (const auto &snapshot : tree.snapshot_all())
    require(hashes.insert(snapshot.key.position_hash).second,
            "concurrent tree capacity created a duplicate key");
  tree.validate_quiescent();
}

void test_pending_attach_publish_race(TreeBackend backend) {
  constexpr int waiter_count = 16;
  const uint64_t rounds = 16U * stress_multiplier();
  ActionMask mask = one_action_mask(5);
  Policy policy = one_action_policy(5);
  std::array<float, FEATURE_SIZE> features{};
  features[9] = 0.75f;
  Value value{};
  value[0] = 0.125;

  for (uint64_t round = 0; round < rounds; ++round) {
    ConcurrentTree tree(8000 + round, backend, 8, 4);
    const NodeHandle node = tree.find_or_create(exact_key(0x600000 + round));
    auto ledger = std::make_shared<SearchLedger>();
    const ExpansionClaim owner =
        tree.claim_or_attach(node, 1, 0xa5a5, features, mask, ledger);
    require(owner.kind == ExpansionClaimKind::Owner && owner.pending,
            "pending race could not create an owner");

    std::array<ExpansionClaimKind, waiter_count> outcomes{};
    std::vector<uint64_t> published;
    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> waiters;
    waiters.reserve(waiter_count);
    for (int waiter = 0; waiter < waiter_count; ++waiter) {
      waiters.emplace_back([&, waiter] {
        try {
          spin_until(start);
          const ExpansionClaim claim =
              tree.claim_or_attach(node, static_cast<uint64_t>(100 + waiter),
                                   0xa5a5, features, mask, ledger);
          outcomes[static_cast<size_t>(waiter)] = claim.kind;
          if (claim.kind == ExpansionClaimKind::Waiter &&
              claim.pending.get() != owner.pending.get())
            throw std::runtime_error("pending waiter attached to wrong owner");
        } catch (...) {
          failed.store(true, std::memory_order_release);
        }
      });
    }
    std::thread publisher([&] {
      try {
        spin_until(start);
        published =
            tree.publish_evaluation(node, owner.pending, policy, value, ledger);
      } catch (...) {
        failed.store(true, std::memory_order_release);
      }
    });
    start.store(true, std::memory_order_release);
    for (auto &waiter : waiters)
      waiter.join();
    publisher.join();
    require(!failed.load(std::memory_order_acquire),
            "pending attach/publication race threw unexpectedly");

    std::vector<uint64_t> expected{1};
    uint64_t attached = 0;
    for (int waiter = 0; waiter < waiter_count; ++waiter) {
      const auto outcome = outcomes[static_cast<size_t>(waiter)];
      require(outcome == ExpansionClaimKind::Waiter ||
                  outcome == ExpansionClaimKind::Retry ||
                  outcome == ExpansionClaimKind::Expanded,
              "pending attach/publication returned an invalid outcome");
      if (outcome == ExpansionClaimKind::Waiter) {
        ++attached;
        expected.push_back(static_cast<uint64_t>(100 + waiter));
      }
    }
    std::sort(expected.begin(), expected.end());
    std::sort(published.begin(), published.end());
    require(published == expected,
            "pending publication omitted or invented an attached ticket");
    const auto counters = ledger->snapshot();
    const auto snapshot = tree.snapshot(node);
    require(counters.evaluation_owner == 1 &&
                counters.evaluation_waiter == attached &&
                counters.expansion_published == 1 &&
                counters.evaluation_requested == 1 &&
                counters.evaluated_boards == 1 &&
                snapshot.state == ExpansionState::Expanded &&
                !snapshot.has_pending_evaluation,
            "pending race ledger or state is inconsistent");
    tree.validate_quiescent();
  }
}

ParallelInferenceResult
evaluate_stress_request(const ParallelInferenceRequest &request) {
  ParallelInferenceResult result;
  size_t legal = 0;
  for (uint8_t available : request.owner_world_mask)
    legal += available != 0 ? 1U : 0U;
  if (legal != 0) {
    const float probability = 1.0f / static_cast<float>(legal);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      result.policy[action] =
          request.owner_world_mask[action] ? probability : 0.0f;
  }
  const float first =
      static_cast<float>(request.key.position_hash & 3ULL) / 8.0f;
  result.value = {first, -first};
  return result;
}

std::vector<ParallelInferenceResult>
evaluate_stress_batch(const std::vector<ParallelInferenceRequest> &requests) {
  std::vector<ParallelInferenceResult> results;
  results.reserve(requests.size());
  for (const auto &request : requests)
    results.push_back(evaluate_stress_request(request));
  return results;
}

MCTSConfig stress_config(bool determinization, bool root_noise) {
  MCTSConfig config;
  config.use_determinization = determinization;
  config.num_determinizations = 1;
  config.use_dirichlet_noise = root_noise;
  config.forced_playouts = true;
  config.num_simulations = 0;
  return config;
}

ParallelSearchOptions stress_search_options(uint32_t threads,
                                            TreeBackend backend,
                                            ParallelSearchMode mode,
                                            uint64_t seed,
                                            uint64_t simulations) {
  ParallelSearchOptions options;
  options.num_threads = threads;
  options.batch_size = 7;
  options.batch_wait_us = 50;
  options.max_inflight = std::max<uint32_t>(threads * 3U, 8U);
  options.deterministic_epoch_size = 13;
  options.num_simulations = simulations;
  options.master_seed = 0x0123456789abcdefULL ^ seed;
  options.search_nonce = 0x1020304050607080ULL + seed;
  options.simulation_id_base = seed * 1000U;
  options.evaluator_version = 0x7000 + seed;
  options.tree_backend = backend;
  options.shard_count = 32;
  options.mode = mode;
  return options;
}

uint64_t result_visit_sum(const ParallelSearchResult &result) {
  return std::accumulate(result.visits.begin(), result.visits.end(),
                         uint64_t{0});
}

void require_search_quiescent(const MCTS &mcts, const char *message) {
  for (const auto &node : mcts.get_parallel_tree_snapshot()) {
    const uint64_t virtual_loss =
        std::accumulate(node.stats.virtual_loss.begin(),
                        node.stats.virtual_loss.end(), uint64_t{0});
    if (virtual_loss != 0 || node.live_reservation_count != 0 ||
        node.has_pending_evaluation || node.state == ExpansionState::Evaluating)
      throw std::runtime_error(message);
  }
}

void test_parallel_search_matrix() {
  const uint32_t seed_count = 2U * stress_multiplier();
  for (uint64_t seed = 0; seed < seed_count; ++seed) {
    for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
      for (ParallelSearchMode mode : {ParallelSearchMode::Throughput,
                                      ParallelSearchMode::DeterministicEpoch}) {
        for (uint32_t threads : {2U, 8U, 16U}) {
          MCTS mcts(stress_config(seed % 2U != 0, seed % 3U == 0));
          ParallelMCTSSearcher searcher;
          const uint64_t budget = 96 + seed % 17U;
          const auto options = stress_search_options(
              threads, backend, mode, seed * 100U + threads, budget);
          const auto result = searcher.run(
              mcts, Game(90000 + seed), options,
              ParallelInferenceFunction(evaluate_stress_batch), 1.0f);
          const uint64_t expected_inflight_limit =
              mode == ParallelSearchMode::DeterministicEpoch
                  ? options.deterministic_epoch_size
                  : options.max_inflight;
          if (result.stop_reason != SearchStopReason::Completed ||
              result.partial || result.ledger.issued != budget ||
              result.ledger.completed() != budget ||
              result.ledger.cancelled != 0 || result.ledger.failed != 0 ||
              !result.ledger.virtual_loss_balanced() ||
              result.ledger.max_inflight_observed > expected_inflight_limit ||
              result_visit_sum(result) != budget ||
              mcts.is_parallel_search_active()) {
            throw std::runtime_error(
                "parallel search matrix failed: seed=" + std::to_string(seed) +
                " backend=" + std::to_string(static_cast<int>(backend)) +
                " mode=" + std::to_string(static_cast<int>(mode)) +
                " threads=" + std::to_string(threads) +
                " issued=" + std::to_string(result.ledger.issued) +
                " completed=" + std::to_string(result.ledger.completed()) +
                " visits=" + std::to_string(result_visit_sum(result)));
          }
          require_search_quiescent(
              mcts, "parallel search matrix retained transient tree state");
        }
      }
    }
  }
}

void test_callback_triggered_cancellation_and_reuse() {
  const uint32_t rounds = stress_multiplier();
  uint64_t case_id = 0;
  for (uint32_t round = 0; round < rounds; ++round) {
    for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
      for (ParallelSearchMode mode : {ParallelSearchMode::Throughput,
                                      ParallelSearchMode::DeterministicEpoch}) {
        for (uint32_t threads : {2U, 16U}) {
          MCTS mcts(stress_config(false, false));
          ParallelMCTSSearcher searcher;
          auto options = stress_search_options(threads, backend, mode,
                                               10000 + case_id, 512);
          const ParallelCancellationToken token = options.cancellation_token;
          std::atomic<uint64_t> calls{0};
          ParallelInferenceFunction cancelling =
              [&](const std::vector<ParallelInferenceRequest> &requests) {
                const uint64_t call =
                    calls.fetch_add(1, std::memory_order_relaxed) + 1;
                auto results = evaluate_stress_batch(requests);
                if (call == 3)
                  token.request_cancel();
                return results;
              };
          const auto cancelled =
              searcher.run(mcts, Game(100000 + case_id), options, cancelling);
          require(calls.load(std::memory_order_relaxed) >= 3 &&
                      cancelled.stop_reason == SearchStopReason::Cancelled &&
                      cancelled.partial && cancelled.ledger.issued < 512 &&
                      cancelled.ledger.issued ==
                          cancelled.ledger.completed() +
                              cancelled.ledger.cancelled +
                              cancelled.ledger.failed &&
                      cancelled.ledger.failed == 0 &&
                      cancelled.ledger.virtual_loss_balanced() &&
                      !mcts.is_parallel_search_active(),
                  "callback-triggered cancellation did not drain cleanly");
          require_search_quiescent(
              mcts, "cancelled search retained transient tree state");

          auto recovery = stress_search_options(threads, backend, mode,
                                                10000 + case_id, 24);
          recovery.evaluator_version = options.evaluator_version;
          const auto recovered =
              searcher.run(mcts, Game(100000 + case_id), recovery,
                           ParallelInferenceFunction(evaluate_stress_batch));
          require(recovered.stop_reason == SearchStopReason::Completed &&
                      !recovered.partial &&
                      recovered.ledger.completed() == 24 &&
                      recovered.ledger.virtual_loss_balanced(),
                  "MCTS was not reusable after callback cancellation");
          require_search_quiescent(
              mcts, "recovered search retained transient tree state");
          ++case_id;
        }
      }
    }
  }
}

void test_root_parallel_cancellation() {
  MCTSConfig config = stress_config(false, false);
  ParallelSearchOptions options = stress_search_options(
      1, TreeBackend::Sharded, ParallelSearchMode::RootParallel, 20000, 0);
  options.search_nonce = 77;
  constexpr uint32_t workers = 8;
  std::atomic<uint64_t> factory_calls{0};
  ParallelEvaluatorFactory factory =
      [&](uint32_t) -> ParallelInferenceFunction {
    factory_calls.fetch_add(1, std::memory_order_relaxed);
    return evaluate_stress_batch;
  };

  options.cancellation_token.request_cancel();
  const auto pre_cancelled =
      run_root_parallel(config, Game(120000), 256, workers, options, factory);
  require(pre_cancelled.merged.stop_reason == SearchStopReason::Cancelled &&
              pre_cancelled.merged.partial &&
              pre_cancelled.merged.ledger.issued == 0 &&
              factory_calls.load(std::memory_order_relaxed) == 0,
          "pre-cancelled root-parallel search invoked user code");

  options.cancellation_token = ParallelCancellationToken{};
  const ParallelCancellationToken token = options.cancellation_token;
  std::atomic<uint64_t> logical_calls{0};
  factory = [&](uint32_t) -> ParallelInferenceFunction {
    factory_calls.fetch_add(1, std::memory_order_relaxed);
    return [&, token](const std::vector<ParallelInferenceRequest> &requests) {
      const bool bootstrap =
          requests.size() == 1 && requests.front().owner_simulation_id ==
                                      std::numeric_limits<uint64_t>::max();
      if (!bootstrap &&
          logical_calls.fetch_add(1, std::memory_order_relaxed) + 1 == 4)
        token.request_cancel();
      return evaluate_stress_batch(requests);
    };
  };
  const auto cancelled =
      run_root_parallel(config, Game(120001), 256, workers, options, factory);
  require(cancelled.merged.stop_reason == SearchStopReason::Cancelled &&
              cancelled.merged.partial &&
              cancelled.merged.tree_generation == 1 &&
              cancelled.merged.ledger.issued ==
                  cancelled.merged.ledger.completed() +
                      cancelled.merged.ledger.cancelled +
                      cancelled.merged.ledger.failed &&
              cancelled.merged.ledger.failed == 0 &&
              cancelled.merged.ledger.virtual_loss_balanced() &&
              result_visit_sum(cancelled.merged) ==
                  cancelled.merged.ledger.completed() &&
              logical_calls.load(std::memory_order_relaxed) >= 4,
          "in-flight root-parallel cancellation did not drain cleanly");
  for (const auto &worker : cancelled.workers)
    require(worker.ledger.virtual_loss_balanced() &&
                worker.ledger.issued == worker.ledger.completed() +
                                            worker.ledger.cancelled +
                                            worker.ledger.failed,
            "cancelled root worker retained an unbalanced ledger");
}

} // namespace

int main() {
  try {
    for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
      test_shared_edge_reserve_commit(backend);
      test_same_leaf_claim_dedup(backend);
      test_mixed_reservation_commit_abort(backend);
      test_concurrent_tree_capacity_is_exact(backend);
      test_pending_attach_publish_race(backend);
    }
    test_bounded_queue_mpmc_contention();
    test_parallel_search_matrix();
    test_callback_triggered_cancellation_and_reuse();
    test_root_parallel_cancellation();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mcts_parallel_stress: " << error.what() << '\n';
    return 1;
  }
}
