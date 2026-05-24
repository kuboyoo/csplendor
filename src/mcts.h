#ifndef CSPLENDOR_MCTS_H
#define CSPLENDOR_MCTS_H

#include "action.h"
#include "action_encoder.h"
#include "game.h"
#include "state_encoder.h"
#include <array>
#include <cmath>
#include <random>
#include <unordered_map>
#include <vector>

// Constants
static constexpr size_t MAX_ACTIONS = 48;    // ActionEncoder.BASE_ACTION_COUNT
static constexpr size_t NUM_PLAYERS = 2;
static constexpr float EPS = 1e-8f;
static constexpr int MAX_DEPTH = 300;
static constexpr size_t MAX_TREE_SIZE = 50000;
static constexpr size_t PRUNE_THRESHOLD = 40000;
static constexpr size_t FEATURE_SIZE = 196;

// Action encoder interface (for getting valid action masks)
class IActionEncoder {
public:
  virtual ~IActionEncoder() = default;
  virtual int encode(const Action &action, const Game &game) = 0;
  virtual Action decode(int action_idx, const Game &game) = 0;
  virtual std::array<uint8_t, MAX_ACTIONS> get_action_mask(const Game &game) = 0;
};

// MCTS Node - stores statistics for a game state
struct MCTSNode {
  std::array<uint8_t, MAX_ACTIONS> valid_actions = {0};  // Valid action mask
  std::array<float, MAX_ACTIONS> prior = {0};            // Policy prior P(s,a)
  std::array<float, MAX_ACTIONS> Q = {0};                // Action value Q(s,a)
  std::array<uint32_t, MAX_ACTIONS> N = {0};             // Visit count N(s,a)
  std::array<int32_t, MAX_ACTIONS> virtual_loss = {0};   // Virtual loss for parallel MCTS
  uint32_t total_visits = 0;                             // N(s) = sum of N(s,a)
  std::array<float, NUM_PLAYERS> value = {0};            // Value estimate v(s)
  bool is_terminal = false;
  bool is_expanded = false;
};

// Leaf node request for batch inference
struct LeafRequest {
  uint64_t hash;                                         // State hash
  std::array<float, 196> features;                       // Encoded board features
  std::array<uint8_t, MAX_ACTIONS> valid_actions;        // Valid action mask
  int path_index;                                        // Index in search path
};

// Search path entry for backpropagation
struct PathEntry {
  uint64_t hash;
  int action;
  int player;  // Player who made the move
};

// MCTS Configuration
struct MCTSConfig {
  float cpuct = 1.5f;
  float dirichlet_alpha = 0.3f;
  float dirichlet_epsilon = 0.25f;
  bool use_dirichlet_noise = true;
  bool use_determinization = false;
  int num_simulations = 25;
  int num_determinizations = 1;  // Number of shuffled worlds to average

  // FPU (First Play Urgency) - value for unvisited nodes
  // Negative value: use as absolute FPU value
  // Positive value: parent value reduction (parent_value - fpu)
  float fpu = 0.0f;

  // Forced playouts - ensure high-policy moves get explored
  bool forced_playouts = false;
  float forced_playouts_k = 0.5f;  // k parameter for sqrt(k * P * N)
};

// Batch simulation leaf data - information needed for NN evaluation
struct BatchLeafData {
  uint64_t hash;                                    // Observable state hash
  std::vector<PathEntry> path;                      // Path from root to this leaf
  std::vector<std::array<float, FEATURE_SIZE>> encoded_boards;  // Multi-world encoded boards
  std::vector<std::array<uint8_t, MAX_ACTIONS>> valid_actions;  // Multi-world valid actions
  int num_worlds;                                   // Number of worlds evaluated
};

// Result of batch simulation preparation
struct BatchSimulationRequest {
  std::vector<BatchLeafData> leaves;                // Leaf nodes needing evaluation
  std::vector<std::pair<std::vector<PathEntry>, std::array<float, NUM_PLAYERS>>> terminals;  // Terminal paths with values
  int total_boards;                                 // Total boards to evaluate (for batching)
};

// C++ MCTS Implementation
class MCTS {
public:
  explicit MCTS(const MCTSConfig &config) : config_(config), rng_(std::random_device{}()) {}

