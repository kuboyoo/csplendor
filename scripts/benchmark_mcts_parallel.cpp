#include "mcts.h"
#include "mcts_parallel_searcher.h"
#include "mcts_root_parallel.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Arguments {
  std::vector<uint32_t> threads{1, 2, 4, 8};
  uint64_t simulations = 4096;
  uint32_t samples = 7;
  uint32_t batch_size = 16;
  uint32_t latency_us = 0;
  uint64_t seed = 42;
  bool determinization = false;
  std::string backend = "all";
};

std::vector<uint32_t> parse_threads(const std::string &text) {
  std::vector<uint32_t> result;
  std::stringstream stream(text);
  std::string item;
  while (std::getline(stream, item, ',')) {
    const unsigned long value = std::stoul(item);
    if (value == 0 || value > 256)
      throw std::invalid_argument("thread count is out of range");
    result.push_back(static_cast<uint32_t>(value));
  }
  if (result.empty())
    throw std::invalid_argument("thread list is empty");
  return result;
}

Arguments parse_arguments(int argc, char **argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    const auto split = option.find('=');
    const std::string key = option.substr(0, split);
    const std::string value =
        split == std::string::npos ? std::string{} : option.substr(split + 1);
    if (key == "--threads")
      arguments.threads = parse_threads(value);
    else if (key == "--simulations")
      arguments.simulations = std::stoull(value);
    else if (key == "--samples")
      arguments.samples = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--batch")
      arguments.batch_size = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--latency-us")
      arguments.latency_us = static_cast<uint32_t>(std::stoul(value));
    else if (key == "--seed")
      arguments.seed = std::stoull(value);
    else if (key == "--backend")
      arguments.backend = value;
    else if (key == "--determinization")
      arguments.determinization = value == "1" || value == "true";
    else
      throw std::invalid_argument("unknown benchmark option: " + key);
  }
  if (arguments.simulations == 0 || arguments.samples == 0 ||
      arguments.batch_size == 0)
    throw std::invalid_argument(
        "simulations, samples and batch must be positive");
  if (arguments.backend != "all" && arguments.backend != "legacy" &&
      arguments.backend != "coarse" && arguments.backend != "sharded" &&
      arguments.backend != "root")
    throw std::invalid_argument(
        "backend must be all/legacy/coarse/sharded/root");
  return arguments;
}

mcts_parallel::ParallelInferenceFunction evaluator(uint32_t latency_us) {
  return [latency_us](const std::vector<mcts_parallel::ParallelInferenceRequest>
                          &requests) {
    if (latency_us > 0)
      std::this_thread::sleep_for(std::chrono::microseconds(latency_us));
    std::vector<mcts_parallel::ParallelInferenceResult> results(
        requests.size());
    for (size_t index = 0; index < requests.size(); ++index) {
      // Keep a raw unmasked policy. Selection normalizes it over each world's
      // current legality mask.
      for (size_t action = 0; action < MAX_ACTIONS; ++action)
        results[index].policy[action] = static_cast<float>(action + 1);
      results[index].value = {0.1f, -0.1f};
    }
    return results;
  };
}

