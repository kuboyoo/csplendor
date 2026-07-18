#ifndef CSPLENDOR_MCTS_SEARCHER_H
#define CSPLENDOR_MCTS_SEARCHER_H

#include "game.h"
#include "mcts.h"
#include "mcts_game_adapter.h"
#include <functional>
#include <vector>

// Feature extractor interface (to be implemented via Python callback or C++)
class IFeaturizer {
public:
  virtual ~IFeaturizer() = default;
  virtual std::array<float, 196> featurize(const Game &game) = 0;
};

// IActionEncoder is defined in mcts.h

// Batch inference result
struct InferenceResult {
  std::array<float, MAX_ACTIONS> policy{};
  std::array<float, NUM_PLAYERS> value{};
};

// MCTS Searcher - orchestrates the search process
class MCTSSearcher {
public:
  MCTSSearcher(MCTS &mcts, IFeaturizer &featurizer, IActionEncoder &encoder)
      : mcts_(mcts), featurizer_(featurizer), encoder_(encoder) {}

  // Run a single simulation from root, collecting leaf nodes for batch inference
  // Returns true if a leaf was found, false if terminal
  bool run_simulation(const Game &root_game, int observer,
                      std::vector<LeafRequest> &leaf_requests,
                      bool is_first_sim, int current_sim = 0) {
    if (mcts_internal::GameAdapter::requires_forced_pass(root_game))
      throw std::invalid_argument(
          "MCTS root requires a forced pass; apply it before searching");
    // Non-determinized Python callbacks observe an isolated copy of the
    // root's history. Keep that contract while transferring ownership to the
    // simulation loop so search() does not need another copy.
    return run_simulation_owned(
        mcts_internal::GameAdapter::clone_with_history(root_game), observer,
        leaf_requests, is_first_sim, current_sim);
  }

private:
  bool run_simulation_owned(Game game, int observer,
                            std::vector<LeafRequest> &leaf_requests,
                            bool is_first_sim, int current_sim) {
    path_.clear();
    uint64_t current_hash = mcts_internal::GameAdapter::hash(
        game, static_cast<uint8_t>(observer),
        mcts_.config().use_determinization);
    int depth = 0;

    while (depth < MAX_DEPTH) {
      MCTSNode *node = mcts_.get_node(current_hash);

      // Check for terminal state
      if (mcts_internal::GameAdapter::is_terminal(game)) {
        auto terminal_value =
            mcts_internal::GameAdapter::terminal_value(game, 0.01f);
        mcts_.set_terminal(current_hash, terminal_value);
        backpropagate(terminal_value);
        return false;
      }

      if (mcts_internal::GameAdapter::requires_forced_pass(game)) {
        mcts_internal::GameAdapter::resolve_forced_pass(game);
        current_hash = mcts_internal::GameAdapter::hash(
            game, static_cast<uint8_t>(observer),
            mcts_.config().use_determinization);
        ++depth;
        continue;
      }

      // Leaf node - needs expansion
      if (!node || !node->is_expanded) {
        LeafRequest req;
        req.hash = current_hash;
        req.features =
            mcts_internal::GameAdapter::featurize(featurizer_, game);
        // This is the last use of the leaf Game. A Python encoder can take
        // ownership of it while preserving callback isolation and history.
        req.valid_actions = mcts_internal::GameAdapter::action_mask_owned(
            encoder_, std::move(game));
        req.path_index = static_cast<int>(leaf_requests.size());

        // Store path for later backpropagation
        pending_paths_.push_back(path_);
        leaf_requests.push_back(req);
        return true;
      }

      // Terminal node (cached)
      if (node->is_terminal) {
        backpropagate(node->value);
        return false;
      }

      // Select action using PUCT
      int action = mcts_.select_action(*node, is_first_sim && depth == 0,
                                       current_sim);
      if (action < 0) {
        // No valid action - treat as terminal
        auto value =
            mcts_internal::GameAdapter::terminal_value(game, 0.01f);
        backpropagate(value);
        return false;
      }

      // Record path for backpropagation
      PathEntry entry;
      entry.hash = current_hash;
      entry.action = action;
      entry.player = mcts_internal::GameAdapter::current_player(game);
      path_.push_back(entry);

      // Apply action
      if (!mcts_internal::GameAdapter::decode_and_apply(game, encoder_,
                                                        action)) {
        auto value =
            mcts_internal::GameAdapter::terminal_value(game, 0.01f);
        backpropagate(value);
        return false;
      }

      // Get next state hash
      current_hash = mcts_internal::GameAdapter::hash(
          game, static_cast<uint8_t>(observer),
          mcts_.config().use_determinization);
      depth++;
    }

    // Max depth reached
    std::array<float, NUM_PLAYERS> timeout_value = {0.01f, 0.01f};
    backpropagate(timeout_value);
    return false;
  }

public:
  // Expand leaf nodes with inference results and backpropagate
  void expand_and_backpropagate(const std::vector<LeafRequest> &requests,
                                const std::vector<InferenceResult> &results) {
    for (size_t i = 0; i < requests.size() && i < results.size(); ++i) {
      const auto &req = requests[i];
      const auto &res = results[i];

      // Expand the leaf node
      mcts_.expand_node(req.hash, res.policy, res.value, req.valid_actions);

      // Backpropagate through the path
      if (i < pending_paths_.size()) {
        mcts_.backpropagate(pending_paths_[i], res.value);
      }
    }

    pending_paths_.clear();
  }

