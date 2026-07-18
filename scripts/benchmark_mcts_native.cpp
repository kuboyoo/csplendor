// Engine-only MCTS benchmark. This intentionally replaces NN inference with
// fixed C++ arrays, so it measures native orchestration rather than model
// latency. Compile the same source against baseline and candidate src/ trees.
#include "mcts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {

constexpr int kBatchSize = 16;
constexpr int kSearchSimulations = 256;
volatile std::uint64_t benchmark_sink = 0;

Game make_midgame() {
  Game game(42);
  for (int ply = 0; ply < 12 && !game.is_game_over(); ++ply) {
    const auto codes = game.legal_action_codes();
    if (codes.empty())
      break;
    const std::size_t index =
        (static_cast<std::size_t>(ply) * 7U + 3U) % codes.size();
    if (!game.apply_action_code_trusted(codes[index], false))
      std::abort();
  }
  return game;
}

MCTSConfig make_config(bool determinization) {
  MCTSConfig config;
  config.use_determinization = determinization;
  config.use_dirichlet_noise = false;
  config.forced_playouts = false;
  config.fpu = 0.0f;
  return config;
}

double elapsed_seconds(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                       started)
      .count();
}

double benchmark_leaf_preparation(const Game &root, bool determinization,
                                  int iterations) {
  MCTS mcts(make_config(determinization));
  for (int i = 0; i < 20; ++i) {
    const auto request = mcts.prepare_batch_simulations(
        root, root.current_player(), kBatchSize, 1, nullptr);
    benchmark_sink += static_cast<std::uint64_t>(request.total_boards);
  }

  std::uint64_t simulations = 0;
  const auto started = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) {
    const auto request = mcts.prepare_batch_simulations(
        root, root.current_player(), kBatchSize, 1, nullptr);
    simulations += kBatchSize;
    benchmark_sink += static_cast<std::uint64_t>(request.total_boards);
  }
  return static_cast<double>(simulations) / elapsed_seconds(started);
}

void apply_fake_inference(MCTS &mcts,
                          const BatchSimulationRequest &request) {
  std::array<float, MAX_ACTIONS> policy{};
  for (std::size_t action = 0; action < policy.size(); ++action)
    policy[action] = static_cast<float>(action + 1);
  const std::array<float, NUM_PLAYERS> value = {0.1f, -0.1f};
  const auto board_count = static_cast<std::size_t>(request.total_boards);
  mcts.apply_batch_results(
      request,
      std::vector<std::array<float, MAX_ACTIONS>>(board_count, policy),
      std::vector<std::array<float, NUM_PLAYERS>>(board_count, value));
}

double benchmark_synthetic_search(const Game &root, bool determinization,
                                  int searches) {
  std::uint64_t simulations = 0;
  const auto started = std::chrono::steady_clock::now();
  for (int search = 0; search < searches; ++search) {
    MCTS mcts(make_config(determinization));
    for (int completed = 0; completed < kSearchSimulations;
         completed += kBatchSize) {
      const auto request = mcts.prepare_batch_simulations(
          root, root.current_player(), kBatchSize, 1, nullptr);
      apply_fake_inference(mcts, request);
      simulations += kBatchSize;
    }
    benchmark_sink += static_cast<std::uint64_t>(mcts.tree_size());
  }
  return static_cast<double>(simulations) / elapsed_seconds(started);
}

double median(std::vector<double> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

void report(const Game &root, const std::string &metric,
            bool determinization, int samples, int iterations) {
  std::vector<double> rates;
  rates.reserve(static_cast<std::size_t>(samples));
  for (int sample = 0; sample < samples; ++sample) {
    if (metric == "leaf_prepare") {
      rates.push_back(
          benchmark_leaf_preparation(root, determinization, iterations));
    } else {
      rates.push_back(
          benchmark_synthetic_search(root, determinization, iterations));
    }
  }
  const double middle = median(rates);
  const double mean =
      std::accumulate(rates.begin(), rates.end(), 0.0) / rates.size();
  std::cout << metric << ',' << (determinization ? "on" : "off") << ','
            << std::fixed << std::setprecision(2) << middle << ',' << mean
            << '\n';
  if (std::getenv("CSPLENDOR_BENCH_RAW") != nullptr) {
    for (std::size_t sample = 0; sample < rates.size(); ++sample) {
      std::cerr << "raw," << metric << ','
                << (determinization ? "on" : "off") << ',' << sample << ','
                << std::fixed << std::setprecision(2) << rates[sample] << '\n';
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  const int samples = argc > 1 ? std::max(1, std::atoi(argv[1])) : 15;
  const int leaf_iterations =
      argc > 2 ? std::max(1, std::atoi(argv[2])) : 1000;
  const int searches = argc > 3 ? std::max(1, std::atoi(argv[3])) : 10;
  const Game root = make_midgame();

  std::cout << "metric,determinization,median_simulations_per_second,"
               "mean_simulations_per_second\n";
  for (bool determinization : {false, true})
    report(root, "leaf_prepare", determinization, samples, leaf_iterations);
  for (bool determinization : {false, true})
    report(root, "synthetic_search", determinization, samples, searches);

  if (benchmark_sink == 0)
    return 2;
  return 0;
}
