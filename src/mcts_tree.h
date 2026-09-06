#ifndef CSPLENDOR_MCTS_TREE_H
#define CSPLENDOR_MCTS_TREE_H

#include "mcts_concurrent_tree.h"
#include "mcts_types.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
#define CSPLENDOR_MCTS_LEGACY_TREE_RECORDS 1
#endif

class Game;
class IActionEncoder;

// C++ MCTS tree and statistics. Game-dependent orchestration is defined in
// mcts_orchestration.h and remains available through the mcts.h facade.
class MCTS {
public:
  class SearchGuard {
  public:
    SearchGuard(const SearchGuard &) = delete;
    SearchGuard &operator=(const SearchGuard &) = delete;
    SearchGuard(SearchGuard &&other) noexcept;
    SearchGuard &operator=(SearchGuard &&other) noexcept;
    ~SearchGuard() noexcept;

    const MCTSConfig &config() const noexcept { return config_snapshot_; }
    uint64_t tree_generation() const noexcept { return tree_generation_; }
    uint64_t resolved_master_seed() const noexcept {
      return resolved_master_seed_;
    }
    uint64_t search_nonce() const noexcept { return search_nonce_; }
    uint64_t search_id() const noexcept { return search_id_; }
    bool active() const noexcept { return owner_ != nullptr; }
    bool tree_prepared() const noexcept { return tree_prepared_; }
    void finish();

  private:
    friend class MCTS;
    SearchGuard(MCTS *owner, MCTSConfig config, uint64_t tree_generation,
                uint64_t resolved_master_seed, uint64_t search_nonce,
                uint64_t search_id) noexcept
        : owner_(owner), config_snapshot_(config),
          tree_generation_(tree_generation),
          resolved_master_seed_(resolved_master_seed),
          search_nonce_(search_nonce), search_id_(search_id) {}

    void finish_noexcept() noexcept;

    MCTS *owner_ = nullptr;
    MCTSConfig config_snapshot_{};
    uint64_t tree_generation_ = 0;
    uint64_t resolved_master_seed_ = 0;
    uint64_t search_nonce_ = 0;
    uint64_t search_id_ = 0;
    bool tree_prepared_ = false;
  };

  explicit MCTS(const MCTSConfig &config)
      : MCTS(config, resolve_mcts_entropy_seed()) {}

  MCTS(const MCTSConfig &config, uint64_t default_parallel_seed)
      : config_(config), default_parallel_seed_(default_parallel_seed),
        parallel_tree_(0),
        rng_(static_cast<std::mt19937::result_type>(
            default_parallel_seed_ ^ (default_parallel_seed_ >> 32))) {}

  MCTS(const MCTS &) = delete;
  MCTS &operator=(const MCTS &) = delete;
  MCTS(MCTS &&) = delete;
  MCTS &operator=(MCTS &&) = delete;

  SearchGuard begin_parallel_search() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (parallel_active_.load(std::memory_order_acquire))
      throw std::logic_error("an MCTS search is already active");
    if (next_search_id_ == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("MCTS search identifier is exhausted");
    if (next_search_nonce_ == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("MCTS search nonce is exhausted");
    parallel_active_.store(true, std::memory_order_release);
    const uint64_t search_id = ++next_search_id_;
    active_search_id_.store(search_id, std::memory_order_release);
    const uint64_t nonce = next_search_nonce_++;
    return SearchGuard(this, config_, tree_generation_.load(),
                       default_parallel_seed_, nonce, search_id);
  }

  // Reset only the replay identity sequence. The search tree and its
  // generation deliberately remain untouched so callers can replay from a
  // retained tree without silently invalidating node handles.
  void reset_replay_sequence(uint64_t seed, uint64_t nonce) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (parallel_active_.load(std::memory_order_acquire))
      throw std::logic_error(
          "cannot reset MCTS replay sequence during active search");
    if (nonce == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("MCTS search nonce is exhausted");
    parallel_tree_.validate_quiescent();
    default_parallel_seed_ = seed;
    next_search_nonce_ = nonce;
  }

  bool is_parallel_search_active() const noexcept {
    return parallel_active_.load(std::memory_order_acquire);
  }

  uint64_t tree_generation_snapshot() const noexcept {
    return tree_generation_.load(std::memory_order_acquire);
  }

  MCTSConfig get_config_snapshot() const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return config_;
  }

