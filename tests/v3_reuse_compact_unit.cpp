// Tree reuse (GameSearch::try_advance) keeps only the subtree of the new root:
// node_count() equals the nodes reachable from it, and that subtree is the same
// as the one below the played actions in the old tree (indices aside).
#include "mcts_v3.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using csplendor::v3search::Batch;
using csplendor::v3search::Config;
using csplendor::v3search::GameSearch;
using csplendor::v3search::Node;
using csplendor::v3search::NodeKind;
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

void drive(Session &session) {
  std::vector<float> priors;
  std::vector<float> values;
  while (!session.all_done()) {
    Batch batch = session.collect();
    if (batch.rows() == 0)
      break;
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
    session.apply(batch.legal_ids.data(), batch.legal_ids.size(),
                  batch.offsets.data(), batch.rows(), priors.data(), values.data());
  }
}

template <typename T> void put(std::vector<uint8_t> &out, T value) {
  const auto *bytes = reinterpret_cast<const uint8_t *>(&value);
  out.insert(out.end(), bytes, bytes + sizeof(T));
}

// Preorder serialisation of the subtree below `index`, without node indices.
size_t serialise(const GameSearch &search, int32_t index, std::vector<uint8_t> &out) {
  const Node &node = search.node(index);
  put(out, static_cast<uint8_t>(node.kind));
  put(out, node.player);
  put(out, node.expanded);
  put(out, node.pending);
  put(out, node.enumerated);
  put(out, node.visits);
  put(out, node.virtual_visits);
  put(out, node.value_sum);
  put(out, node.terminal_value);
  put(out, node.chance_mean);
  put(out, node.edges.size());
  size_t count = 1;
  for (const auto &edge : node.edges) {
    put(out, edge.key);
    put(out, edge.prior);
    put(out, edge.clean_prior);
    put(out, edge.chance_value);
    put(out, edge.child >= 0);
    if (edge.child >= 0)
      count += serialise(search, edge.child, out);
  }
  return count;
}

int32_t root_index(const GameSearch &search) {
  return static_cast<int32_t>(&search.root() - &search.node(0));
}

// The most visited root action whose child is an expanded decision node
// (no reveal in between), so that the tree can be re-rooted on it.
int32_t reusable_action(const GameSearch &search, int32_t &child) {
  int32_t best = -1;
  uint32_t best_visits = 0;
  for (const auto &edge : search.root().edges) {
    if (edge.child < 0)
      continue;
    const Node &node = search.node(edge.child);
    if (node.kind == NodeKind::Decision && node.expanded && node.visits > best_visits) {
      best = edge.key;
      best_visits = node.visits;
      child = edge.child;
    }
  }
  return best;
}

void play(Game &game, int32_t action_id) {
  bool found = false;
  Action chosen{};
  auto sink = [&](int id, const Action &action) {
    if (id == action_id) {
      chosen = action;
      found = true;
    }
  };
  ActionEncoderV5::for_each_legal_with_id(game, sink);
  check(found, "played action is legal");
  check(game.apply(chosen), "apply played action");
}

void check_reuse_releases_unreachable_nodes(const Config &config, uint64_t game_seed) {
  Session session(config);
  Game game(game_seed);
  const int observer = game.current_player();
  session.add_game(game, observer, 3, false, -1);
  drive(session);
  for (int move = 0; move < 4; ++move) {
    const GameSearch &search = session.game(0);
    int32_t child = -1;
    const int32_t action = reusable_action(search, child);
    if (action < 0)
      return; // nothing to re-root on (e.g. only reveals); other moves cover it
    std::vector<uint8_t> before;
    const size_t reachable = serialise(search, child, before);
    const size_t old_count = search.node_count();
    play(game, action);
    if (game.is_game_over())
      return;
    check(session.advance_game(0, game, {action}, observer, 5 + move, false, -1),
          "advance on an expanded decision child");
    const GameSearch &advanced = session.game(0);
    check(root_index(advanced) == 0, "new root is node 0");
    check(advanced.node_count() == reachable, "node_count is the reachable subtree");
    check(advanced.node_count() < old_count, "unreachable nodes released");
    std::vector<uint8_t> after;
    check(serialise(advanced, 0, after) == reachable, "subtree size");
    check(before == after, "subtree statistics unchanged");
    check(advanced.reused_visits() == static_cast<int>(advanced.root().visits),
          "reused visits");
    drive(session);
  }
}

} // namespace

int main() {
  Config config;
  config.num_simulations = 160;
  config.leaf_batch_size = 8;
  for (uint64_t seed : {3, 11, 81}) {
    check_reuse_releases_unreachable_nodes(config, seed);
    Config enumerated = config;
    enumerated.chance_enumeration_depth = 1;
    enumerated.chance_control_variate = true;
    check_reuse_releases_unreachable_nodes(enumerated, seed);
  }
  return 0;
}
