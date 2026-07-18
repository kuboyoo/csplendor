#include "mcts_parallel_searcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mcts_parallel;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

ParallelInferenceResult
evaluate_request(const ParallelInferenceRequest &request) {
  ParallelInferenceResult result;
  size_t count = 0;
  for (uint8_t available : request.owner_world_mask)
    count += available ? 1 : 0;
  if (count != 0) {
    const float probability = 1.0f / static_cast<float>(count);
    for (size_t action = 0; action < MAX_ACTIONS; ++action)
      result.policy[action] =
          request.owner_world_mask[action] ? probability : 0.0f;
  }
  result.value.fill(0.0f);
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

MCTSConfig search_config() {
  MCTSConfig config;
  config.use_determinization = false;
  config.use_dirichlet_noise = false;
  config.forced_playouts = false;
  config.num_simulations = 32;
  return config;
}

ParallelSearchOptions search_options(uint32_t threads, TreeBackend backend,
                                     uint64_t simulations = 32) {
  ParallelSearchOptions options;
  options.num_threads = threads;
  options.batch_size = 4;
  options.batch_wait_us = 100;
  options.max_inflight = std::max<uint32_t>(threads, threads * 2);
  options.num_simulations = simulations;
  options.master_seed = 12345;
  options.search_nonce = 7;
  options.evaluator_version = 1;
  options.tree_backend = backend;
  options.shard_count = 8;
  return options;
}

uint64_t sum_visits(const ParallelSearchResult &result) {
  return std::accumulate(result.visits.begin(), result.visits.end(),
                         uint64_t{0});
}

Game hidden_reserve_determinization_fixture() {
  Game root(42);
  root.board.reset();

  // Player 0 has exactly one legal move (take colours 0, 1, and 2).  Keeping
  // the root branch forced makes every determinized ticket reach the same
  // public leaf, independent of worker scheduling.
  root.board.bank = {2, 2, 2, 0, 0, 0};
  auto &player_zero = root.board.players[0];
  player_zero.reserved = {87, 88, 89};
  player_zero.reserved_count = 3;
  player_zero.sync_packed();

  // From observer 0's perspective player 1's tier-1 reserve is hidden.  The
  // portable determinization swaps card 0 and card 1 between this slot and
  // the one-card deck.  Player 1 can buy card 0, but not card 1, so action 42
  // is deliberately world-local under one observable TreeKey.
  auto &player_one = root.board.players[1];
  player_one.reserved = {0, -1, -1};
  player_one.reserved_is_hidden = {true, false, false};
  player_one.reserved_count = 1;
  for (size_t colour = 0; colour < 5; ++colour)
    player_one.gems[colour] = get_card(0).cost[colour];
  player_one.sync_packed();
  root.board.decks[0].push_back(1);
  root.board.invalidate_hash();

  const ActionMask root_mask =
      mcts_internal::GameAdapter::native_action_mask(root);
  require(root_mask[0] == 1 && std::accumulate(root_mask.begin(),
                                               root_mask.end(), size_t{0}) == 1,
          "hidden-reserve fixture root is not a forced branch");
  return root;
}

struct DeterminizedLeaf {
  TreeKey key{};
  ActionMask mask{};
  std::array<float, FEATURE_SIZE> features{};
};

DeterminizedLeaf determinized_leaf_for(const Game &root,
                                       const ParallelSearchOptions &options,
                                       uint64_t simulation_id) {
  constexpr uint8_t observer = 0;
  const TreeKey root_key =
      mcts_internal::GameAdapter::tree_key(root, observer, true);
  const SearchRandomContext random{
      options.master_seed.value(), root_key, options.search_nonce,
      options.simulation_id_base, MCTS_RNG_VERSION};
  Game world = mcts_internal::GameAdapter::determinize_portable(
      root, observer,
      random.seed_for(SearchRandomDomain::RootDeterminization, simulation_id));
  require(mcts_internal::GameAdapter::decode_and_apply_native(world, 0),
          "forced root action did not replay in a determinized world");
  DeterminizedLeaf leaf;
  leaf.key = mcts_internal::GameAdapter::tree_key(world, observer, true);
  leaf.mask = mcts_internal::GameAdapter::native_action_mask(world);
  leaf.features = mcts_internal::GameAdapter::native_features(world, observer);
  return leaf;
}

ParallelInferenceResult
evaluate_unmasked_hidden_request(const ParallelInferenceRequest &request) {
  ParallelInferenceResult result;
  // This is intentionally an unmasked 48-action network policy.  In
  // particular, action 42 retains its prior even when the pending owner lives
  // in the world where that hidden card cannot be purchased.
  result.policy.fill(0.001f);
  result.policy[42] = 1.0f;
  result.value.fill(0.0f);
  require(std::any_of(request.owner_world_mask.begin(),
                      request.owner_world_mask.end(),
                      [](uint8_t available) { return available == 0; }),
          "hidden evaluator unexpectedly received an all-legal mask");
  return result;
}

TreeKey root_key_for(const Game &root, const MCTSConfig &config) {
  const uint8_t observer =
      static_cast<uint8_t>(mcts_internal::GameAdapter::current_player(root));
  return mcts_internal::GameAdapter::tree_key(root, observer,
                                              config.use_determinization);
}

struct TreeFingerprint {
  uint64_t generation = 0;
  size_t size = 0;
  ExpansionState root_state = ExpansionState::Unexpanded;
  uint64_t root_visits = 0;
  std::array<uint64_t, MAX_ACTIONS> visits{};
  std::array<double, MAX_ACTIONS> q_values{};
  Policy base_policy{};
  Value value{};
};

TreeFingerprint capture_tree(const MCTS &mcts, const TreeKey &root_key) {
  const auto root = mcts.get_parallel_node_snapshot(root_key);
  require(root.has_value(), "parallel root snapshot is missing");
  TreeFingerprint result;
  result.generation = mcts.tree_generation_snapshot();
  result.size = mcts.parallel_tree_size();
  result.root_state = root->state;
  result.root_visits = root->stats.total_visits;
  result.visits = root->stats.N;
  result.q_values = root->stats.Q;
  result.base_policy = root->base_policy;
  result.value = root->stats.value;
  return result;
}

bool operator==(const TreeFingerprint &left, const TreeFingerprint &right) {
  return left.generation == right.generation && left.size == right.size &&
         left.root_state == right.root_state &&
         left.root_visits == right.root_visits && left.visits == right.visits &&
         left.q_values == right.q_values &&
         left.base_policy == right.base_policy && left.value == right.value;
}

struct CallbackProbe {
  std::atomic<int> active{0};
  std::atomic<int> max_active{0};
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> boards{0};

  void enter(size_t batch_size) {
    const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
    int observed = max_active.load(std::memory_order_relaxed);
    while (observed < now && !max_active.compare_exchange_weak(
                                 observed, now, std::memory_order_relaxed,
                                 std::memory_order_relaxed)) {
    }
    calls.fetch_add(1, std::memory_order_relaxed);
    boards.fetch_add(static_cast<uint64_t>(batch_size),
                     std::memory_order_relaxed);
  }

  void leave() { active.fetch_sub(1, std::memory_order_acq_rel); }
};

void require_quiescent_snapshots(const MCTS &mcts, const char *message) {
  for (const auto &node : mcts.get_parallel_tree_snapshot()) {
    const bool virtual_loss_is_zero = std::all_of(
        node.stats.virtual_loss.begin(), node.stats.virtual_loss.end(),
        [](uint64_t value) { return value == 0; });
    if (!virtual_loss_is_zero || node.live_reservation_count != 0 ||
        node.has_pending_evaluation || node.state == ExpansionState::Evaluating)
      throw std::runtime_error(message);
  }
}

void test_bounded_queue_close_and_wakeup() {
  try {
    BoundedQueue<int> invalid(0);
    (void)invalid;
    throw std::runtime_error("zero-capacity bounded queue was accepted");
  } catch (const std::invalid_argument &) {
  }

  BoundedQueue<int> producer_queue(1);
  require(producer_queue.push(1), "initial queue push failed");
  std::atomic<bool> producer_entered{false};
  std::atomic<bool> producer_result{true};
  std::thread producer([&] {
    producer_entered.store(true, std::memory_order_release);
    producer_result.store(producer_queue.push(2), std::memory_order_release);
  });
  while (!producer_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  producer_queue.close();
  producer_queue.close();
  producer.join();
  require(!producer_result.load(std::memory_order_acquire),
          "close did not wake a blocked producer");
  int value = 0;
  require(producer_queue.pop(value) && value == 1,
          "queued item was not drainable after close");
  require(!producer_queue.pop(value),
          "closed drained queue unexpectedly produced an item");

  BoundedQueue<int> consumer_queue(1);
  std::atomic<bool> consumer_entered{false};
  std::atomic<bool> consumer_result{true};
  std::thread consumer([&] {
    consumer_entered.store(true, std::memory_order_release);
    int consumed = 0;
    consumer_result.store(consumer_queue.pop(consumed),
                          std::memory_order_release);
  });
  while (!consumer_entered.load(std::memory_order_acquire))
    std::this_thread::yield();
  consumer_queue.close();
  consumer.join();
  require(!consumer_result.load(std::memory_order_acquire),
          "close did not wake a blocked consumer");

  BoundedQueue<int> timed_queue(1);
  const auto started = std::chrono::steady_clock::now();
  require(!timed_queue.pop_for(value, std::chrono::milliseconds(2)),
          "empty timed queue unexpectedly produced an item");
  require(std::chrono::steady_clock::now() >= started,
          "steady clock moved backwards");
}

void test_fake_evaluator_search(uint32_t threads, TreeBackend backend) {
  MCTS mcts(search_config());
  Game root(42);
  ParallelMCTSSearcher searcher;
  auto probe = std::make_shared<CallbackProbe>();
  const std::thread::id caller_thread = std::this_thread::get_id();
  std::atomic<bool> callback_thread_consistent{true};
  ParallelInferenceFunction inference =
      [probe, caller_thread, &callback_thread_consistent](
          const std::vector<ParallelInferenceRequest> &requests) {
        if (std::this_thread::get_id() != caller_thread)
          callback_thread_consistent.store(false, std::memory_order_release);
        probe->enter(requests.size());
        auto results = evaluate_batch(requests);
        probe->leave();
        return results;
      };

  constexpr uint64_t budget = 32;
  const ParallelSearchOptions options =
      search_options(threads, backend, budget);
  const ParallelSearchResult result =
      searcher.run(mcts, root, options, inference, 1.0f);
  require(result.stop_reason == SearchStopReason::Completed && !result.partial,
          "fake evaluator search did not complete");
  require(result.ledger.issued == budget,
          "fake evaluator issued count differs from budget");
  require(result.ledger.completed() == budget && result.ledger.cancelled == 0 &&
              result.ledger.failed == 0,
          "fake evaluator completion ledger is inconsistent");
  require(result.ledger.virtual_loss_balanced(),
          "fake evaluator leaked virtual loss");
  require(result.ledger.reservations_committed == result.ledger.selected,
          "fake evaluator did not commit every selected edge");
  require(result.ledger.max_inflight_observed <= options.max_inflight,
          "fake evaluator exceeded max_inflight");
  require(sum_visits(result) == budget,
          "cold-tree root visit sum differs from budget");
  require(probe->active.load(std::memory_order_acquire) == 0 &&
              probe->max_active.load(std::memory_order_acquire) == 1,
          "inference callback ran concurrently");
  require(callback_thread_consistent.load(std::memory_order_acquire),
          "inference callback changed coordinator thread");
  require(probe->calls.load(std::memory_order_relaxed) >= 1 &&
              probe->boards.load(std::memory_order_relaxed) >= 1,
          "inference callback was not invoked");
  require(probe->boards.load(std::memory_order_relaxed) ==
              result.ledger.evaluated_boards + 1,
          "callback boards disagree with the bootstrap/dedup ledger");
  require(!mcts.is_parallel_search_active(),
          "completed search retained the active lifecycle state");
}

void test_active_mutation_rejected() {
  MCTS mcts(search_config());
  Game root(43);
  ParallelMCTSSearcher searcher;
  std::atomic<bool> saw_active{false};
  std::atomic<bool> clear_rejected{false};
  std::atomic<bool> config_rejected{false};
  ParallelInferenceFunction inference =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        saw_active.store(mcts.is_parallel_search_active(),
                         std::memory_order_release);
        try {
          mcts.clear();
        } catch (const std::logic_error &) {
          clear_rejected.store(true, std::memory_order_release);
        }
        try {
          MCTSConfig replacement = mcts.get_config_snapshot();
          replacement.cpuct += 1.0f;
          mcts.set_config(replacement);
        } catch (const std::logic_error &) {
          config_rejected.store(true, std::memory_order_release);
        }
        return evaluate_batch(requests);
      };
  auto options = search_options(2, TreeBackend::Coarse, 8);
  const auto result = searcher.run(mcts, root, options, inference);
  require(result.ledger.completed() == 8,
          "active-mutation search did not complete");
  require(saw_active.load(std::memory_order_acquire) &&
              clear_rejected.load(std::memory_order_acquire) &&
              config_rejected.load(std::memory_order_acquire),
          "active search accepted clear or config mutation");
}

void test_callback_exception_cleanup_and_reuse() {
  MCTS mcts(search_config());
  Game root(44);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Sharded, 24);
  options.evaluator_version = 9;
  std::atomic<uint64_t> calls{0};
  ParallelInferenceFunction failing =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 2)
          throw std::runtime_error("intentional evaluator failure");
        return evaluate_batch(requests);
      };
  bool preserved_exception = false;
  try {
    (void)searcher.run(mcts, root, options, failing);
  } catch (const std::runtime_error &error) {
    preserved_exception =
        std::string(error.what()) == "intentional evaluator failure";
  }
  require(preserved_exception && !mcts.is_parallel_search_active(),
          "callback failure did not clean up the search session");

  ParallelInferenceFunction healthy = evaluate_batch;
  const auto recovered = searcher.run(mcts, root, options, healthy);
  require(recovered.stop_reason == SearchStopReason::Completed &&
              recovered.ledger.completed() == options.num_simulations &&
              recovered.ledger.virtual_loss_balanced(),
          "MCTS was not reusable after callback failure");
}