double percentile(std::vector<double> values, double fraction) {
  std::sort(values.begin(), values.end());
  const size_t index =
      static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

struct Measurement {
  double simulations_per_second = 0.0;
  uint64_t tree_size = 0;
  uint64_t inference_requests = 0;
  uint64_t waiters = 0;
};

Game make_midgame(uint64_t seed) {
  Game game(seed);
  for (int ply = 0; ply < 12 && !game.is_game_over(); ++ply) {
    const auto codes = game.legal_action_codes();
    if (codes.empty())
      break;
    const size_t index = (static_cast<size_t>(ply) * 7U + 3U) % codes.size();
    if (!game.apply_action_code_trusted(codes[index], false))
      throw std::runtime_error("failed to construct benchmark midgame");
  }
  return game;
}

Measurement run_shared(const Arguments &arguments, uint32_t threads,
                       mcts_parallel::TreeBackend backend) {
  MCTSConfig config;
  config.use_determinization = arguments.determinization;
  config.num_determinizations = 1;
  config.use_dirichlet_noise = false;
  MCTS mcts(config);
  Game root = make_midgame(arguments.seed);
  mcts_parallel::ParallelSearchOptions options;
  options.num_threads = threads;
  options.batch_size = arguments.batch_size;
  options.batch_wait_us = 200;
  options.max_inflight =
      std::max<uint32_t>(threads * 4, arguments.batch_size * 2);
  options.num_simulations = arguments.simulations;
  options.master_seed = arguments.seed;
  options.search_nonce = 0;
  options.evaluator_version = 1;
  options.tree_backend = backend;
  options.shard_count = 64;
  mcts_parallel::ParallelMCTSSearcher searcher;
  const auto started = std::chrono::steady_clock::now();
  const auto result =
      searcher.run(mcts, root, options, evaluator(arguments.latency_us));
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  if (result.ledger.completed() != arguments.simulations ||
      !result.ledger.virtual_loss_balanced())
    throw std::runtime_error("shared benchmark search failed invariants");
  return {static_cast<double>(arguments.simulations) / seconds,
          result.tree_size, result.ledger.evaluation_requested,
          result.ledger.evaluation_waiter};
}

Measurement run_legacy(const Arguments &arguments) {
  MCTSConfig config;
  config.use_determinization = arguments.determinization;
  config.num_determinizations = 1;
  config.use_dirichlet_noise = false;
  MCTS mcts(config);
  Game root = make_midgame(arguments.seed);
  std::array<float, MAX_ACTIONS> policy{};
  for (size_t action = 0; action < MAX_ACTIONS; ++action)
    policy[action] = static_cast<float>(action + 1);
  const std::array<float, NUM_PLAYERS> value = {0.1f, -0.1f};
  const auto started = std::chrono::steady_clock::now();
  uint64_t completed = 0;
  uint64_t requests = 0;
  while (completed < arguments.simulations) {
    const int batch = static_cast<int>(std::min<uint64_t>(
        arguments.batch_size, arguments.simulations - completed));
    auto request = mcts.prepare_batch_simulations(
        root, static_cast<uint8_t>(root.current_player()), batch, 1, nullptr);
    const size_t boards = static_cast<size_t>(request.total_boards);
    if (arguments.latency_us > 0)
      std::this_thread::sleep_for(
          std::chrono::microseconds(arguments.latency_us));
    mcts.apply_batch_results(
        request, std::vector<std::array<float, MAX_ACTIONS>>(boards, policy),
        std::vector<std::array<float, NUM_PLAYERS>>(boards, value));
    requests += static_cast<uint64_t>(request.leaves.size());
    completed += static_cast<uint64_t>(batch);
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  return {static_cast<double>(arguments.simulations) / seconds,
          static_cast<uint64_t>(mcts.tree_size()), requests, 0};
}

Measurement run_root(const Arguments &arguments, uint32_t threads) {
  MCTSConfig config;
  config.use_determinization = arguments.determinization;
  config.num_determinizations = 1;
  config.use_dirichlet_noise = false;
  Game root = make_midgame(arguments.seed);
  mcts_parallel::ParallelSearchOptions options;
  options.batch_size = arguments.batch_size;
  options.deterministic_epoch_size = arguments.batch_size;
  options.master_seed = arguments.seed;
  options.search_nonce = 0;
  options.evaluator_version = 1;
  const auto factory = [&](uint32_t) {
    return evaluator(arguments.latency_us);
  };
  const auto started = std::chrono::steady_clock::now();
  const auto result = mcts_parallel::run_root_parallel(
      config, root, arguments.simulations, threads, options, factory);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  if (result.merged.ledger.completed() != arguments.simulations ||
      !result.merged.ledger.virtual_loss_balanced())
    throw std::runtime_error("root-parallel benchmark failed invariants");
  return {static_cast<double>(arguments.simulations) / seconds,
          result.merged.tree_size, result.merged.ledger.evaluation_requested,
          result.merged.ledger.evaluation_waiter};
}

template <typename Runner>
void measure(const Arguments &arguments, const std::string &backend,
             uint32_t threads, Runner &&runner) {
  std::vector<double> rates;
  rates.reserve(arguments.samples);
  uint64_t tree_size = 0;
  uint64_t inference_requests = 0;
  uint64_t waiters = 0;
  for (uint32_t sample = 0; sample < arguments.samples; ++sample) {
    const Measurement value = runner();
    rates.push_back(value.simulations_per_second);
    tree_size = value.tree_size;
    inference_requests = value.inference_requests;
    waiters = value.waiters;
  }
  const double mean =
      std::accumulate(rates.begin(), rates.end(), 0.0) / rates.size();
  std::cout << backend << ',' << threads << ',' << arguments.simulations << ','
            << arguments.batch_size << ',' << arguments.latency_us << ','
            << (arguments.determinization ? 1 : 0) << ',' << std::fixed
            << std::setprecision(2) << percentile(rates, 0.5) << ',' << mean
            << ',' << percentile(rates, 0.95) << ',' << tree_size << ','
            << inference_requests << ',' << waiters << '\n';
}

} // namespace

int main(int argc, char **argv) {
  try {
    const Arguments arguments = parse_arguments(argc, argv);
    std::cout << "backend,threads,simulations,batch,latency_us,determinization,"
                 "median_sims_per_s,mean_sims_per_s,p95_sims_per_s,tree_size,"
                 "inference_requests,waiters\n";
    for (uint32_t threads : arguments.threads) {
      if ((arguments.backend == "all" || arguments.backend == "legacy") &&
          threads == arguments.threads.front())
        measure(arguments, "legacy", 1, [&] { return run_legacy(arguments); });
      if (arguments.backend == "all" || arguments.backend == "coarse")
        measure(arguments, "coarse", threads, [&] {
          return run_shared(arguments, threads,
                            mcts_parallel::TreeBackend::Coarse);
        });
      if (arguments.backend == "all" || arguments.backend == "sharded")
        measure(arguments, "sharded", threads, [&] {
          return run_shared(arguments, threads,
                            mcts_parallel::TreeBackend::Sharded);
        });
      if (arguments.backend == "all" || arguments.backend == "root")
        measure(arguments, "root", threads,
                [&] { return run_root(arguments, threads); });
    }
  } catch (const std::exception &error) {
    std::cerr << "benchmark_mcts_parallel: " << error.what() << '\n';
    return 1;
  }
  return 0;
}
