// Diagnostic adapter around the engine's real split_root API and existing
// fixture/digest/counter infrastructure. This is NOT a standalone correctness
// probe or an end-to-end solver performance claim.
#define main original_engine_benchmark_main
#include CSPLENDOR_EXISTING_BENCHMARK
#undef main

int main(int argc, char **argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    const auto fixture = make_fixture(arguments);
    uint64_t purchase = UINT64_MAX;
    for (const auto &action : fixture.game.legal_actions()) {
      if (action.type == PURCHASE && !action.from_reserved) {
        const int level = get_card(action.card_id).level - 1;
        if (!fixture.game.board.decks[level].empty()) {
          purchase = action.pack();
          break;
        }
      }
    }
    if (purchase == UINT64_MAX)
      throw std::runtime_error("N/A: fixture has no legal visible purchase with a nonempty deck");
    Result result;
    result.digest = kDigestOffset;
    RevealVerifiedFrontierResult frontier;
    RevealVerifiedSolver solver(fixture.game.current_player(), arguments.depth, 0, 0.0,
        {}, false, 100000, 500000, purchase, false, 0, false, true);
    result.elapsed_ns = time_once([&] {
      frontier = solver.split_root(fixture.game, 100000);
    }, result.perf);
    if (!frontier.complete)
      throw std::runtime_error("split_root incomplete: " + frontier.unknown_reason);
    const auto order = observe_reveal_root_order(frontier);
    result.operations = frontier.edges.size();
    result.digest = digest_u64(result.digest, order.ordered_outcome_sequence_digest);
    // Child snapshots/hashes are also inspected by the strong rollback build.
    for (const auto &edge : frontier.edges)
      result.digest = digest_u64(result.digest, edge.child.board.hash());
    result.semantics.string("scope", "one forced visible purchase; full root materialization, not solve");
    result.semantics.string("required_root_action", std::to_string(purchase));
    result.semantics.integer("edges", frontier.edges.size());
    result.semantics.boolean("complete", frontier.complete);
    emit_result("purchase_split_cost", arguments, fixture, std::move(result));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