void test_malformed_result_cleanup_and_reuse() {
  MCTS mcts(search_config());
  Game root(45);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Coarse, 24);
  options.evaluator_version = 10;
  std::atomic<uint64_t> calls{0};
  ParallelInferenceFunction malformed =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 2)
          return std::vector<ParallelInferenceResult>{};
        return evaluate_batch(requests);
      };
  bool threw = false;
  try {
    (void)searcher.run(mcts, root, options, malformed);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  require(threw && !mcts.is_parallel_search_active(),
          "malformed result did not clean up the search session");

  const uint8_t observer =
      static_cast<uint8_t>(mcts_internal::GameAdapter::current_player(root));
  const TreeKey root_key =
      mcts_internal::GameAdapter::tree_key(root, observer, false);
  auto root_after_count_error = mcts.get_parallel_node_snapshot(root_key);
  require(root_after_count_error &&
              root_after_count_error->stats.total_visits == 0,
          "malformed count partially mutated root statistics");

  ParallelInferenceFunction nonfinite =
      [](const std::vector<ParallelInferenceRequest> &requests) {
        auto results = evaluate_batch(requests);
        require(!results.empty(), "nonfinite test received an empty batch");
        results.front().policy[0] = std::numeric_limits<float>::quiet_NaN();
        return results;
      };
  threw = false;
  try {
    (void)searcher.run(mcts, root, options, nonfinite);
  } catch (const std::invalid_argument &) {
    threw = true;
  }
  require(threw && !mcts.is_parallel_search_active(),
          "nonfinite result did not clean up the search session");
  auto root_after_nonfinite = mcts.get_parallel_node_snapshot(root_key);
  require(root_after_nonfinite && root_after_nonfinite->stats.total_visits == 0,
          "nonfinite result partially mutated root statistics");

  const auto recovered = searcher.run(
      mcts, root, options, ParallelInferenceFunction(evaluate_batch));
  require(recovered.stop_reason == SearchStopReason::Completed &&
              recovered.ledger.completed() == options.num_simulations &&
              recovered.ledger.virtual_loss_balanced(),
          "MCTS was not reusable after malformed result");
}

