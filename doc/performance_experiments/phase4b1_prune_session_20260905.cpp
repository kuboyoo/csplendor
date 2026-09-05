// Supplemental native legacy session adapter. Timing/allocation/JSON/fixture/
// fake inference/digest helpers are exactly the existing benchmark's helpers.
// Compile this same source against baseline and candidate headers/libraries.
#define main csplendor_existing_benchmark_main
#include "../../scripts/benchmark_engine_hotpaths.cpp"
#undef main

int main(int argc, char **argv) {
  try {
    const Arguments args = parse_arguments(argc, argv);
    const Fixture fixture = make_fixture(args);
    MCTS tree(mcts_config(args), args.seed);
    Result result;
    LegacyFormalTrace trace;
    uint64_t evaluated = 0;
    uint64_t prunes = 0;
    uint64_t removed = 0;
    uint64_t max_nodes = 0;
    // Retained historical nodes ensure the size boundary is exercised in this
    // bounded guard, without representing it as a naturally collected tree.
    std::array<float, MAX_ACTIONS> policy{};
    std::array<float, NUM_PLAYERS> value{};
    std::array<uint8_t, MAX_ACTIONS> mask{};
    mask[0] = 1; policy[0] = 1;
    for (uint64_t key = 0; key < 49000; ++key)
      tree.expand_node(key, policy, value, mask);
    uint64_t hash = mcts_internal::GameAdapter::hash(
        fixture.game, static_cast<uint8_t>(args.observer), args.determinization);
    result.elapsed_ns = time_once([&] {
      for (uint64_t completed = 0; completed < args.iterations;) {
        const int batch = static_cast<int>(std::min<uint64_t>(args.batch_size, args.iterations - completed));
        const auto request = tree.prepare_batch_simulations(
            fixture.game, static_cast<uint8_t>(args.observer), batch, 1, nullptr);
        observe_legacy_request(request, trace);
        apply_fake_inference(tree, request, evaluated);
        completed += static_cast<uint64_t>(batch);
        const auto before = tree.tree_size();
        max_nodes = std::max<uint64_t>(max_nodes, before);
        tree.prune_if_needed();
        if (tree.tree_size() < before) {
          ++prunes;
          removed += before - tree.tree_size();
        }
      }
    }, result.perf);
    if (!prunes) throw std::runtime_error("prune guard did not prune");
    const auto root = tree.get_node_snapshot(hash);
    if (!root) throw std::runtime_error("prune lost current root");
    uint64_t digest = digest_u64(trace.request_replay_trace_digest, tree.tree_size());
    digest = digest_u64(digest, trace.inference_request_sequence_digest);
    digest = digest_u64(digest, root->total_visits);
    for (size_t action = 0; action < MAX_ACTIONS; ++action) {
      digest = digest_u64(digest, root->N[action]);
      digest = digest_float(digest, root->Q[action]);
      if (root->virtual_loss[action]) throw std::runtime_error("root VL leaked");
    }
    for (uint64_t key = 0; key < 49000; ++key)
      digest = digest_u64(digest, tree.get_node_snapshot(key).has_value());
    result.digest = digest;
    result.operations = args.iterations;
    result.counters.integer("evaluated_boards", evaluated);
    result.counters.integer("tree_size", tree.tree_size());
    result.semantics.integer("prunes", prunes);
    result.semantics.integer("removed_nodes", removed);
    result.semantics.integer("max_nodes", max_nodes);
    result.semantics.boolean("root_virtual_loss_balanced", true);
    result.semantics.string("history", "49000 synthetic expanded retained nodes; not natural game history");
    result.semantics.string("trace_scope", "all timed leaf paths/features and historical prune membership");
    emit_result("legacy_mcts", args, fixture, std::move(result));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