  void set_config(const MCTSConfig &config) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (parallel_active_.load(std::memory_order_acquire))
      throw std::logic_error("cannot change MCTS config during active search");
    const bool changes_tree_domain =
        config_.use_determinization != config.use_determinization;
    const uint64_t current_generation =
        tree_generation_.load(std::memory_order_acquire);
    if (changes_tree_domain &&
        current_generation == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("MCTS tree generation is exhausted");
    config_ = config;
    if (changes_tree_domain) {
      nodes_.clear();
#if !CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
      node_aux_.clear();
      access_count_.clear();
#endif
      access_counter_ = 0;
      const uint64_t next_generation = current_generation + 1;
      tree_generation_.store(next_generation, std::memory_order_release);
      parallel_tree_.clear_and_set_generation(next_generation);
    }
  }

  // Clear the search tree
  void clear() {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (parallel_active_.load(std::memory_order_acquire))
      throw std::logic_error("cannot clear MCTS during active search");
    const uint64_t current_generation =
        tree_generation_.load(std::memory_order_acquire);
    if (current_generation == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("MCTS tree generation is exhausted");
    nodes_.clear();
#if !CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    node_aux_.clear();
    access_count_.clear();
#endif
    access_counter_ = 0;
    const uint64_t next_generation = current_generation + 1;
    tree_generation_.store(next_generation, std::memory_order_release);
    parallel_tree_.clear_and_set_generation(next_generation);
  }

  std::optional<MCTSNode> get_node_snapshot(uint64_t hash) const {
    reject_if_parallel_active("read legacy node");
    const auto iterator = nodes_.find(hash);
    if (iterator == nodes_.end())
      return std::nullopt;
    return node_value(iterator->second);
  }

  std::optional<mcts_parallel::MCTSNodeSnapshot64>
  get_parallel_node_snapshot(const TreeKey &key) const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return parallel_tree_.snapshot(key);
  }