void test_timeout_returns_balanced_partial_result() {
  MCTS mcts(search_config());
  Game root(46);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Sharded, 128);
  options.batch_size = 1;
  options.max_inflight = 8;
  options.timeout_ms = 1;
  options.evaluator_version = 11;
  std::atomic<uint64_t> calls{0};
  ParallelInferenceFunction slow =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call >= 2)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return evaluate_batch(requests);
      };
  const auto partial = searcher.run(mcts, root, options, slow);
  require(partial.stop_reason == SearchStopReason::TimedOut && partial.partial,
          "timeout did not return a partial timed-out result");
  require(partial.ledger.issued == partial.ledger.completed() +
                                       partial.ledger.cancelled +
                                       partial.ledger.failed,
          "timeout ticket ledger is unbalanced");
  require(partial.ledger.failed == 0 && partial.ledger.virtual_loss_balanced(),
          "timeout cleanup leaked reservations");
  require(sum_visits(partial) == partial.ledger.completed(),
          "timeout root visits differ from completed tickets");
  require(!mcts.is_parallel_search_active(),
          "timeout retained the active lifecycle state");

  options.num_simulations = 8;
  options.timeout_ms = 0;
  const auto recovered = searcher.run(
      mcts, root, options, ParallelInferenceFunction(evaluate_batch));
  require(recovered.stop_reason == SearchStopReason::Completed &&
              recovered.ledger.completed() == 8 &&
              recovered.ledger.virtual_loss_balanced(),
          "MCTS was not reusable after timeout cleanup");
}

void test_sharded_layout_reuse_and_reset() {
  const MCTSConfig config = search_config();
  MCTS mcts(config);
  Game root(47);
  const TreeKey root_key = root_key_for(root, config);
  ParallelMCTSSearcher searcher;
  auto options = search_options(1, TreeBackend::Sharded, 4);
  options.shard_count = 4;
  options.evaluator_version = 12;

  const auto first = searcher.run(mcts, root, options,
                                  ParallelInferenceFunction(evaluate_batch));
  const auto first_root = mcts.get_parallel_node_snapshot(root_key);
  require(first_root && first_root->stats.total_visits == 4,
          "first sharded search did not populate the root");
  const uint64_t first_generation = first.tree_generation;

  const auto reused = searcher.run(mcts, root, options,
                                   ParallelInferenceFunction(evaluate_batch));
  const auto reused_root = mcts.get_parallel_node_snapshot(root_key);
  require(reused.tree_generation == first_generation && reused_root &&
              reused_root->stats.total_visits == 8,
          "an equal sharded layout replaced the existing tree");

  options.shard_count = 8;
  const auto replaced = searcher.run(mcts, root, options,
                                     ParallelInferenceFunction(evaluate_batch));
  const auto replaced_root = mcts.get_parallel_node_snapshot(root_key);
  require(replaced.tree_generation == first_generation + 1 &&
              mcts.tree_generation_snapshot() == first_generation + 1 &&
              replaced_root && replaced_root->stats.total_visits == 4,
          "a changed sharded layout did not reset/generate a new tree");
}

