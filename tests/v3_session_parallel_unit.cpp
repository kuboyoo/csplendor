// V3 Session with num_threads > 1 must reproduce the sequential session
// bit for bit: every collected batch, every pending row and every tree.
#include "mcts_v3.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace {

using csplendor::v3search::Batch;
using csplendor::v3search::Config;
using csplendor::v3search::Session;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "check failed: %s\n", what);
    std::abort();
  }
}

uint64_t mix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
  return hash;
}

struct Trace {
  std::vector<std::vector<float>> features;
  std::vector<std::vector<int32_t>> legal_ids;
  std::vector<std::vector<int32_t>> offsets;
  std::vector<std::vector<int32_t>> slots;
  std::vector<std::vector<int32_t>> root_visits;
  uint64_t simulations = 0;
  uint64_t leaves = 0;
  uint64_t nodes = 0;
};

Trace run(const Config &base, int threads, const std::vector<Game> &roots) {
  Config config = base;
  config.num_threads = threads;
  Session session(config);
  for (size_t slot = 0; slot < roots.size(); ++slot)
    session.add_game(roots[slot], roots[slot].current_player(), 1000 + slot,
                     slot % 3 == 0, -1);
  Trace trace;
  std::vector<float> priors;
  std::vector<float> values;
  for (int round = 0; !session.all_done(); ++round) {
    if (round == 3) {
      // A reset between rounds must behave identically too.
      session.reset_game(1, roots[1], roots[1].current_player(), 77, false, -1);
    }
    Batch batch = session.collect();
    trace.features.push_back(batch.features);
    trace.legal_ids.push_back(batch.legal_ids);
    trace.offsets.push_back(batch.offsets);
    trace.slots.push_back(batch.slots);
    if (batch.rows() == 0)
      break;
    // Deterministic pseudo-network that depends only on the batch contents.
    priors.assign(batch.legal_ids.size(), 0.0f);
    for (size_t i = 0; i < priors.size(); ++i)
      priors[i] = static_cast<float>(
                      mix(static_cast<uint64_t>(batch.legal_ids[i]), i) % 997) /
                      997.0f +
                  1e-3f;
    values.assign(batch.rows(), 0.0f);
    for (size_t row = 0; row < batch.rows(); ++row) {
      uint32_t bits = 0;
      std::memcpy(&bits, &batch.features[row * batch.state_dim + 7], sizeof(bits));
      values[row] = static_cast<float>(mix(bits, row) % 1001) / 1000.0f - 0.5f;
    }
    session.apply(batch.legal_ids.data(), batch.offsets.data(), batch.rows(),
                  priors.data(), values.data());
  }
  for (size_t slot = 0; slot < roots.size(); ++slot) {
    const auto &game = session.game(static_cast<int>(slot));
    std::vector<int32_t> summary;
    for (const auto &entry : game.root_visits()) {
      summary.push_back(entry.first);
      summary.push_back(static_cast<int32_t>(entry.second));
    }
    for (const auto &entry : game.root_action_values()) {
      int32_t bits = 0;
      std::memcpy(&bits, &entry.second, sizeof(bits));
      summary.push_back(entry.first);
      summary.push_back(bits);
    }
    trace.root_visits.push_back(summary);
  }
  trace.simulations = session.stats().simulations;
  trace.leaves = session.stats().leaves;
  trace.nodes = session.stats().nodes;
  return trace;
}

void same(const Trace &a, const Trace &b) {
  check(a.features.size() == b.features.size(), "round count");
  for (size_t i = 0; i < a.features.size(); ++i) {
    check(a.features[i].size() == b.features[i].size(), "feature size");
    check(std::memcmp(a.features[i].data(), b.features[i].data(),
                      a.features[i].size() * sizeof(float)) == 0,
          "feature bits");
    check(a.legal_ids[i] == b.legal_ids[i], "legal ids");
    check(a.offsets[i] == b.offsets[i], "offsets");
    check(a.slots[i] == b.slots[i], "slots");
  }
  check(a.root_visits == b.root_visits, "root edges and visits");
  check(a.simulations == b.simulations, "simulations");
  check(a.leaves == b.leaves, "leaves");
  check(a.nodes == b.nodes, "nodes");
}

} // namespace

int main() {
  std::mt19937_64 rng(11);
  std::vector<Game> roots;
  for (int index = 0; roots.size() < 10; ++index) {
    Game game(index + 3);
    const int plies = static_cast<int>(rng() % 40);
    for (int ply = 0; ply < plies && !game.is_game_over(); ++ply)
      game.apply_random_action(rng());
    if (!game.is_game_over())
      roots.push_back(game);
  }
  Config config;
  config.num_simulations = 96;
  config.leaf_batch_size = 8;
  for (int variant = 0; variant < 3; ++variant) {
    Config current = config;
    current.semantic_groups = variant == 1;
    current.determinization = variant != 2;
    if (variant == 2) {
      current.rollout_samples = 2;
      current.rollout_candidates = 3;
      current.rollout_horizon = 6;
      current.chance_enumeration_depth = 1;
    }
    const Trace sequential = run(current, 1, roots);
    for (int threads : {2, 4, 16})
      same(sequential, run(current, threads, roots));
  }
  return 0;
}