  // Clear the search tree
  void clear() {
    nodes_.clear();
    access_count_.clear();
    access_counter_ = 0;
  }

  // Get node by hash, returns nullptr if not found
  MCTSNode *get_node(uint64_t hash) {
    auto it = nodes_.find(hash);
    if (it != nodes_.end()) {
      // Update access count for LRU
      access_count_[hash] = ++access_counter_;
      return &it->second;
    }
    return nullptr;
  }

  // Create or get node
  MCTSNode &get_or_create_node(uint64_t hash) {
    access_count_[hash] = ++access_counter_;
    return nodes_[hash];
  }

  // Select action using PUCT formula
  // Now supports FPU (First Play Urgency) and forced playouts
  int select_action(const MCTSNode &node, bool is_root, int current_sim = 0) {
    float best_ucb = -1e9f;
    int best_action = -1;

    float sqrt_total = std::sqrt(static_cast<float>(node.total_visits) + EPS);

    // Compute FPU value based on config
    // Note: node.value[0] is always player 0's value, not necessarily the current player's.
    // Using a fixed FPU is safer since we don't track current player per node.
    float fpu_init;
    if (config_.fpu < 0) {
      fpu_init = config_.fpu;  // Use absolute negative value
    } else if (config_.fpu > 0) {
      // Parent-based reduction: use a conservative estimate
      fpu_init = -config_.fpu;  // Penalize unvisited actions
    } else {
      fpu_init = 0.0f;  // Neutral FPU for fpu=0
    }

    // Apply Dirichlet noise at root if enabled
    std::array<float, MAX_ACTIONS> noise = {0};
    if (is_root && config_.use_dirichlet_noise) {
      generate_dirichlet_noise(node, noise);
    }

    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (!node.valid_actions[a])
        continue;

      // Forced playouts: if visit count is below threshold, force this action
      if (config_.forced_playouts && current_sim > 0) {
        float p = node.prior[a];
        int threshold = static_cast<int>(std::sqrt(config_.forced_playouts_k * p * current_sim));
        if (static_cast<int>(node.N[a]) < threshold) {
          return static_cast<int>(a);
        }
      }

      float q = (node.N[a] > 0) ? node.Q[a] : fpu_init;
      float p = node.prior[a];

      // Mix prior with Dirichlet noise at root
      if (is_root && config_.use_dirichlet_noise) {
        p = (1.0f - config_.dirichlet_epsilon) * p +
            config_.dirichlet_epsilon * noise[a];
      }

      float ucb = q + config_.cpuct * p * sqrt_total / (1.0f + node.N[a]);

      if (ucb > best_ucb) {
        best_ucb = ucb;
        best_action = static_cast<int>(a);
      }
    }