void test_entry_validation_is_transactional() {
  const MCTSConfig config = search_config();
  MCTS mcts(config);
  Game root(48);
  const TreeKey root_key = root_key_for(root, config);
  ParallelMCTSSearcher searcher;
  auto valid = search_options(1, TreeBackend::Sharded, 4);
  valid.shard_count = 8;
  valid.evaluator_version = 13;
  (void)searcher.run(mcts, root, valid,
                     ParallelInferenceFunction(evaluate_batch));
  const TreeFingerprint before = capture_tree(mcts, root_key);

  std::atomic<uint64_t> callbacks{0};
  ParallelInferenceFunction counted =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        return evaluate_batch(requests);
      };
  auto require_unchanged = [&] {
    require(!mcts.is_parallel_search_active(),
            "invalid entry retained the active search state");
    require(callbacks.load(std::memory_order_relaxed) == 0,
            "invalid entry invoked the evaluator");
    require(capture_tree(mcts, root_key) == before,
            "invalid entry mutated tree generation or content");
  };

  auto invalid_options = valid;
  invalid_options.num_threads = 0;
  bool rejected = false;
  try {
    (void)searcher.run(mcts, root, invalid_options, counted);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "invalid options were accepted");
  require_unchanged();

  MCTSConfig invalid_config = config;
  invalid_config.cpuct = std::numeric_limits<float>::quiet_NaN();
  mcts.set_config(invalid_config);
  rejected = false;
  try {
    (void)searcher.run(mcts, root, valid, counted);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "invalid config was accepted");
  require_unchanged();
  mcts.set_config(config);

  rejected = false;
  try {
    (void)searcher.run(mcts, root, valid, counted,
                       std::numeric_limits<float>::quiet_NaN());
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "non-finite temperature was accepted");
  require_unchanged();

  auto overflowing = valid;
  overflowing.num_simulations = 2;
  overflowing.simulation_id_base = std::numeric_limits<uint64_t>::max() - 1;
  bool overflow_rejected = false;
  try {
    (void)searcher.run(mcts, root, overflowing, counted);
  } catch (const std::overflow_error &) {
    overflow_rejected = true;
  }
  require(overflow_rejected, "wrapping simulation ID range was accepted");
  require_unchanged();
}

void test_evaluator_overflow_is_rethrown_and_reusable() {
  MCTS mcts(search_config());
  Game root(49);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Sharded, 24);
  options.evaluator_version = 14;
  std::atomic<uint64_t> calls{0};
  ParallelInferenceFunction overflowing =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        if (calls.fetch_add(1, std::memory_order_relaxed) + 1 == 2)
          throw std::overflow_error("intentional evaluator overflow");
        return evaluate_batch(requests);
      };

  bool preserved = false;
  try {
    (void)searcher.run(mcts, root, options, overflowing);
  } catch (const std::overflow_error &error) {
    preserved = std::string(error.what()) == "intentional evaluator overflow";
  }
  require(preserved && !mcts.is_parallel_search_active(),
          "evaluator overflow was mistaken for tree capacity or leaked state");

  const auto recovered = searcher.run(
      mcts, root, options, ParallelInferenceFunction(evaluate_batch));
  require(recovered.stop_reason == SearchStopReason::Completed &&
              !recovered.partial &&
              recovered.ledger.completed() == options.num_simulations &&
              recovered.ledger.virtual_loss_balanced(),
          "MCTS was not reusable after evaluator overflow");
}

void test_capacity_named_callback_exception_is_rethrown_and_reusable() {
  uint64_t case_id = 0;
  for (ParallelSearchMode mode : {ParallelSearchMode::Throughput,
                                  ParallelSearchMode::DeterministicEpoch}) {
    for (uint64_t throw_on_call : {uint64_t{1}, uint64_t{2}}) {
      const bool bootstrap_failure = throw_on_call == 1;
      const char *expected = bootstrap_failure
                                 ? "intentional bootstrap callback capacity"
                                 : "intentional batch callback capacity";
      MCTS mcts(search_config());
      Game root(500 + case_id);
      ParallelMCTSSearcher searcher;
      auto options = search_options(4, TreeBackend::Sharded, 24);
      options.mode = mode;
      options.deterministic_epoch_size = 8;
      options.evaluator_version = 140 + case_id;
      std::atomic<uint64_t> calls{0};
      ParallelInferenceFunction failing =
          [&](const std::vector<ParallelInferenceRequest> &requests) {
            if (calls.fetch_add(1, std::memory_order_relaxed) + 1 ==
                throw_on_call)
              throw TreeCapacityReachedError(expected);
            return evaluate_batch(requests);
          };

      bool preserved = false;
      try {
        (void)searcher.run(mcts, root, options, failing);
      } catch (const TreeCapacityReachedError &error) {
        preserved = std::string(error.what()) == expected;
      }
      require(preserved &&
                  calls.load(std::memory_order_relaxed) >= throw_on_call &&
                  !mcts.is_parallel_search_active(),
              "callback capacity exception was classified as tree capacity");
      require_quiescent_snapshots(
          mcts,
          "callback capacity failure retained pending work or virtual loss");

      const auto recovered = searcher.run(
          mcts, root, options, ParallelInferenceFunction(evaluate_batch));
      require(recovered.stop_reason == SearchStopReason::Completed &&
                  !recovered.partial &&
                  recovered.ledger.completed() == options.num_simulations &&
                  recovered.ledger.virtual_loss_balanced() &&
                  !mcts.is_parallel_search_active(),
              "MCTS was not reusable after callback capacity failure");
      ++case_id;
    }
  }
}

void test_zero_resolved_budget_skips_evaluator() {
  for (ParallelSearchMode mode : {ParallelSearchMode::Throughput,
                                  ParallelSearchMode::DeterministicEpoch}) {
    MCTSConfig config = search_config();
    config.num_simulations = 0;
    MCTS mcts(config);
    Game root(50);
    ParallelMCTSSearcher searcher;
    auto options = search_options(2, TreeBackend::Sharded, 0);
    options.mode = mode;
    options.num_simulations = 0;
    options.evaluator_version = 15;
    std::atomic<uint64_t> callbacks{0};
    ParallelInferenceFunction inference =
        [&](const std::vector<ParallelInferenceRequest> &requests) {
          callbacks.fetch_add(1, std::memory_order_relaxed);
          return evaluate_batch(requests);
        };
    const uint64_t generation_before = mcts.tree_generation_snapshot();
    const auto result = searcher.run(mcts, root, options, inference);
    require(result.stop_reason == SearchStopReason::Completed &&
                result.ledger.issued == 0 && result.ledger.completed() == 0 &&
                result.tree_size == 0 &&
                callbacks.load(std::memory_order_relaxed) == 0 &&
                mcts.tree_generation_snapshot() == generation_before &&
                !mcts.is_parallel_search_active(),
            "zero resolved budget performed inference or retained state");

    options.num_simulations = 1;
    options.evaluator_version = 16;
    const auto recovered = searcher.run(mcts, root, options, inference);
    require(recovered.ledger.completed() == 1,
            "zero budget bound the tree to an unused evaluator version");
  }
}

void test_pre_cancel_is_side_effect_free() {
  MCTS mcts(search_config());
  Game root(51);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Coarse, 32);
  options.evaluator_version = 17;
  options.cancellation_token.request_cancel();
  std::atomic<uint64_t> callbacks{0};
  ParallelInferenceFunction inference =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        callbacks.fetch_add(1, std::memory_order_relaxed);
        return evaluate_batch(requests);
      };

  const uint64_t generation_before = mcts.tree_generation_snapshot();
  const auto result = searcher.run(mcts, root, options, inference);
  require(result.stop_reason == SearchStopReason::Cancelled && result.partial &&
              result.ledger.issued == 0 && result.tree_size == 0 &&
              callbacks.load(std::memory_order_relaxed) == 0 &&
              mcts.tree_generation_snapshot() == generation_before &&
              !mcts.is_parallel_search_active(),
          "pre-cancelled search performed work or retained lifecycle state");
}