  size_t parallel_tree_size() const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return parallel_tree_.size();
  }

  std::vector<mcts_parallel::MCTSNodeSnapshot64>
  get_parallel_tree_snapshot() const {
    std::lock_guard<std::mutex> lock(control_mutex_);
    return parallel_tree_.snapshot_all();
  }

  mcts_parallel::ConcurrentTree &
  prepare_parallel_tree(SearchGuard &guard, mcts_parallel::TreeBackend backend,
                        uint32_t shard_count, size_t capacity = MAX_TREE_SIZE) {
    if (shard_count == 0)
      throw std::invalid_argument("parallel tree shard count must be positive");
    if (capacity == 0 || capacity > MAX_TREE_SIZE)
      throw std::invalid_argument("parallel tree capacity is out of range");
    std::lock_guard<std::mutex> lock(control_mutex_);
    validate_guard(guard);
    if (guard.tree_prepared_)
      throw std::logic_error("parallel tree has already been prepared");

    const bool shard_layout_changed =
        backend == mcts_parallel::TreeBackend::Sharded &&
        parallel_tree_.backend() == backend &&
        parallel_tree_.shard_count() != shard_count;
    const bool capacity_changed = parallel_tree_.capacity() != capacity;
    if (parallel_tree_.backend() != backend || shard_layout_changed ||
        capacity_changed) {
      const uint64_t current_generation =
          tree_generation_.load(std::memory_order_acquire);
      if (current_generation == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("MCTS tree generation is exhausted");
      const uint64_t next_generation = current_generation + 1;
      // Allocate every shard before publishing either the replacement tree or
      // its generation. Allocation failure therefore leaves the old state
      // fully reusable.
      mcts_parallel::ConcurrentTree replacement(next_generation, backend,
                                                capacity, shard_count);
      parallel_tree_ = std::move(replacement);
      tree_generation_.store(next_generation, std::memory_order_release);
      guard.tree_generation_ = next_generation;
    }
    guard.tree_prepared_ = true;
    return parallel_tree_;
  }

  mcts_parallel::ConcurrentTree &parallel_tree(SearchGuard &guard) {
    validate_guard(guard);
    if (!guard.tree_prepared_)
      throw std::logic_error("parallel tree has not been prepared");
    return parallel_tree_;
  }

  const mcts_parallel::ConcurrentTree &
  parallel_tree(const SearchGuard &guard) const {
    validate_guard(guard);
    if (!guard.tree_prepared_)
      throw std::logic_error("parallel tree has not been prepared");
    return parallel_tree_;
  }

  // Get node by hash, returns nullptr if not found
  MCTSNode *get_node(uint64_t hash) {
    reject_if_parallel_active("access legacy node");
    auto it = nodes_.find(hash);
    if (it != nodes_.end()) {
      // Update access count for LRU
      touch_node(it);
      return &node_value(it->second);
    }
    return nullptr;
  }

  // Create or get node
  MCTSNode &get_or_create_node(uint64_t hash) {
    reject_if_parallel_active("create legacy node");
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    return get_or_create_record(hash).node;
#else
    access_count_[hash] = ++access_counter_;
    return nodes_[hash];
#endif
  }

  // Select action using PUCT formula
  // Now supports FPU (First Play Urgency) and forced playouts
  int select_action(const MCTSNode &node, bool is_root, int current_sim = 0) {
    reject_if_parallel_active("select from legacy node");
    float best_ucb = -1e9f;
    int best_action = -1;

    float sqrt_total = std::sqrt(static_cast<float>(node.total_visits) + EPS);

    // Compute FPU value based on config
    // Note: node.value[0] is always player 0's value, not necessarily the
    // current player's. Using a fixed FPU is safer since we don't track current
    // player per node.
    float fpu_init;
    if (config_.fpu < 0) {
      fpu_init = config_.fpu; // Use absolute negative value
    } else if (config_.fpu > 0) {
      // Parent-based reduction: use a conservative estimate
      fpu_init = -config_.fpu; // Penalize unvisited actions
    } else {
      fpu_init = 0.0f; // Neutral FPU for fpu=0
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
        int threshold = static_cast<int>(
            std::sqrt(config_.forced_playouts_k * p * current_sim));
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
  // Virtual loss is applied more gently to allow concentration on good moves.
  int select_action_with_virtual_loss(
      uint64_t hash, bool is_root,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise = nullptr,
      int current_sim = 0) {
    reject_if_parallel_active("select from legacy tree");
    auto it = nodes_.find(hash);
    if (it == nodes_.end())
      return -1;

    MCTSNode &node = node_value(it->second);
    float best_ucb = -1e9f;
    int best_action = -1;

    // Virtual loss weight - reduced from 1.0 to allow more concentration
    constexpr float VL_WEIGHT = 0.3f;

    // Include virtual loss in total visits (weighted)
    float total_vl = 0.0f;
    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      total_vl += node.virtual_loss[a] * VL_WEIGHT;
    }

    float sqrt_total =
        std::sqrt(static_cast<float>(node.total_visits) + total_vl + EPS);

    // Compute FPU value based on config
    // Note: node.value[0] is always player 0's value, not necessarily the
    // current player's. Using a fixed FPU is safer since we don't track current
    // player per node.
    float fpu_init;
    if (config_.fpu < 0) {
      fpu_init = config_.fpu; // Use absolute negative value
    } else if (config_.fpu > 0) {
      // Parent-based reduction: use a conservative estimate
      fpu_init = -config_.fpu; // Penalize unvisited actions
    } else {
      fpu_init = 0.0f; // Neutral FPU for fpu=0
    }

    for (size_t a = 0; a < MAX_ACTIONS; ++a) {
      if (!node.valid_actions[a])
        continue;

      // Forced playouts: if visit count is below threshold, force this action
      if (config_.forced_playouts && current_sim > 0) {
        float p = node.prior[a];
        int threshold = static_cast<int>(
            std::sqrt(config_.forced_playouts_k * p * current_sim));
        if (static_cast<int>(node.N[a]) < threshold) {
          return static_cast<int>(a);
        }
      }

      // Effective visit count includes weighted virtual loss
      float effective_n =
          static_cast<float>(node.N[a]) + node.virtual_loss[a] * VL_WEIGHT;

      // Q value with gentle virtual loss penalty
      float q;
      if (node.N[a] > 0) {
        // Blend actual Q with virtual loss (less pessimistic)
        float vl_penalty =
            node.virtual_loss[a] * VL_WEIGHT * 0.5f; // Reduced penalty
        q = (node.Q[a] * node.N[a] - vl_penalty) / (node.N[a] + vl_penalty);
      } else if (node.virtual_loss[a] > 0) {
        q = fpu_init - 0.2f; // Slight penalty, not full loss
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

  // Compatibility boundary for callers that still provide a dense mask.
  // Native orchestration uses the bitset overload below.
  int select_action_with_virtual_loss_for_world(
      uint64_t hash, const std::array<uint8_t, MAX_ACTIONS> &world_mask,
      bool is_root,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise = nullptr,
      int current_sim = 0) {
    return select_action_with_virtual_loss_for_world_bits(
        hash, mcts_action_mask::from_dense(world_mask), is_root,
        dirichlet_noise, current_sim);
  }

  int select_action_with_virtual_loss_for_world_bits(
      uint64_t hash, ActionMaskBits world_mask, bool is_root,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise = nullptr,
      int current_sim = 0) {
    reject_if_parallel_active("select from legacy tree");
    auto node_it = nodes_.find(hash);
    if (node_it == nodes_.end())
      return -1;

    MCTSNode &node = node_value(node_it->second);
    LegacyNodeAux &aux = node_aux(node_it);
    world_mask &= mcts_action_mask::ALL;
    float policy_sum = 0.0f;
    mcts_action_mask::for_each(world_mask, [&](size_t action) {
      auto &edge = aux.ensure_edge(action);
      ++edge.availability_count;
      node.valid_actions[action] = 1;
      const float base =
          aux.has_base_policy ? aux.base_policy[action] : node.prior[action];
      if (std::isfinite(base) && base > 0.0f)
        policy_sum += base;
    });

    float total_vl = 0.0f;
    constexpr float VL_WEIGHT = 0.3f;
    mcts_action_mask::for_each(aux.availability_union, [&](size_t action) {
      total_vl += node.virtual_loss[action] * VL_WEIGHT;
    });
    const float sqrt_total =
        std::sqrt(static_cast<float>(node.total_visits) + total_vl + EPS);

    float fpu_init = 0.0f;
    if (config_.fpu < 0.0f)
      fpu_init = config_.fpu;
    else if (config_.fpu > 0.0f)
      fpu_init = -config_.fpu;

    float best_ucb = -1e9f;
    int best_action = -1;
    const int candidate_count =
        static_cast<int>(mcts_action_mask::popcount(world_mask));
    if (candidate_count == 0)
      return -1;

    bool forced_choice = false;
    mcts_action_mask::for_each(world_mask, [&](size_t action) {
      if (forced_choice)
        return;
      const float base =
          aux.has_base_policy ? aux.base_policy[action] : node.prior[action];
      float p = (policy_sum > EPS && std::isfinite(base) && base > 0.0f)
                    ? base / policy_sum
                    : 1.0f / static_cast<float>(candidate_count);
      if (is_root && dirichlet_noise != nullptr) {
        p = (1.0f - config_.dirichlet_epsilon) * p +
            config_.dirichlet_epsilon * (*dirichlet_noise)[action];
      }

      if (config_.forced_playouts && current_sim > 0) {
        const int threshold = static_cast<int>(std::sqrt(
            config_.forced_playouts_k * p *
            static_cast<float>(aux.edge_for(action).availability_count)));
        if (static_cast<int>(node.N[action]) < threshold) {
          best_action = static_cast<int>(action);
          forced_choice = true;
          return;
        }
      }

      const float effective_n = static_cast<float>(node.N[action]) +
                                node.virtual_loss[action] * VL_WEIGHT;
      float q = fpu_init;
      if (node.N[action] > 0) {
        const float penalty = node.virtual_loss[action] * VL_WEIGHT * 0.5f;
        q = (node.Q[action] * node.N[action] - penalty) /
            (node.N[action] + penalty);
      } else if (node.virtual_loss[action] > 0) {
        q = fpu_init - 0.2f;
      }
      const float ucb =
          q + config_.cpuct * p * sqrt_total / (1.0f + effective_n);
      if (ucb > best_ucb) {
        best_ucb = ucb;
        best_action = static_cast<int>(action);
      }
    });
    return best_action;
  }

  // Add virtual loss to an action
  void add_virtual_loss(uint64_t hash, int action) {
    reject_if_parallel_active("mutate legacy virtual loss");
    auto it = nodes_.find(hash);
    if (it != nodes_.end() && action >= 0 &&
        action < static_cast<int>(MAX_ACTIONS)) {
      node_value(it->second).virtual_loss[action]++;
    }
  }

  // Remove virtual loss from an action
  void remove_virtual_loss(uint64_t hash, int action) {
    reject_if_parallel_active("mutate legacy virtual loss");
    auto it = nodes_.find(hash);
    if (it != nodes_.end() && action >= 0 &&
        action < static_cast<int>(MAX_ACTIONS)) {
      // Applying a stale result twice or an explicit extra removal must not
      // create a negative visit penalty and poison the subsequent sqrt/PUCT.
      if (node_value(it->second).virtual_loss[action] > 0)
        node_value(it->second).virtual_loss[action]--;
    }
  }

  // Clear all virtual losses (for safety after batch)
  void clear_virtual_losses() {
    reject_if_parallel_active("clear legacy virtual loss");
    for (auto &kv : nodes_) {
      for (size_t a = 0; a < MAX_ACTIONS; ++a) {
        node_value(kv.second).virtual_loss[a] = 0;
      }
    }
  }

  void abort_path_virtual_losses(const std::vector<PathEntry> &path) {
    for (auto it = path.rbegin(); it != path.rend(); ++it)
      remove_virtual_loss(it->hash, it->action);
  }

  // Generate Dirichlet noise (public version for batch MCTS)
  std::array<float, MAX_ACTIONS>
  generate_dirichlet_noise_for_node(uint64_t hash) {
    reject_if_parallel_active("use legacy RNG");
    std::array<float, MAX_ACTIONS> noise = {0};
    auto it = nodes_.find(hash);
    if (it != nodes_.end()) {
      generate_dirichlet_noise(node_value(it->second), noise);
    }
    return noise;
  }

  // Backpropagate value through the search path
  void backpropagate(const std::vector<PathEntry> &path,
                     const std::array<float, NUM_PLAYERS> &value) {
    reject_if_parallel_active("backpropagate legacy path");
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      MCTSNode *node = get_node(it->hash);
      if (!node)
        continue;

      int a = it->action;
      if (a < 0 || a >= static_cast<int>(MAX_ACTIONS))
        continue;
      if (it->player < 0 || it->player >= static_cast<int>(NUM_PLAYERS))
        continue;

      // Roll value to be relative to the player who made the move
      float v = value[it->player];

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
    reject_if_parallel_active("expand legacy node");
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    auto &record = get_or_create_record(hash);
    MCTSNode &node = record.node;
#else
    MCTSNode &node = get_or_create_node(hash);
#endif
    node.valid_actions = valid_actions;
    node.value = value;
    node.is_expanded = true;

#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    LegacyNodeAux &aux = record.aux;
#else
    LegacyNodeAux &aux = node_aux_[hash];
#endif
    aux.base_policy = policy;
    aux.has_base_policy = true;
    const ActionMaskBits valid_bits =
        mcts_action_mask::from_dense(valid_actions);
    mcts_action_mask::for_each(valid_bits,
                               [&](size_t action) { aux.ensure_edge(action); });

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
    reject_if_parallel_active("mark legacy node terminal");
    MCTSNode &node = get_or_create_node(hash);
    node.is_terminal = true;
    node.is_expanded = true;
    node.value = terminal_value;
  }

  // Get action probabilities after search
  std::array<float, MAX_ACTIONS> get_action_probs(uint64_t root_hash,
                                                  float temperature) const {
    reject_if_parallel_active("read legacy probabilities");
    std::array<float, MAX_ACTIONS> probs = {0};

    auto it = nodes_.find(root_hash);
    if (it == nodes_.end()) {
      return probs;
    }

    const MCTSNode &node = node_value(it->second);

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
          probs[a] =
              std::pow(static_cast<float>(node.N[a]), 1.0f / temperature);
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
    reject_if_parallel_active("prune legacy tree");
    if (nodes_.size() < MAX_TREE_SIZE)
      return;

    // Find threshold access count to keep PRUNE_THRESHOLD nodes
    std::vector<uint64_t> access_counts;
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
    access_counts.reserve(nodes_.size());
    for (const auto &kv : nodes_)
      access_counts.push_back(kv.second.last_access);
#else
    access_counts.reserve(access_count_.size());
    for (const auto &kv : access_count_) {
      access_counts.push_back(kv.second);
    }
#endif
    std::sort(access_counts.begin(), access_counts.end(),
              std::greater<uint64_t>());

    uint64_t threshold = 0;
    if (access_counts.size() > PRUNE_THRESHOLD) {
      threshold = access_counts[PRUNE_THRESHOLD];
    }

    // Remove nodes below threshold
    std::vector<uint64_t> to_remove;
    for (const auto &kv : nodes_) {
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
      if (kv.second.last_access < threshold) {
#else
      auto it = access_count_.find(kv.first);
      if (it == access_count_.end() || it->second < threshold) {
#endif
        to_remove.push_back(kv.first);
      }
    }

    for (uint64_t hash : to_remove) {
      nodes_.erase(hash);
#if !CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
      node_aux_.erase(hash);
      access_count_.erase(hash);
#endif
    }

    // Reset access counter if needed
    if (access_counter_ > 1000000) {
      access_counter_ = 0;
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
      for (auto &kv : nodes_)
        kv.second.last_access = 0;
#else
      for (auto &kv : access_count_) {
        kv.second = 0;
      }
#endif
    }
  }

  // Update node statistics after backpropagation
  void update_stats(uint64_t hash, int action, float value) {
    reject_if_parallel_active("update legacy statistics");
    auto it = nodes_.find(hash);
    if (it == nodes_.end())
      return;

    MCTSNode &node = node_value(it->second);
    if (action < 0 || action >= static_cast<int>(MAX_ACTIONS))
      return;

    // Incremental mean update
    node.N[action]++;
    node.total_visits++;
    node.Q[action] +=
        (value - node.Q[action]) / static_cast<float>(node.N[action]);
  }

  BatchSimulationRequest prepare_batch_simulations(
      const Game &root_game, uint8_t observer, int batch_size,
      int num_determinizations,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise);

  // Legacy overload for backward compatibility (deprecated)
  BatchSimulationRequest prepare_batch_simulations(
      const Game &root_game, uint8_t observer, int batch_size,
      int num_determinizations,
      const std::array<float, MAX_ACTIONS> *dirichlet_noise,
      IActionEncoder &encoder);

  /** Apply NN evaluation results to the tree. */
  void apply_batch_results(
      const BatchSimulationRequest &request,
      const std::vector<std::array<float, MAX_ACTIONS>> &policies,
      const std::vector<std::array<float, NUM_PLAYERS>> &values) {
    reject_if_parallel_active("apply legacy batch results");
    if (request.tree_generation != tree_generation_snapshot())
      throw std::logic_error("batch request belongs to a stale tree");
    // Validate the complete external C++ payload before mutating any node or
    // consuming virtual losses. Python performs equivalent validation in its
    // reconstructed-request adapter, but native callers use this method
    // directly.
    size_t expected_results = 0;
    for (const auto &leaf : request.leaves) {
      if (leaf.num_worlds <= 0)
        throw std::invalid_argument("leaf world count must be positive");
      const size_t worlds = static_cast<size_t>(leaf.num_worlds);
      if (leaf.valid_actions.size() < worlds)
        throw std::invalid_argument("leaf valid masks are incomplete");
      expected_results += worlds;
    }
    if (policies.size() < expected_results || values.size() < expected_results)
      throw std::invalid_argument(
          "batch results do not match requested worlds");

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

      // expand_node keeps the unmasked 48-action base policy in the serial
      // information-set sidecar and exposes a masked prior in the legacy DTO.
      expand_node(leaf.hash, avg_policy, avg_value, combined_valid);

      // Remove virtual losses and backpropagate
      backpropagate_with_virtual_loss_removal(leaf.path, avg_value);
    }

    // Handle terminal paths
    for (const auto &[path, value] : request.terminals) {
      backpropagate_with_virtual_loss_removal(path, value);
    }
  }

  /** Backpropagate value and remove virtual losses along the path. */
  void backpropagate_with_virtual_loss_removal(
      const std::vector<PathEntry> &path,
      const std::array<float, NUM_PLAYERS> &value) {
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
      // Remove virtual loss
      remove_virtual_loss(it->hash, it->action);

      if (it->player < 0 || it->player >= static_cast<int>(NUM_PLAYERS))
        continue;
      // Get value relative to the player who made the move
      float v = value[it->player];

      // Update statistics
      update_stats(it->hash, it->action, v);
    }
  }

  // Get statistics
  size_t tree_size() const {
    reject_if_parallel_active("read legacy tree size");
    return nodes_.size();
  }

  // Legacy C++ read facade. New code should use get_config_snapshot(). The
  // mutable overload was intentionally removed so an active search cannot be
  // modified through a retained reference.
  const MCTSConfig &config() const { return config_; }

private:
  struct LegacyAvailabilityEdge {
    uint64_t availability_count = 0;
    uint8_t action = 0;
  };

  struct LegacyNodeAux {
    std::array<float, MAX_ACTIONS> base_policy{};
    std::vector<LegacyAvailabilityEdge> edges;
    ActionMaskBits availability_union = 0;
    bool has_base_policy = false;

    LegacyAvailabilityEdge &ensure_edge(size_t action) {
      auto iterator = std::lower_bound(
          edges.begin(), edges.end(), action,
          [](const LegacyAvailabilityEdge &edge, size_t requested) {
            return static_cast<size_t>(edge.action) < requested;
          });
      if (iterator == edges.end() || iterator->action != action) {
        LegacyAvailabilityEdge edge;
        edge.action = static_cast<uint8_t>(action);
        iterator = edges.insert(iterator, edge);
      }
      availability_union |= mcts_action_mask::bit(action);
      return *iterator;
    }

    const LegacyAvailabilityEdge &edge_for(size_t action) const {
      const auto iterator = std::lower_bound(
          edges.begin(), edges.end(), action,
          [](const LegacyAvailabilityEdge &edge, size_t requested) {
            return static_cast<size_t>(edge.action) < requested;
          });
      if (iterator == edges.end() || iterator->action != action)
        throw std::logic_error("legacy compact edge is missing");
      return *iterator;
    }
  };

  // unordered_map preserves references/pointers to record members on rehash.
  // The public mutable node is this member itself, never a cached snapshot.
  struct LegacyTreeRecord {
    MCTSNode node;
    LegacyNodeAux aux;
    uint64_t last_access = 0;
  };
#if CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
  using LegacyNodeMap = std::unordered_map<uint64_t, LegacyTreeRecord>;
  static MCTSNode &node_value(LegacyTreeRecord &record) { return record.node; }
  static const MCTSNode &node_value(const LegacyTreeRecord &record) {
    return record.node;
  }
  LegacyNodeAux &node_aux(LegacyNodeMap::iterator it) { return it->second.aux; }
  void touch_node(LegacyNodeMap::iterator it) {
    it->second.last_access = ++access_counter_;
  }
  LegacyTreeRecord &get_or_create_record(uint64_t hash) {
    const uint64_t access = ++access_counter_;
    auto &record = nodes_[hash];
    record.last_access = access;
    return record;
  }
#else
  using LegacyNodeMap = std::unordered_map<uint64_t, MCTSNode>;
  static MCTSNode &node_value(MCTSNode &node) { return node; }
  static const MCTSNode &node_value(const MCTSNode &node) { return node; }
  LegacyNodeAux &node_aux(LegacyNodeMap::iterator it) {
    return node_aux_[it->first];
  }
  void touch_node(LegacyNodeMap::iterator it) {
    access_count_[it->first] = ++access_counter_;
  }
#endif
#ifdef CSPLENDOR_MCTS_TESTING
  friend struct MCTSLegacyRecordTestAccess;
#endif

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

  void reject_if_parallel_active(const char *operation) const {
    if (parallel_active_.load(std::memory_order_acquire))
      throw std::logic_error(std::string("cannot ") + operation +
                             " during active parallel search");
  }

  void validate_guard(const SearchGuard &guard) const {
    if (guard.owner_ != this || !guard.active() ||
        !parallel_active_.load(std::memory_order_acquire) ||
        guard.search_id_ != active_search_id_.load(std::memory_order_acquire) ||
        guard.tree_generation_ != tree_generation_snapshot())
      throw std::logic_error("invalid or stale MCTS search guard");
  }

  void finish_parallel_search(uint64_t search_id) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!parallel_active_.load(std::memory_order_acquire) ||
        active_search_id_.load(std::memory_order_acquire) != search_id)
      throw std::logic_error("MCTS search guard is not active");
    parallel_active_.store(false, std::memory_order_release);
    active_search_id_.store(0, std::memory_order_release);
  }

  void finish_parallel_search_noexcept(uint64_t search_id) noexcept {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (parallel_active_.load(std::memory_order_acquire) &&
        active_search_id_.load(std::memory_order_acquire) == search_id) {
      parallel_active_.store(false, std::memory_order_release);
      active_search_id_.store(0, std::memory_order_release);
    }
  }

  MCTSConfig config_;
  mutable std::mutex control_mutex_;
  std::atomic<bool> parallel_active_{false};
  std::atomic<uint64_t> tree_generation_{0};
  uint64_t next_search_nonce_ = 0;
  uint64_t next_search_id_ = 0;
  uint64_t default_parallel_seed_ = 0;
  std::atomic<uint64_t> active_search_id_{0};
  mcts_parallel::ConcurrentTree parallel_tree_;
  LegacyNodeMap nodes_;
#if !CSPLENDOR_MCTS_LEGACY_TREE_RECORDS
  std::unordered_map<uint64_t, LegacyNodeAux> node_aux_;
  std::unordered_map<uint64_t, uint64_t> access_count_;
#endif
  uint64_t access_counter_ = 0;
  std::mt19937 rng_;
};

inline MCTS::SearchGuard::SearchGuard(SearchGuard &&other) noexcept
    : owner_(other.owner_), config_snapshot_(other.config_snapshot_),
      tree_generation_(other.tree_generation_),
      resolved_master_seed_(other.resolved_master_seed_),
      search_nonce_(other.search_nonce_), search_id_(other.search_id_),
      tree_prepared_(other.tree_prepared_) {
  other.owner_ = nullptr;
  other.search_id_ = 0;
  other.tree_prepared_ = false;
}

inline MCTS::SearchGuard &
MCTS::SearchGuard::operator=(SearchGuard &&other) noexcept {
  if (this != &other) {
    finish_noexcept();
    owner_ = other.owner_;
    config_snapshot_ = other.config_snapshot_;
    tree_generation_ = other.tree_generation_;
    resolved_master_seed_ = other.resolved_master_seed_;
    search_nonce_ = other.search_nonce_;
    search_id_ = other.search_id_;
    tree_prepared_ = other.tree_prepared_;
    other.owner_ = nullptr;
    other.search_id_ = 0;
    other.tree_prepared_ = false;
  }
  return *this;
}

inline MCTS::SearchGuard::~SearchGuard() noexcept { finish_noexcept(); }

inline void MCTS::SearchGuard::finish() {
  if (!owner_)
    throw std::logic_error("MCTS search guard has already been released");
  MCTS *owner = owner_;
  const uint64_t id = search_id_;
  if (tree_prepared_)
    owner->parallel_tree(*this).validate_quiescent_for_search();
  owner->finish_parallel_search(id);
  owner_ = nullptr;
  search_id_ = 0;
  tree_prepared_ = false;
}

inline void MCTS::SearchGuard::finish_noexcept() noexcept {
  if (!owner_)
    return;
  owner_->finish_parallel_search_noexcept(search_id_);
  owner_ = nullptr;
  search_id_ = 0;
}

#endif // CSPLENDOR_MCTS_TREE_H