    return best_action;
  }

  // Select action with virtual loss consideration for parallel MCTS
  // Virtual loss is applied more gently to allow some concentration on good moves
  // Now supports FPU (First Play Urgency) and forced playouts
  int select_action_with_virtual_loss(uint64_t hash, bool is_root,
                                       const std::array<float, MAX_ACTIONS> *dirichlet_noise = nullptr,
                                       int current_sim = 0) {
    auto it = nodes_.find(hash);
    if (it == nodes_.end())
      return -1;

    MCTSNode &node = it->second;
    float best_ucb = -1e9f;
    int best_action = -1;

    // Virtual loss weight - reduced from 1.0 to allow more concentration
    constexpr float VL_WEIGHT = 0.3f;

    // Include virtual loss in total visits (weighted)
    float total_vl = 0.0f;
    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      total_vl += node.virtual_loss[a] * VL_WEIGHT;
    }

    float sqrt_total = std::sqrt(static_cast<float>(node.total_visits) + total_vl + EPS);

    // Compute FPU value based on config
    // Note: node.value[0] is always player 0's value, not necessarily the current player's.
    // Using a fixed FPU is safer since we don't track current player per node.
    float fpu_init;
    if (config_.fpu < 0) {
      fpu_init = config_.fpu;  // Use absolute negative value
    } else if (config_.fpu > 0) {
      // Parent-based reduction: use a conservative estimate
      fpu_init = -config_.fpu;  // Penalize unvisited actions
    } else {
      fpu_init = 0.0f;  // Neutral FPU for fpu=0
    }

    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (!node.valid_actions[a])
        continue;

      // Forced playouts: if visit count is below threshold, force this action
      if (config_.forced_playouts && current_sim > 0) {
        float p = node.prior[a];
        int threshold = static_cast<int>(std::sqrt(config_.forced_playouts_k * p * current_sim));
        if (static_cast<int>(node.N[a]) < threshold) {
          return static_cast<int>(a);
        }
      }

      // Effective visit count includes weighted virtual loss
      float effective_n = static_cast<float>(node.N[a]) + node.virtual_loss[a] * VL_WEIGHT;

      // Q value with gentle virtual loss penalty
      float q;
      if (node.N[a] > 0) {
        // Blend actual Q with virtual loss (less pessimistic)
        float vl_penalty = node.virtual_loss[a] * VL_WEIGHT * 0.5f;  // Reduced penalty
        q = (node.Q[a] * node.N[a] - vl_penalty) / (node.N[a] + vl_penalty);
      } else if (node.virtual_loss[a] > 0) {
        q = fpu_init - 0.2f;  // Slight penalty, not full loss
      } else {
        q = fpu_init;
      }

      float p = node.prior[a];

      // Mix prior with Dirichlet noise at root
      if (is_root && dirichlet_noise != nullptr) {
        p = (1.0f - config_.dirichlet_epsilon) * p +
            config_.dirichlet_epsilon * (*dirichlet_noise)[a];
      }

      float ucb = q + config_.cpuct * p * sqrt_total / (1.0f + effective_n);

      if (ucb > best_ucb) {
        best_ucb = ucb;
        best_action = static_cast<int>(a);
      }
    }

    return best_action;
  }

  // Add virtual loss to an action
  void add_virtual_loss(uint64_t hash, int action) {
    auto it = nodes_.find(hash);
    if (it != nodes_.end() && action >= 0 && action < static_cast<int>(MAX_ACTIONS)) {
      it->second.virtual_loss[action]++;
    }
  }

  // Remove virtual loss from an action
  void remove_virtual_loss(uint64_t hash, int action) {
    auto it = nodes_.find(hash);
    if (it != nodes_.end() && action >= 0 && action < static_cast<int>(MAX_ACTIONS)) {
      it->second.virtual_loss[action]--;
    }
  }

  // Clear all virtual losses (for safety after batch)
  void clear_virtual_losses() {
    for (auto &kv : nodes_) {
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        kv.second.virtual_loss[a] = 0;
      }
    }
  }

  // Generate Dirichlet noise (public version for batch MCTS)
  std::array<float, MAX_ACTIONS> generate_dirichlet_noise_for_node(uint64_t hash) {
    std::array<float, MAX_ACTIONS> noise = {0};
    auto it = nodes_.find(hash);
    if (it != nodes_.end()) {
      generate_dirichlet_noise(it->second, noise);
    }
    return noise;
  }

  // Backpropagate value through the search path
  void backpropagate(const std::vector<PathEntry> &path,
                     const std::array<float, NUM_PLAYERS> &value) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      MCTSNode *node = get_node(it->hash);
      if (!node)
        continue;

      int a = it->action;
      if (a < 0 || a >= static_cast<int>(MAX_ACTIONS))
        continue;

      // Roll value to be relative to the player who made the move
      float v = value[(it->player) % NUM_PLAYERS];

      // Incremental mean update
      node->N[a]++;
      node->total_visits++;
      node->Q[a] += (v - node->Q[a]) / static_cast<float>(node->N[a]);
    }
  }

  // Expand a leaf node with NN predictions
  void expand_node(uint64_t hash, const std::array<float, MAX_ACTIONS> &policy,
                   const std::array<float, NUM_PLAYERS> &value,
                   const std::array<uint8_t, MAX_ACTIONS> &valid_actions) {
    MCTSNode &node = get_or_create_node(hash);
    node.valid_actions = valid_actions;
    node.value = value;
    node.is_expanded = true;

    // Normalize policy over valid actions
    float policy_sum = 0.0f;
    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (valid_actions[a]) {
        policy_sum += policy[a];
      }
    }

    if (policy_sum > EPS) {
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        node.prior[a] = valid_actions[a] ? (policy[a] / policy_sum) : 0.0f;
      }
    } else {
      // Uniform over valid actions
      int num_valid = 0;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        if (valid_actions[a])
          num_valid++;
      }
      float uniform_p = (num_valid > 0) ? 1.0f / num_valid : 0.0f;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        node.prior[a] = valid_actions[a] ? uniform_p : 0.0f;
      }
    }
  }

  // Mark node as terminal
  void set_terminal(uint64_t hash,
                    const std::array<float, NUM_PLAYERS> &terminal_value) {
    MCTSNode &node = get_or_create_node(hash);
    node.is_terminal = true;
    node.is_expanded = true;
    node.value = terminal_value;
  }

  // Get action probabilities after search
  std::array<float, MAX_ACTIONS> get_action_probs(uint64_t root_hash,
                                                   float temperature) const {
    std::array<float, MAX_ACTIONS> probs = {0};

    auto it = nodes_.find(root_hash);
    if (it == nodes_.end()) {
      return probs;
    }

    const MCTSNode &node = it->second;

    if (temperature < EPS) {
      // Greedy: pick best action(s)
      uint32_t max_n = 0;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        if (node.N[a] > max_n)
          max_n = node.N[a];
      }
      int num_best = 0;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        if (node.N[a] == max_n && max_n > 0)
          num_best++;
      }
      if (num_best > 0) {
        float p = 1.0f / num_best;
        for (size_t a = 0; a < MAX_ACTIONS; ++a) {
          probs[a] = (node.N[a] == max_n && max_n > 0) ? p : 0.0f;
        }
      }
    } else {
      // Softmax with temperature
      float sum = 0.0f;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        if (node.N[a] > 0) {
          probs[a] = std::pow(static_cast<float>(node.N[a]), 1.0f / temperature);
          sum += probs[a];
        }
      }
      if (sum > EPS) {
        for (size_t a = 0; a < MAX_ACTIONS; ++a) {
          probs[a] /= sum;
        }
      }
    }

    return probs;
  }

  // Prune tree if too large (LRU-based)
  void prune_if_needed() {
    if (nodes_.size() < MAX_TREE_SIZE)
      return;

    // Find threshold access count to keep PRUNE_THRESHOLD nodes
    std::vector<uint64_t> access_counts;
    access_counts.reserve(access_count_.size());
    for (const auto &kv : access_count_) {
      access_counts.push_back(kv.second);
    }
    std::sort(access_counts.begin(), access_counts.end(), std::greater<uint64_t>());

    uint64_t threshold = 0;
    if (access_counts.size() > PRUNE_THRESHOLD) {
      threshold = access_counts[PRUNE_THRESHOLD];
    }

    // Remove nodes below threshold
    std::vector<uint64_t> to_remove;
    for (const auto &kv : nodes_) {
      auto it = access_count_.find(kv.first);
      if (it == access_count_.end() || it->second < threshold) {
        to_remove.push_back(kv.first);
      }
    }

    for (uint64_t hash : to_remove) {
      nodes_.erase(hash);
      access_count_.erase(hash);
    }

    // Reset access counter if needed
    if (access_counter_ > 1000000) {
      access_counter_ = 0;
      for (auto &kv : access_count_) {
        kv.second = 0;
      }
    }
  }

  // Update node statistics after backpropagation
  void update_stats(uint64_t hash, int action, float value) {
    auto it = nodes_.find(hash);
    if (it == nodes_.end())
      return;

    MCTSNode &node = it->second;
    if (action < 0 || action >= static_cast<int>(MAX_ACTIONS))
      return;

    // Incremental mean update
    node.N[action]++;
    node.total_visits++;
    node.Q[action] += (value - node.Q[action]) / static_cast<float>(node.N[action]);
  }

  /**
   * Run batch simulations and prepare data for NN evaluation.
   * This method performs selection with virtual loss and collects leaf nodes
   * for batch evaluation across multiple shuffled worlds.
   *
   * Uses the native C++ ActionEncoderCpp to avoid GIL contention.
   *
   * @param root_game The root game state
   * @param observer The observing player (for determinization)
   * @param batch_size Number of simulations in this batch
   * @param num_determinizations Number of shuffled worlds per leaf
   * @param dirichlet_noise Pre-generated Dirichlet noise for root (or nullptr)
   * @return BatchSimulationRequest containing leaves and terminals
   */
  BatchSimulationRequest prepare_batch_simulations(
      const Game &root_game,
      uint8_t observer,
      int batch_size,
      int num_determinizations,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise) {

    BatchSimulationRequest result;
    result.total_boards = 0;

    uint64_t root_hash = config_.use_determinization
                             ? root_game.board.observable_hash(observer)
                             : root_game.board.hash();

    for (int sim = 0; sim < batch_size; ++sim) {
      // Create shuffled game for this simulation
      Game search_game = config_.use_determinization
                             ? root_game.shuffled_clone(observer, rng_())
                             : root_game.clone_light();

      std::vector<PathEntry> path;
      uint64_t current_hash = root_hash;
      int depth = 0;

      while (depth < MAX_DEPTH) {
        MCTSNode *node = get_node(current_hash);

        // Check terminal state
        if (search_game.is_game_over()) {
          std::array<float, NUM_PLAYERS> terminal_value = {0};
          int winner = search_game.winner();
          if (winner == 0) {
            terminal_value[0] = 1.0f;
            terminal_value[1] = -1.0f;
          } else if (winner == 1) {
            terminal_value[0] = -1.0f;
            terminal_value[1] = 1.0f;
          }
          result.terminals.push_back({path, terminal_value});
          break;
        }

        // Leaf node - needs expansion
        if (node == nullptr || !node->is_expanded) {
          BatchLeafData leaf;
          leaf.hash = current_hash;
          leaf.path = path;
          leaf.num_worlds = num_determinizations;

          // Generate multiple shuffled worlds for evaluation
          for (int w = 0; w < num_determinizations; ++w) {
            Game world_game = (w == 0) ? search_game
                                       : root_game.shuffled_clone(observer, rng_());

            // Replay path in this world using encoded action IDs
            bool path_valid = true;
            for (const auto &entry : path) {
              // entry.action is an encoded action ID (0-47), not an index
              // Decode it to get the actual Action for this world's game state
              Action decoded_action = ActionEncoderCpp::decode(entry.action, world_game);

              // Check if the decoded action is valid (type != ACTION_TYPE_COUNT)
              if (decoded_action.type == ACTION_TYPE_COUNT) {
                // Action not legal in this world - path is invalid
                path_valid = false;
                break;
              }

              if (!world_game.apply(decoded_action, false)) {
                path_valid = false;
                break;
              }
              if (world_game.is_game_over()) {
                path_valid = false;
                break;
              }
            }

            if (path_valid) {
              // Encode board with observer perspective
              auto encoded = StateEncoder::encode_canonical(
                  world_game, 0, observer);
              leaf.encoded_boards.push_back(encoded);

              // Get valid actions using C++ ActionEncoder (no GIL!)
              auto mask = ActionEncoderCpp::get_action_mask(world_game);
              leaf.valid_actions.push_back(mask);
            }
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
        int action = select_action_with_virtual_loss(
            current_hash, is_root, dirichlet_noise);

        if (action < 0) {
          // No valid action - treat as terminal
          std::array<float, NUM_PLAYERS> v = {0};
          result.terminals.push_back({path, v});
          break;
        }

        // Add virtual loss
        add_virtual_loss(current_hash, action);

        // Record path (action is the encoded action ID)
        int current_player = search_game.current_player();
        path.push_back({current_hash, action, current_player});

        // Apply action - decode the encoded action ID to get the actual Action
        Action decoded_action = ActionEncoderCpp::decode(action, search_game);
        if (decoded_action.type == ACTION_TYPE_COUNT ||
            !search_game.apply(decoded_action, false)) {
          std::array<float, NUM_PLAYERS> v = {0};
          result.terminals.push_back({path, v});
          break;
        }

        current_hash = config_.use_determinization
                           ? search_game.board.observable_hash(observer)
                           : search_game.board.hash();
        depth++;
      }
    }

    return result;
  }

  // Legacy overload for backward compatibility (deprecated)
  BatchSimulationRequest prepare_batch_simulations(
      const Game &root_game,
      uint8_t observer,
      int batch_size,
      int num_determinizations,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise,
      IActionEncoder & /*encoder*/) {
    // Ignore the encoder parameter and use the native C++ implementation
    return prepare_batch_simulations(root_game, observer, batch_size,
                                     num_determinizations, dirichlet_noise);
  }

  /**
   * Apply NN evaluation results to the tree.
   * This method expands leaf nodes and backpropagates values.
   *
   * @param request The batch simulation request (contains paths)
   * @param policies NN policy outputs (flattened, num_leaves * num_worlds)
   * @param values NN value outputs (flattened, num_leaves * num_worlds * 2)
   */
  void apply_batch_results(
      const BatchSimulationRequest &request,
      const std::vector<std::array<float, MAX_ACTIONS>> &policies,
      const std::vector<std::array<float, NUM_PLAYERS>> &values) {

    size_t result_idx = 0;

    for (const auto &leaf : request.leaves) {
      // Average policy and value across worlds
      std::array<float, MAX_ACTIONS> avg_policy = {0};
      std::array<float, NUM_PLAYERS> avg_value = {0};
      std::array<uint8_t, MAX_ACTIONS> combined_valid = {0};

      for (int w = 0; w < leaf.num_worlds; ++w) {
        for (size_t a = 0; a < MAX_ACTIONS; ++a) {
          avg_policy[a] += policies[result_idx][a];
          if (leaf.valid_actions[w][a]) {
            combined_valid[a] = 1;
          }
        }
        for (size_t p = 0; p < NUM_PLAYERS; ++p) {
          avg_value[p] += values[result_idx][p];
        }
        result_idx++;
      }

      // Normalize
      float world_count = static_cast<float>(leaf.num_worlds);
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        avg_policy[a] /= world_count;
      }
      for (size_t p = 0; p < NUM_PLAYERS; ++p) {
        avg_value[p] /= world_count;
      }

      // Re-normalize policy over valid actions
      float policy_sum = 0.0f;
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        if (combined_valid[a]) {
          policy_sum += avg_policy[a];
        } else {
          avg_policy[a] = 0.0f;
        }
      }
      if (policy_sum > EPS) {
        for (size_t a = 0; a < MAX_ACTIONS; ++a) {
          avg_policy[a] /= policy_sum;
        }
      }

      // Expand node
      expand_node(leaf.hash, avg_policy, avg_value, combined_valid);

      // Remove virtual losses and backpropagate
      backpropagate_with_virtual_loss_removal(leaf.path, avg_value);
    }

    // Handle terminal paths
    for (const auto &[path, value] : request.terminals) {
      backpropagate_with_virtual_loss_removal(path, value);
    }
  }

  /**
   * Backpropagate value and remove virtual losses along the path.
   */
  void backpropagate_with_virtual_loss_removal(
      const std::vector<PathEntry> &path,
      const std::array<float, NUM_PLAYERS> &value) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      // Remove virtual loss
      remove_virtual_loss(it->hash, it->action);

      // Get value relative to the player who made the move
      float v = value[it->player % NUM_PLAYERS];

      // Update statistics
      update_stats(it->hash, it->action, v);
    }
  }

  // Get statistics
  size_t tree_size() const { return nodes_.size(); }

  const MCTSConfig &config() const { return config_; }
  MCTSConfig &config() { return config_; }

private:
  void generate_dirichlet_noise(const MCTSNode &node,
                                std::array<float, MAX_ACTIONS> &noise) {
    // Count valid actions
    int num_valid = 0;
    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (node.valid_actions[a])
        num_valid++;
    }

    if (num_valid == 0)
      return;

    // Generate Dirichlet samples using Gamma distribution
    std::gamma_distribution<float> gamma(config_.dirichlet_alpha, 1.0f);
    float sum = 0.0f;

    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (node.valid_actions[a]) {
        noise[a] = gamma(rng_);
        sum += noise[a];
      }
    }

    // Normalize
    if (sum > EPS) {
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        noise[a] /= sum;
      }
    }
  }

  MCTSConfig config_;
  std::unordered_map<uint64_t, MCTSNode> nodes_;
  std::unordered_map<uint64_t, uint64_t> access_count_;
  uint64_t access_counter_ = 0;
  std::mt19937 rng_;
};

#endif // CSPLENDOR_MCTS_H