void test_root_noise_seed_is_storage_domain_independent() {
  MCTSConfig exact_config = search_config();
  exact_config.use_dirichlet_noise = true;
  exact_config.use_determinization = false;
  MCTSConfig observable_config = exact_config;
  observable_config.use_determinization = true;
  observable_config.num_determinizations = 1;
  Game root(56);

  auto options = search_options(1, TreeBackend::Coarse, 1);
  options.mode = ParallelSearchMode::DeterministicEpoch;
  options.deterministic_epoch_size = 1;
  options.master_seed = 0x123456789abcdef0ULL;
  options.search_nonce = 0x0fedcba987654321ULL;
  options.evaluator_version = 21;

  ParallelMCTSSearcher searcher;
  MCTS exact(exact_config);
  MCTS observable(observable_config);
  DeterministicTrace exact_trace;
  DeterministicTrace observable_trace;
  (void)searcher.run(exact, root, options,
                     ParallelInferenceFunction(evaluate_batch), 1.0f,
                     &exact_trace);
  (void)searcher.run(observable, root, options,
                     ParallelInferenceFunction(evaluate_batch), 1.0f,
                     &observable_trace);
  require(exact_trace.root_key.domain == TreeDomain::Exact &&
              observable_trace.root_key.domain == TreeDomain::Observable &&
              exact_trace.has_root_noise && observable_trace.has_root_noise &&
              exact_trace.root_noise == observable_trace.root_noise,
          "root Dirichlet seed changed with the tree storage domain");
}

void test_inflight_cooperative_cancellation(ParallelSearchMode mode) {
  MCTS mcts(search_config());
  Game root(mode == ParallelSearchMode::Throughput ? 52 : 53);
  ParallelMCTSSearcher searcher;
  auto options =
      search_options(mode == ParallelSearchMode::Throughput ? 4U : 2U,
                     TreeBackend::Sharded, 128);
  options.mode = mode;
  options.batch_size = 16;
  options.max_inflight = 32;
  options.deterministic_epoch_size = 32;
  options.evaluator_version = mode == ParallelSearchMode::Throughput ? 18 : 19;

  const ParallelCancellationToken token = options.cancellation_token;
  std::atomic<uint64_t> calls{0};
  std::atomic<bool> simulation_callback_entered{false};
  ParallelInferenceFunction inference =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        const uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (call == 2) {
          simulation_callback_entered.store(true, std::memory_order_release);
          const auto wait_deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(5);
          while (!token.is_cancelled() &&
                 std::chrono::steady_clock::now() < wait_deadline)
            std::this_thread::yield();
        }
        return evaluate_batch(requests);
      };

  ParallelSearchResult result;
  std::exception_ptr failure;
  std::thread search_thread([&] {
    try {
      result = searcher.run(mcts, root, options, inference);
    } catch (...) {
      failure = std::current_exception();
    }
  });

  const auto entry_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!simulation_callback_entered.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < entry_deadline)
    std::this_thread::yield();
  const bool callback_was_entered =
      simulation_callback_entered.load(std::memory_order_acquire);
  token.request_cancel();
  search_thread.join();
  if (failure)
    std::rethrow_exception(failure);

  require(callback_was_entered,
          "cancellation test never reached an in-flight evaluation");
  require(result.stop_reason == SearchStopReason::Cancelled && result.partial &&
              result.ledger.issued > 0 && result.ledger.cancelled > 0 &&
              result.ledger.failed == 0 &&
              result.ledger.issued ==
                  result.ledger.completed() + result.ledger.cancelled &&
              result.ledger.virtual_loss_balanced() &&
              result.ledger.reservations_aborted +
                      result.ledger.reservations_committed ==
                  result.ledger.selected &&
              !mcts.is_parallel_search_active(),
          "cooperative cancellation returned an unbalanced partial result");
  require_quiescent_snapshots(
      mcts, "cooperative cancellation retained pending work or virtual loss");

  options.cancellation_token = ParallelCancellationToken{};
  options.num_simulations = 8;
  options.max_inflight = 8;
  options.deterministic_epoch_size = 8;
  const auto recovered = searcher.run(
      mcts, root, options, ParallelInferenceFunction(evaluate_batch));
  require(recovered.stop_reason == SearchStopReason::Completed &&
              recovered.ledger.completed() == options.num_simulations &&
              recovered.ledger.virtual_loss_balanced(),
          "MCTS was not reusable after cooperative cancellation");
}

void test_huge_budget_uses_bounded_ticket_storage(ParallelSearchMode mode) {
  MCTS mcts(search_config());
  Game root(mode == ParallelSearchMode::Throughput ? 57 : 58);
  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Sharded,
                                std::numeric_limits<uint64_t>::max() / 2);
  options.mode = mode;
  options.batch_size = 4;
  options.max_inflight = 4;
  options.deterministic_epoch_size = 4;
  options.evaluator_version = mode == ParallelSearchMode::Throughput ? 22 : 23;

  const ParallelCancellationToken token = options.cancellation_token;
  std::atomic<uint64_t> calls{0};
  ParallelInferenceFunction cancelling =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        if (calls.fetch_add(1, std::memory_order_relaxed) + 1 == 2)
          token.request_cancel();
        return evaluate_batch(requests);
      };

  const auto result = searcher.run(mcts, root, options, cancelling);
  require(calls.load(std::memory_order_relaxed) >= 2,
          "huge-budget test did not reach a simulation callback");
  require(result.stop_reason == SearchStopReason::Cancelled && result.partial &&
              result.ledger.issued > 0 &&
              result.ledger.issued <= options.max_inflight &&
              result.ledger.cancelled > 0 && result.ledger.failed == 0 &&
              result.ledger.issued ==
                  result.ledger.completed() + result.ledger.cancelled &&
              result.ledger.virtual_loss_balanced() &&
              result.ledger.reservations_aborted +
                      result.ledger.reservations_committed ==
                  result.ledger.selected &&
              !mcts.is_parallel_search_active(),
          "huge-budget cancellation returned an unbalanced partial result");
  require_quiescent_snapshots(
      mcts, "huge-budget cancellation retained pending work or virtual loss");
}

