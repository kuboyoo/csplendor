#ifndef CSPLENDOR_MCTS_TYPES_H
#define CSPLENDOR_MCTS_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

// Public MCTS constants. These remain available through the mcts.h facade.
static constexpr size_t MAX_ACTIONS = 48; // ActionEncoder.BASE_ACTION_COUNT
static constexpr size_t NUM_PLAYERS = 2;
static constexpr float EPS = 1e-8f;
static constexpr int MAX_DEPTH = 300;
static constexpr size_t MAX_TREE_SIZE = 50000;
static constexpr size_t PRUNE_THRESHOLD = 40000;
static constexpr size_t FEATURE_SIZE = 196;

// MCTS Node - stores statistics for a game state
struct MCTSNode {
  std::array<uint8_t, MAX_ACTIONS> valid_actions = {0}; // Valid action mask
  std::array<float, MAX_ACTIONS> prior = {0};           // Policy prior P(s,a)
  std::array<float, MAX_ACTIONS> Q = {0};               // Action value Q(s,a)
  std::array<uint32_t, MAX_ACTIONS> N = {0};            // Visit count N(s,a)
  std::array<int32_t, MAX_ACTIONS> virtual_loss = {
      0};                                     // Virtual loss for parallel MCTS
  uint32_t total_visits = 0;                  // N(s) = sum of N(s,a)
  std::array<float, NUM_PLAYERS> value = {0}; // Value estimate v(s)
  bool is_terminal = false;
  bool is_expanded = false;
};

// Leaf node request for batch inference
struct LeafRequest {
  uint64_t hash = 0;                              // State hash
  std::array<float, FEATURE_SIZE> features{};      // Encoded board features
  std::array<uint8_t, MAX_ACTIONS> valid_actions{}; // Valid action mask
  int path_index = 0;                             // Index in search path
};

// Search path entry for backpropagation
struct PathEntry {
  uint64_t hash = 0;
  int action = 0;
  int player = 0; // Player who made the move
};

// MCTS Configuration
struct MCTSConfig {
  float cpuct = 1.5f;
  float dirichlet_alpha = 0.3f;
  float dirichlet_epsilon = 0.25f;
  bool use_dirichlet_noise = true;
  // Real-game searches key the tree by public information and sample hidden
  // information. Full-information search remains available for analysis.
  bool use_determinization = true;
  int num_simulations = 25;
  // Hidden-information search currently supports one independently sampled
  // world per logical simulation.  Values >1 remain meaningful only for the
  // legacy full-information batch replication path.
  int num_determinizations = 1;

  // FPU (First Play Urgency) - value for unvisited nodes
  // Negative value: use as absolute FPU value
  // Positive value: parent value reduction (parent_value - fpu)
  float fpu = 0.0f;

  // Forced playouts - ensure high-policy moves get explored
  bool forced_playouts = false;
  float forced_playouts_k = 0.5f; // k parameter for sqrt(k * P * N)
};

// Batch simulation leaf data - information needed for NN evaluation
struct BatchLeafData {
  uint64_t hash = 0;           // Observable state hash
  std::vector<PathEntry> path; // Path from root to this leaf
  std::vector<std::array<float, FEATURE_SIZE>>
      encoded_boards; // Multi-world encoded boards
  std::vector<std::array<uint8_t, MAX_ACTIONS>>
      valid_actions; // Multi-world valid actions
  int num_worlds = 0; // Number of worlds evaluated
};

// Result of batch simulation preparation
struct BatchSimulationRequest {
  std::vector<BatchLeafData> leaves; // Leaf nodes needing evaluation
  std::vector<std::pair<std::vector<PathEntry>, std::array<float, NUM_PLAYERS>>>
      terminals;    // Terminal paths with values
  int total_boards = 0; // Total boards to evaluate (for batching)
  // A clear or tree-domain change invalidates an outstanding split batch.
  // Interleaving arbitrary manual mutations between prepare/apply remains a
  // legacy single-thread-only contract; parallel search uses owned tickets.
  uint64_t tree_generation = 0;
};

#endif // CSPLENDOR_MCTS_TYPES_H
