#include "mcts_tree.h"
#include <iostream>
#include <stdexcept>

struct MCTSLegacyRecordTestAccess {
  static uint64_t counter(const MCTS &tree) { return tree.access_counter_; }
  static void counter(MCTS &tree, uint64_t value) { tree.access_counter_ = value; }
  static void all_times(MCTS &tree, uint64_t value) {
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    for (auto &entry : tree.nodes_) entry.second.last_access = value;
#else
    for (auto &entry : tree.access_count_) entry.second = value;
#endif
  }
};

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
void insert(MCTS &tree, uint64_t begin, uint64_t end) {
  for (uint64_t key = begin; key < end; ++key)
    tree.get_or_create_node(key);
}
void references_and_touch() {
  MCTS tree(MCTSConfig{}, 42);
  auto &retained = tree.get_or_create_node(0);
  MCTSNode *pointer = tree.get_node(0);
  const uint64_t before = MCTSLegacyRecordTestAccess::counter(tree);
  require(tree.get_node_snapshot(0).has_value(), "snapshot missing");
  require(!tree.get_node_snapshot(999).has_value(), "snapshot fabricated");
  require(!tree.get_node(999), "missing mutable node fabricated");
  require(MCTSLegacyRecordTestAccess::counter(tree) == before, "read touched LRU");
  insert(tree, 1, 50000);
  require(pointer == &retained && pointer == tree.get_node(0), "rehash invalidated pointer");
  retained.valid_actions[7] = 1;
  retained.prior[7] = 1.0f;
  retained.is_expanded = true;
  require(tree.select_action_with_virtual_loss(0, false) == 7, "mutable alias ignored");
  tree.add_virtual_loss(0, 7);
  tree.update_stats(0, 7, 0.5f);
  require(pointer->N[7] == 1 && pointer->Q[7] == 0.5f && pointer->virtual_loss[7] == 1,
          "record is not the public mutable node");
  tree.remove_virtual_loss(0, 7);
  tree.prune_if_needed();
  require(tree.tree_size() == 40001, "strict prune threshold changed");
  for (uint64_t key = 0; key < 50000; ++key)
    require(tree.get_node_snapshot(key).has_value() == (key == 0 || key >= 10000),
            "prune membership changed");
  require(pointer == tree.get_node(0) && pointer->N[7] == 1, "prune moved survivor");
}
void time_boundaries() {
  MCTS tree(MCTSConfig{}, 42);
  insert(tree, 0, 49999);
  MCTSLegacyRecordTestAccess::counter(tree, 1000001);
  tree.prune_if_needed();
  require(MCTSLegacyRecordTestAccess::counter(tree) == 1000001,
          "counter reset below size threshold");
  tree.get_or_create_node(49999);
  tree.prune_if_needed();
  require(tree.tree_size() == 40001 && MCTSLegacyRecordTestAccess::counter(tree) == 0,
          "counter reset boundary changed");
  insert(tree, 50000, 59999);
  MCTSLegacyRecordTestAccess::all_times(tree, 0);
  MCTSLegacyRecordTestAccess::counter(tree, 1000000);
  tree.prune_if_needed();
  require(tree.tree_size() == 50000 && MCTSLegacyRecordTestAccess::counter(tree) == 1000000,
          "equal times pruned or exact million reset");
  MCTSLegacyRecordTestAccess::counter(tree, std::numeric_limits<uint64_t>::max());
  require(tree.get_node(49999) != nullptr, "wrap test node missing");
  require(MCTSLegacyRecordTestAccess::counter(tree) == 0, "unsigned wrap changed");
  tree.prune_if_needed();
  require(tree.tree_size() == 50000, "wrap changed equal-time membership");
  tree.clear();
  require(tree.tree_size() == 0 && MCTSLegacyRecordTestAccess::counter(tree) == 0,
          "clear left records/timestamps");
}
void outstanding_batch_and_aux() {
  MCTS tree(MCTSConfig{}, 42);
  std::array<float, MAX_ACTIONS> policy{};
  std::array<uint8_t, MAX_ACTIONS> mask{};
  std::array<float, NUM_PLAYERS> value{};
  policy[3] = 1.0f; mask[3] = 1; value[0] = 0.25f;
  tree.expand_node(0, policy, value, mask);
  MCTSNode *retained = tree.get_node(0);
  // Overwriting the public node does NOT reset its existing auxiliary policy.
  *retained = MCTSNode{};
  // Two legal choices distinguish retained aux policy from the overwritten
  // public prior: a lost auxiliary record would now select action 7.
  retained->prior[7] = 10.0f;
  require(tree.select_action_with_virtual_loss_for_world_bits(
              0, (uint64_t{1} << 3) | (uint64_t{1} << 7), false) == 3,
          "auxiliary policy lost after public mutation");
  BatchSimulationRequest request;
  request.tree_generation = tree.tree_generation_snapshot();
  request.total_boards = 1;
  BatchLeafData leaf;
  leaf.hash = 60000;
  leaf.num_worlds = 1;
  leaf.path.push_back({0, 3, 0});
  leaf.valid_actions.push_back(mask);
  request.leaves.push_back(leaf);
  tree.add_virtual_loss(0, 3);
  insert(tree, 1, 50000);
  tree.prune_if_needed(); // It is legal for an outstanding path to be pruned.
  require(!tree.get_node_snapshot(0), "old outstanding path should be pruned");
  tree.apply_batch_results(request, {policy}, {value});
  require(tree.get_node_snapshot(60000)->is_expanded, "pending result not expanded");
  require(!tree.get_node_snapshot(0), "pending result recreated erased path");
  tree.clear_virtual_losses();
  auto config = tree.get_config_snapshot();
  config.use_determinization = !config.use_determinization;
  tree.set_config(config);
  require(tree.tree_size() == 0 && MCTSLegacyRecordTestAccess::counter(tree) == 0,
          "domain change retained old records");
  bool rejected = false;
  try { tree.apply_batch_results(request, {policy}, {value}); }
  catch (const std::logic_error &) { rejected = true; }
  require(rejected, "stale pending request accepted");
}
}
int main() {
  try {
    references_and_touch();
    time_boundaries();
    outstanding_batch_and_aux();
    std::cout << "legacy record pointer / touch / prune / counter / pending batch PASS\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