void test_capacity_partial_and_root_failure() {
  MCTSConfig config = search_config();
  config.use_dirichlet_noise = true;
  MCTS mcts(config);
  Game root(54);
  const TreeKey root_key = root_key_for(root, config);

  // Leave exactly one slot. The search consumes it for the root, then every
  // child insertion hits the hard capacity while root reservations are live.
  {
    auto guard = mcts.begin_parallel_search();
    auto &tree = mcts.prepare_parallel_tree(guard, TreeBackend::Coarse, 8);
    for (size_t index = 0; index < MAX_TREE_SIZE - 1; ++index) {
      TreeKey key{0xa500000000000000ULL + static_cast<uint64_t>(index),
                  MCTS_TREE_KEY_VERSION + 100, 0, TreeDomain::Exact, 0};
      require(key != root_key, "capacity fixture collided with the root key");
      (void)tree.find_or_create(key);
    }
    guard.finish();
  }

  ParallelMCTSSearcher searcher;
  auto options = search_options(4, TreeBackend::Coarse, 64);
  options.max_inflight = 16;
  options.evaluator_version = 20;
  const auto partial = searcher.run(mcts, root, options,
                                    ParallelInferenceFunction(evaluate_batch));
  require(partial.stop_reason == SearchStopReason::TreeCapacityReached &&
              partial.partial && partial.tree_size == MAX_TREE_SIZE &&
              partial.ledger.issued > 0 &&
              partial.ledger.issued < options.num_simulations &&
              partial.ledger.cancelled == partial.ledger.issued &&
              partial.ledger.completed() == 0 && partial.ledger.failed == 0 &&
              partial.ledger.virtual_loss_balanced() &&
              partial.ledger.reservations_aborted == partial.ledger.selected,
          "capacity boundary did not return a balanced partial result");
  const auto root_snapshot = mcts.get_parallel_node_snapshot(root_key);
  require(root_snapshot && root_snapshot->state == ExpansionState::Expanded &&
              root_snapshot->live_reservation_count == 0 &&
              !root_snapshot->has_pending_evaluation,
          "capacity cleanup retained root reservations or pending work");
  const double probability_sum = std::accumulate(
      partial.probabilities.begin(), partial.probabilities.end(), 0.0);
  bool illegal_probabilities_are_zero = true;
  for (size_t action = 0; action < MAX_ACTIONS; ++action) {
    if (!root_snapshot->valid_actions[action] &&
        partial.probabilities[action] != 0.0f)
      illegal_probabilities_are_zero = false;
  }
  require(std::abs(probability_sum - 1.0) < 1e-5 &&
              illegal_probabilities_are_zero,
          "zero-visit capacity result did not return a legal root prior");
  require_quiescent_snapshots(
      mcts, "capacity cleanup retained pending work or virtual loss");

  // A full tree with no root cannot produce a meaningful partial policy. It
  // must surface the capacity error instead of returning an all-zero result.
  Game missing_root(55);
  bool threw_capacity = false;
  try {
    (void)searcher.run(mcts, missing_root, options,
                       ParallelInferenceFunction(evaluate_batch));
  } catch (const TreeCapacityReachedError &) {
    threw_capacity = true;
  }
  require(threw_capacity && !mcts.is_parallel_search_active() &&
              mcts.parallel_tree_size() == MAX_TREE_SIZE,
          "capacity before root expansion returned an unusable partial result");

  // Capacity is first reported from a noexcept traversal worker through the
  // shared failure state. Clearing the deliberately full tree must leave the
  // same MCTS instance reusable with a balanced completion ledger.
  mcts.clear();
  options.num_simulations = 16;
  const auto recovered = searcher.run(
      mcts, missing_root, options, ParallelInferenceFunction(evaluate_batch));
  require(recovered.stop_reason == SearchStopReason::Completed &&
              !recovered.partial &&
              recovered.ledger.completed() == options.num_simulations &&
              recovered.ledger.failed == 0 &&
              recovered.ledger.virtual_loss_balanced() &&
              !mcts.is_parallel_search_active(),
          "MCTS was not reusable after a traversal-worker capacity failure");
}

void test_pending_terminal_duplicate_and_stale_races() {
  Policy policy{};
  policy[0] = 1.0f;
  ActionMask mask{};
  mask[0] = 1;
  std::array<float, FEATURE_SIZE> features{};
  Value value{};
  value[0] = 0.25;

  for (uint64_t iteration = 0; iteration < 32; ++iteration) {
    ConcurrentTree tree(1000 + iteration, TreeBackend::Sharded, 8, 4);
    const TreeKey key{0xb600000000000000ULL + iteration, MCTS_TREE_KEY_VERSION,
                      0, TreeDomain::Exact, 0};
    const auto node = tree.find_or_create(key);
    auto claim =
        tree.claim_or_attach(node, 7, iteration, features, mask, nullptr);
    require(claim.kind == ExpansionClaimKind::Owner && claim.pending,
            "race fixture could not claim a pending evaluation");

    std::atomic<bool> start{false};
    std::atomic<bool> published{false};
    std::atomic<bool> terminal_rejected{false};
    std::thread publisher([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      const auto ready =
          tree.publish_evaluation(node, claim.pending, policy, value, nullptr);
      published.store(ready == std::vector<uint64_t>{7},
                      std::memory_order_release);
    });
    std::thread terminal([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      try {
        tree.set_terminal(node, value);
      } catch (const std::logic_error &) {
        terminal_rejected.store(true, std::memory_order_release);
      }
    });
    start.store(true, std::memory_order_release);
    publisher.join();
    terminal.join();
    require(published.load(std::memory_order_acquire) &&
                terminal_rejected.load(std::memory_order_acquire) &&
                tree.snapshot(node).state == ExpansionState::Expanded,
            "terminal/publish race violated the expansion state machine");
    bool duplicate_rejected = false;
    try {
      (void)tree.publish_evaluation(node, claim.pending, policy, value,
                                    nullptr);
    } catch (const std::logic_error &) {
      duplicate_rejected = true;
    }
    require(duplicate_rejected, "duplicate pending publication was accepted");
    tree.validate_quiescent();
  }

  // Invalidation racing publication has two valid outcomes: publication wins
  // before clear, or the stale request is failed. Neither may leave a pending
  // object or mutate the new generation.
  for (uint64_t iteration = 0; iteration < 32; ++iteration) {
    const uint64_t generation = 2000 + iteration * 2;
    ConcurrentTree tree(generation, TreeBackend::Sharded, 8, 4);
    const TreeKey key{0xc700000000000000ULL + iteration, MCTS_TREE_KEY_VERSION,
                      0, TreeDomain::Exact, 0};
    const auto node = tree.find_or_create(key);
    auto claim =
        tree.claim_or_attach(node, 9, iteration, features, mask, nullptr);
    std::atomic<bool> start{false};
    std::exception_ptr publication_failure;
    std::thread publisher([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      try {
        (void)tree.publish_evaluation(node, claim.pending, policy, value,
                                      nullptr);
      } catch (...) {
        publication_failure = std::current_exception();
      }
    });
    std::thread invalidator([&] {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();
      tree.clear_and_set_generation(generation + 1);
    });
    start.store(true, std::memory_order_release);
    publisher.join();
    invalidator.join();
    if (publication_failure) {
      bool was_stale = false;
      try {
        std::rethrow_exception(publication_failure);
      } catch (const std::logic_error &) {
        was_stale = true;
      }
      require(was_stale, "stale publication threw an unexpected exception");
    }
    const auto old = tree.snapshot(node);
    require((old.state == ExpansionState::Expanded ||
             old.state == ExpansionState::Unexpanded) &&
                !old.has_pending_evaluation && tree.size() == 0 &&
                tree.generation() == generation + 1,
            "stale publication retained state in the new generation");
    tree.validate_quiescent();
  }
}

