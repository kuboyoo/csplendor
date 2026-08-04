#ifndef CSPLENDOR_MCTS_ORCHESTRATION_H
#define CSPLENDOR_MCTS_ORCHESTRATION_H

#include "mcts_game_adapter.h"
#include "mcts_tree.h"

/**
 * Run batch simulations and prepare data for NN evaluation.
 *
 * World zero is copied from the already-selected search leaf. Additional
 * worlds start at the root and replay the selected path exactly once.
 */
inline BatchSimulationRequest MCTS::prepare_batch_simulations(
    const Game &root_game, uint8_t observer, int batch_size,
    int num_determinizations,
    const std::array<float, MAX_ACTIONS> *dirichlet_noise) {
  using mcts_internal::GameAdapter;

  reject_if_parallel_active("prepare legacy batch simulations");
  BatchSimulationRequest result;
  result.total_boards = 0;
  result.tree_generation = tree_generation_snapshot();
  if (batch_size <= 0 || num_determinizations <= 0)
    return result;
  if (GameAdapter::requires_forced_pass(root_game))
    throw std::invalid_argument(
        "MCTS root requires a forced pass; apply it before searching");
  if (config_.use_determinization && num_determinizations != 1) {
    throw std::invalid_argument(
        "hidden-information search requires one world per simulation");
  }

  uint64_t root_hash =
      GameAdapter::hash(root_game, observer, config_.use_determinization);

  for (int sim = 0; sim < batch_size; ++sim) {
    // Create shuffled game for this simulation. The conditional keeps the
    // existing RNG consumption: full-information search does not draw a seed.
    Game search_game = GameAdapter::clone_for_batch(
        root_game, config_.use_determinization, observer,
        config_.use_determinization ? rng_() : 0);

    std::vector<PathEntry> path;
    uint64_t current_hash = root_hash;
    int depth = 0;

    while (depth < MAX_DEPTH) {
      MCTSNode *node = get_node(current_hash);

      // Check terminal state
      if (GameAdapter::is_terminal(search_game)) {
        result.terminals.push_back(
            {path, GameAdapter::terminal_value(search_game, 0.0f)});
        break;
      }

      if (GameAdapter::requires_forced_pass(search_game)) {
        GameAdapter::resolve_forced_pass(search_game);
        current_hash = GameAdapter::hash(search_game, observer,
                                         config_.use_determinization);
        ++depth;
        continue;
      }

      // Leaf node - needs expansion
      if (node == nullptr || !node->is_expanded) {
        BatchLeafData leaf;
        leaf.hash = current_hash;
        leaf.path = path;
        leaf.num_worlds = num_determinizations;

        // Generate multiple worlds for evaluation. search_game has already
        // followed path, so world zero is ready to encode as-is. Additional
        // worlds start from the root and replay the path exactly once.
        for (int w = 0; w < num_determinizations; ++w) {
          Game world_game = (w == 0)
                                ? GameAdapter::copy_current(search_game)
                                : GameAdapter::clone_for_batch(
                                      root_game, config_.use_determinization,
                                      observer,
                                      config_.use_determinization ? rng_() : 0);

          // Replay path in root-based worlds using encoded action IDs.
          bool path_valid = true;
          if (w != 0) {
            for (const auto &entry : path) {
              GameAdapter::resolve_forced_pass(world_game);
              if (GameAdapter::is_terminal(world_game)) {
                path_valid = false;
                break;
              }
              if (!GameAdapter::decode_and_apply_native(world_game,
                                                        entry.action)) {
                path_valid = false;
                break;
              }
              if (GameAdapter::is_terminal(world_game)) {
                path_valid = false;
                break;
              }
            }
          }

          if (!path_valid) {
            abort_path_virtual_losses(path);
            throw std::runtime_error(
                "MCTS path replay is invalid in the current world");
          }

          // Encode board with observer perspective
          leaf.encoded_boards.push_back(
              GameAdapter::native_features(world_game, observer));

          // Get valid actions using C++ ActionEncoder (no GIL)
          leaf.valid_actions.push_back(
              GameAdapter::native_action_mask(world_game));
        }

        if (!leaf.encoded_boards.empty()) {
          leaf.num_worlds = static_cast<int>(leaf.encoded_boards.size());
          result.total_boards += leaf.num_worlds;
          result.leaves.push_back(std::move(leaf));
        }
        break;
      }

      // Terminal node (cached)
      if (node->is_terminal) {
        result.terminals.push_back({path, node->value});
        break;
      }

      // Select action with virtual loss
      bool is_root = (depth == 0);
      const auto world_mask = GameAdapter::native_action_mask_bits(search_game);
      int action = select_action_with_virtual_loss_for_world_bits(
          current_hash, world_mask, is_root, dirichlet_noise, sim);

      if (action < 0) {
        abort_path_virtual_losses(path);
        throw std::runtime_error(
            "non-terminal MCTS state has no world-local action");
      }

      // Add virtual loss
      add_virtual_loss(current_hash, action);

      // Record path (action is the encoded action ID)
      int player = GameAdapter::current_player(search_game);
      path.push_back({current_hash, action, player});

      // Apply action - decode the encoded action ID to get the actual Action
      if (!GameAdapter::decode_and_apply_native(search_game, action)) {
        abort_path_virtual_losses(path);
        throw std::runtime_error(
            "selected MCTS action is unavailable in the current world");
      }

      current_hash =
          GameAdapter::hash(search_game, observer, config_.use_determinization);
      depth++;
    }

    // The loop can also finish by reaching MAX_DEPTH. Return the path as a
    // draw terminal so apply_batch_results can both backpropagate it and
    // release every virtual loss accumulated along the path.
    if (depth >= MAX_DEPTH) {
      std::array<float, NUM_PLAYERS> value = {0};
      result.terminals.push_back({std::move(path), value});
    }
  }

  return result;
}

// Legacy overload for backward compatibility. It intentionally ignores the
// encoder argument and uses the native C++ encoder, as before.
inline BatchSimulationRequest MCTS::prepare_batch_simulations(
    const Game &root_game, uint8_t observer, int batch_size,
    int num_determinizations,
    const std::array<float, MAX_ACTIONS> *dirichlet_noise,
    IActionEncoder & /*encoder*/) {
  return prepare_batch_simulations(root_game, observer, batch_size,
                                   num_determinizations, dirichlet_noise);
}

#endif // CSPLENDOR_MCTS_ORCHESTRATION_H
