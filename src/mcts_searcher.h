#ifndef CSPLENDOR_MCTS_SEARCHER_H
#define CSPLENDOR_MCTS_SEARCHER_H

#include "game.h"
#include "mcts.h"
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
  std::array<float, MAX_ACTIONS> policy;
  std::array<float, NUM_PLAYERS> value;
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
                      bool is_first_sim) {
    path_.clear();

    Game game = root_game;
    uint64_t current_hash = get_hash(game, observer);
    int depth = 0;

    while (depth < MAX_DEPTH) {
      MCTSNode *node = mcts_.get_node(current_hash);

      // Check for terminal state
      if (game.is_game_over()) {
        auto terminal_value = get_terminal_value(game);
        mcts_.set_terminal(current_hash, terminal_value);
        backpropagate(terminal_value);
        return false;
      }

      // Leaf node - needs expansion
      if (!node || !node->is_expanded) {
        LeafRequest req;
        req.hash = current_hash;
        req.features = featurizer_.featurize(game);
        req.valid_actions = encoder_.get_action_mask(game);
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
      int action = mcts_.select_action(*node, is_first_sim && depth == 0);
      if (action < 0) {
        // No valid action - treat as terminal
        auto value = get_terminal_value(game);
        backpropagate(value);
        return false;
      }

      // Record path for backpropagation
      PathEntry entry;
      entry.hash = current_hash;
      entry.action = action;
      entry.player = game.current_player();
      path_.push_back(entry);

      // Apply action
      Action decoded = encoder_.decode(action, game);
      if (!game.apply(decoded, false)) {
        auto value = get_terminal_value(game);
        backpropagate(value);
        return false;
      }

      // Get next state hash
      current_hash = get_hash(game, observer);
      depth++;
    }

    // Max depth reached
    std::array<float, NUM_PLAYERS> timeout_value = {0.01f, 0.01f};
    backpropagate(timeout_value);
    return false;
  }

  // Process inference results and complete backpropagation
  void process_inference_results(const std::vector<InferenceResult> &results) {
    for (size_t i = 0; i < results.size() && i < pending_paths_.size(); ++i) {
      const auto &result = results[i];

      // Get the leaf request to find the hash
      // Note: The leaf hash is stored at the end of the corresponding path
      // We need to track it separately
    }
  }

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
    mcts_.prune_if_needed();

    int observer = root_game.current_player();

    for (int sim = 0; sim < num_simulations; ++sim) {
      std::vector<LeafRequest> leaf_requests;

      // Optionally use determinization
      Game search_game = root_game;
      if (mcts_.config().use_determinization) {
        search_game = root_game.shuffled_clone(observer, rng_());
      }

      bool has_leaf = run_simulation(search_game, observer, leaf_requests,
                                     sim == 0);

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
    int observer = root_game.current_player();
    uint64_t root_hash = get_hash(root_game, observer);
    return mcts_.get_action_probs(root_hash, temperature);
  }

private:
  uint64_t get_hash(const Game &game, int observer) const {
    if (mcts_.config().use_determinization) {
      return game.board.observable_hash(observer);
    }
    return game.board.hash();
  }

  std::array<float, NUM_PLAYERS> get_terminal_value(const Game &game) const {
    std::array<float, NUM_PLAYERS> value = {0};
    int winner = game.winner();

    if (winner == 0) {
      value[0] = 1.0f;
      value[1] = -1.0f;
    } else if (winner == 1) {
      value[0] = -1.0f;
      value[1] = 1.0f;
    } else if (winner == -2) {
      // Draw
      value[0] = 0.01f;
      value[1] = 0.01f;
    }

    return value;
  }

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