void test_max_depth_sized_path_cleanup() {
  constexpr uint64_t generation = 3000;
  ConcurrentTree tree(generation, TreeBackend::Sharded,
                      static_cast<size_t>(MAX_DEPTH) + 1, 8);
  auto ledger = std::make_shared<SearchLedger>();
  Policy policy{};
  policy[0] = 1.0f;
  ActionMask mask{};
  mask[0] = 1;
  SelectionContext context;
  context.tree_generation = generation;

  ReservedPath committed;
  std::vector<NodeHandle> nodes;
  nodes.reserve(MAX_DEPTH);
  for (int depth = 0; depth < MAX_DEPTH; ++depth) {
    const TreeKey key{0xd800000000000000ULL + static_cast<uint64_t>(depth),
                      MCTS_TREE_KEY_VERSION, 0, TreeDomain::Exact, 0};
    auto node = tree.find_or_create(key);
    tree.expand(node, policy, Value{}, mask);
    auto reservation = tree.select_and_reserve(node, mask, context, 0, ledger);
    require(reservation.has_value(),
            "MAX_DEPTH fixture could not reserve its forced edge");
    committed.append(std::move(*reservation));
    nodes.push_back(std::move(node));
  }
  committed.commit(Value{}, generation);

  ReservedPath cancelled;
  for (const auto &node : nodes) {
    auto reservation = tree.select_and_reserve(node, mask, context, 0, ledger);
    require(reservation.has_value(),
            "MAX_DEPTH abort fixture could not reserve its forced edge");
    cancelled.append(std::move(*reservation));
  }
  cancelled.abort();
  tree.validate_quiescent();
  const auto counters = ledger->snapshot();
  require(
      counters.selected == static_cast<uint64_t>(MAX_DEPTH) * 2 &&
          counters.reservations_committed == static_cast<uint64_t>(MAX_DEPTH) &&
          counters.reservations_aborted == static_cast<uint64_t>(MAX_DEPTH) &&
          counters.virtual_loss_balanced(),
      "MAX_DEPTH-sized commit/abort path leaked reservations");
}