  // Run full MCTS search with batch inference callback
  // inference_fn: takes vector<LeafRequest>, returns vector<InferenceResult>
  void search(const Game &root_game, int num_simulations,
              std::function<std::vector<InferenceResult>(
                  const std::vector<LeafRequest> &)> inference_fn) {
    if (num_simulations > 0 &&
        mcts_internal::GameAdapter::requires_forced_pass(root_game))
      throw std::invalid_argument(
          "MCTS root requires a forced pass; apply it before searching");
    mcts_.prune_if_needed();

    int observer = mcts_internal::GameAdapter::current_player(root_game);

    for (int sim = 0; sim < num_simulations; ++sim) {
      std::vector<LeafRequest> leaf_requests;

      bool has_leaf;
      if (mcts_.config().use_determinization) {
        // shuffled_clone() already omits history. Transfer its temporary
        // directly instead of full-copying root and copying the shuffled
        // board once more inside run_simulation().
        has_leaf = run_simulation_owned(
            mcts_internal::GameAdapter::determinize(
                root_game, static_cast<uint8_t>(observer), rng_()),
            observer, leaf_requests, sim == 0, sim);
      } else {
        // run_simulation() makes the one full copy required by the observable
        // Python callback history contract.
        has_leaf =
            run_simulation(root_game, observer, leaf_requests, sim == 0, sim);
      }

      if (has_leaf && !leaf_requests.empty()) {
        // Call inference
        auto results = inference_fn(leaf_requests);
        expand_and_backpropagate(leaf_requests, results);
      }
    }
  }

  // Get action probabilities
  std::array<float, MAX_ACTIONS> get_action_probs(const Game &root_game,
                                                   float temperature) {
    if (mcts_internal::GameAdapter::requires_forced_pass(root_game))
      throw std::invalid_argument(
          "MCTS root requires a forced pass; apply it before reading policy");
    int observer = mcts_internal::GameAdapter::current_player(root_game);
    uint64_t root_hash = mcts_internal::GameAdapter::hash(
        root_game, static_cast<uint8_t>(observer),
        mcts_.config().use_determinization);
    return mcts_.get_action_probs(root_hash, temperature);
  }

private:
  void backpropagate(const std::array<float, NUM_PLAYERS> &value) {
    mcts_.backpropagate(path_, value);
  }

  MCTS &mcts_;
  IFeaturizer &featurizer_;
  IActionEncoder &encoder_;
  std::vector<PathEntry> path_;
  std::vector<std::vector<PathEntry>> pending_paths_;
  std::mt19937 rng_{std::random_device{}()};
};

#endif // CSPLENDOR_MCTS_SEARCHER_H