void test_hidden_determinization_integration(uint32_t threads,
                                             TreeBackend backend) {
  constexpr uint64_t wave_size = 64;
  MCTSConfig config = search_config();
  config.use_determinization = true;
  config.num_determinizations = 1;
  MCTS mcts(config);
  const Game root = hidden_reserve_determinization_fixture();
  ParallelMCTSSearcher searcher;
  auto options = search_options(threads, backend, wave_size);
  options.mode = ParallelSearchMode::Throughput;
  options.batch_size = 16;
  options.batch_wait_us = 100;
  options.max_inflight = static_cast<uint32_t>(wave_size);
  options.master_seed = 0x82a4f03d17c95b61ULL;
  options.search_nonce = 0x1020304050607080ULL;
  options.simulation_id_base = 0;
  options.evaluator_version = 100;

  std::vector<DeterminizedLeaf> first_worlds;
  first_worlds.reserve(wave_size);
  for (uint64_t simulation_id = 0; simulation_id < wave_size; ++simulation_id) {
    auto leaf = determinized_leaf_for(root, options, simulation_id);
    const auto repeated = determinized_leaf_for(root, options, simulation_id);
    require(leaf.key == repeated.key && leaf.mask == repeated.mask &&
                leaf.features == repeated.features,
            "logical simulation seed did not reproduce its hidden world");
    if (!first_worlds.empty()) {
      require(leaf.key == first_worlds.front().key &&
                  leaf.features == first_worlds.front().features,
              "hidden identities did not merge into one public leaf");
    }
    first_worlds.push_back(std::move(leaf));
  }
  const TreeKey shared_leaf_key = first_worlds.front().key;
  const bool first_has_purchase =
      std::any_of(first_worlds.begin(), first_worlds.end(),
                  [](const DeterminizedLeaf &leaf) { return leaf.mask[42]; });
  const bool first_lacks_purchase =
      std::any_of(first_worlds.begin(), first_worlds.end(),
                  [](const DeterminizedLeaf &leaf) { return !leaf.mask[42]; });
  require(first_has_purchase && first_lacks_purchase,
          "seed manifest did not exercise both hidden purchase masks");

  bool saw_leaf_owner = false;
  uint64_t leaf_owner_id = std::numeric_limits<uint64_t>::max();
  ActionMask leaf_owner_mask{};
  bool returned_positive_outside_owner_mask = false;
  ParallelInferenceFunction inference =
      [&](const std::vector<ParallelInferenceRequest> &requests) {
        std::vector<ParallelInferenceResult> results;
        results.reserve(requests.size());
        bool first_leaf_publication = false;
        for (const auto &request : requests) {
          auto result = evaluate_unmasked_hidden_request(request);
          for (size_t action = 0; action < MAX_ACTIONS; ++action) {
            if (!request.owner_world_mask[action] &&
                result.policy[action] > 0.0f)
              returned_positive_outside_owner_mask = true;
          }
          if (request.key == shared_leaf_key && !saw_leaf_owner) {
            saw_leaf_owner = true;
            leaf_owner_id = request.owner_simulation_id;
            leaf_owner_mask = request.owner_world_mask;
            first_leaf_publication = true;
          }
          results.push_back(std::move(result));
        }
        // All wave-one tickets are already queued.  Holding only the fake
        // inference call (no tree lock is held here) lets every worker attach
        // to this one pending public leaf, including worlds with the opposite
        // local mask.
        if (first_leaf_publication)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return results;
      };

  const auto first = searcher.run(mcts, root, options, inference);
  require(first.stop_reason == SearchStopReason::Completed && !first.partial &&
              first.ledger.issued == wave_size &&
              first.ledger.completed() == wave_size &&
              first.ledger.evaluation_owner == 1 &&
              first.ledger.evaluation_waiter == wave_size - 1 &&
              first.ledger.evaluation_requested == 1 &&
              first.ledger.evaluated_boards == 1,
          "hidden worlds were not deduplicated into one owner and waiters");
  require(first.ledger.invalid_replay == 0 &&
              first.ledger.integrity_errors == 0 &&
              first.ledger.stale_result == 0 &&
              first.ledger.duplicate_result == 0 &&
              first.ledger.virtual_loss_balanced() &&
              first.ledger.reservations_committed == first.ledger.selected &&
              first.ledger.cancelled == 0 && first.ledger.failed == 0,
          "hidden wave-one ledger or virtual loss is inconsistent");
  require(first.resolved_seed == options.master_seed.value() &&
              first.search_nonce == options.search_nonce && saw_leaf_owner &&
              returned_positive_outside_owner_mask,
          "hidden search did not preserve seed or unmasked-policy contract");
  require(leaf_owner_id < wave_size &&
              first_worlds[static_cast<size_t>(leaf_owner_id)].mask ==
                  leaf_owner_mask,
          "pending owner ID does not match its determinized world mask");
  bool opposite_waiter = false;
  for (uint64_t simulation_id = 0; simulation_id < wave_size; ++simulation_id) {
    if (simulation_id != leaf_owner_id &&
        first_worlds[static_cast<size_t>(simulation_id)].mask !=
            leaf_owner_mask) {
      opposite_waiter = true;
      break;
    }
  }
  require(opposite_waiter,
          "same-key pending did not include a waiter with a different mask");

  const auto leaf_after_publish =
      mcts.get_parallel_node_snapshot(shared_leaf_key);
  require(leaf_after_publish &&
              leaf_after_publish->state == ExpansionState::Expanded &&
              !leaf_after_publish->has_pending_evaluation &&
              leaf_after_publish->live_reservation_count == 0 &&
              std::all_of(leaf_after_publish->stats.virtual_loss.begin(),
                          leaf_after_publish->stats.virtual_loss.end(),
                          [](uint64_t value) { return value == 0; }) &&
              leaf_after_publish->base_policy[42] == 1.0f,
          "published hidden leaf is not quiescent or lost raw policy");

  // A second seeded pair traverses the now-expanded shared node. Availability
  // is recorded from each ticket's own world, while the raw action-42 prior
  // remains usable in worlds where the hidden card is purchasable.
  uint64_t second_base = wave_size;
  while (second_base < wave_size + 4096) {
    const auto left = determinized_leaf_for(root, options, second_base);
    const auto right = determinized_leaf_for(root, options, second_base + 1);
    if (left.mask[42] != right.mask[42])
      break;
    ++second_base;
  }
  require(second_base < wave_size + 4096,
          "could not find adjacent deterministic worlds with distinct masks");
  options.simulation_id_base = second_base;
  options.num_simulations = 2;
  options.max_inflight = 2;
  uint64_t purchase_available = 0;
  for (uint64_t offset = 0; offset < options.num_simulations; ++offset) {
    const uint64_t simulation_id = options.simulation_id_base + offset;
    const auto leaf = determinized_leaf_for(root, options, simulation_id);
    require(leaf.key == shared_leaf_key &&
                leaf.features == first_worlds.front().features,
            "second seeded wave left the shared information set");
    purchase_available += leaf.mask[42] ? 1 : 0;
  }
  require(purchase_available == 1,
          "second seeded wave lacks world-local mask diversity");

  const auto second = searcher.run(mcts, root, options, inference);
  require(second.stop_reason == SearchStopReason::Completed &&
              !second.partial &&
              second.ledger.issued == options.num_simulations &&
              second.ledger.completed() == options.num_simulations &&
              second.ledger.cancelled == 0 && second.ledger.failed == 0 &&
              second.ledger.invalid_replay == 0 &&
              second.ledger.integrity_errors == 0 &&
              second.ledger.virtual_loss_balanced() &&
              second.ledger.reservations_committed == second.ledger.selected &&
              second.resolved_seed == options.master_seed.value() &&
              second.search_nonce == options.search_nonce,
          "expanded hidden-world traversal broke replay or ledger invariants");

  const auto traversed_leaf = mcts.get_parallel_node_snapshot(shared_leaf_key);
  require(traversed_leaf && !traversed_leaf->has_pending_evaluation &&
              traversed_leaf->live_reservation_count == 0 &&
              traversed_leaf->stats.total_visits == options.num_simulations &&
              traversed_leaf->availability_count[42] == purchase_available &&
              traversed_leaf->stats.N[42] > 0 &&
              traversed_leaf->stats.N[42] <= purchase_available &&
              std::all_of(traversed_leaf->stats.virtual_loss.begin(),
                          traversed_leaf->stats.virtual_loss.end(),
                          [](uint64_t value) { return value == 0; }),
          "shared leaf ignored world-local availability or is not quiescent");

  const TreeKey root_key = mcts_internal::GameAdapter::tree_key(root, 0, true);
  const auto root_snapshot = mcts.get_parallel_node_snapshot(root_key);
  require(root_snapshot && !root_snapshot->has_pending_evaluation &&
              root_snapshot->live_reservation_count == 0 &&
              std::all_of(root_snapshot->stats.virtual_loss.begin(),
                          root_snapshot->stats.virtual_loss.end(),
                          [](uint64_t value) { return value == 0; }) &&
              !mcts.is_parallel_search_active(),
          "hidden integration search did not leave a quiescent tree");
}

} // namespace

int main() {
  try {
    test_bounded_queue_close_and_wakeup();
    for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
      for (uint32_t threads : {1U, 2U, 4U, 8U})
        test_fake_evaluator_search(threads, backend);
    }
    test_active_mutation_rejected();
    test_callback_exception_cleanup_and_reuse();
    test_malformed_result_cleanup_and_reuse();
    test_timeout_returns_balanced_partial_result();
    test_sharded_layout_reuse_and_reset();
    test_entry_validation_is_transactional();
    test_evaluator_overflow_is_rethrown_and_reusable();
    test_capacity_named_callback_exception_is_rethrown_and_reusable();
    test_zero_resolved_budget_skips_evaluator();
    test_pre_cancel_is_side_effect_free();
    test_root_noise_seed_is_storage_domain_independent();
    test_inflight_cooperative_cancellation(ParallelSearchMode::Throughput);
    test_inflight_cooperative_cancellation(
        ParallelSearchMode::DeterministicEpoch);
    test_huge_budget_uses_bounded_ticket_storage(
        ParallelSearchMode::Throughput);
    test_huge_budget_uses_bounded_ticket_storage(
        ParallelSearchMode::DeterministicEpoch);
    test_capacity_partial_and_root_failure();
    test_pending_terminal_duplicate_and_stale_races();
    test_max_depth_sized_path_cleanup();
    for (TreeBackend backend : {TreeBackend::Coarse, TreeBackend::Sharded}) {
      for (uint32_t threads : {1U, 2U, 4U, 8U})
        test_hidden_determinization_integration(threads, backend);
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "mcts_parallel_scheduler: " << error.what() << '\n';
    return 1;
  }
}
