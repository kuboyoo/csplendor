#ifndef CSPLENDOR_MCTS_V3_H
#define CSPLENDOR_MCTS_V3_H

// Multi-game PUCT search over ActionEncoderV4 action ids (all payment and
// token-return variants are distinct edges).
//
// Design
// - One V3SearchSession drives many independent games in lockstep. collect()
//   descends every active tree until it holds up to `leaf_batch_size` fresh
//   leaves per game, encodes their network inputs, and returns one batch.
//   apply() installs the priors/values and backs them up. No Python objects
//   are touched inside those two calls, so bindings release the GIL.
// - Hidden information is determinized from the root observer once per
//   simulation (Game::shuffled_clone). The tree is an information-set tree of
//   the observer: nodes are reached by public action sequences.
// - A public reveal (board refill after a purchase or visible reservation) and
//   the observer's own deck reservation create a chance node whose edges are
//   keyed by the revealed card. Each simulation follows the outcome its
//   sampled world produced, so a chance node's value is the empirical
//   expectation over uniform reveals. Optionally (chance_enumeration_depth)
//   the chance nodes closest to the root enumerate every card the observer
//   cannot see in that tier, evaluate all outcomes in one batch, and start
//   from the exact (optionally risk-weighted) expectation; later simulations
//   can back up control-variate corrected values above such nodes.
// - Optional two-level selection: PUCT over "semantic groups" (the decision
//   before payment/return details), then PUCT inside the chosen group.
// - Virtual loss keeps the leaves of one batch distinct. A node selected twice
//   while still pending ends that game's collection for the round.
//
// Not implemented here on purpose: mate search, rollouts, tree reuse across
// moves, opponent re-determinization inside a simulation, stall guards. The
// Python driver adds temperature sampling and any of those on top.

#include "action.h"
#include "action_encoder_v4.h"
#include "game.h"
#include "state_encoder.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <exception>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace csplendor::v3search {

struct Config {
  int num_simulations = 800;
  int leaf_batch_size = 32;
  float c_puct = 1.5f;
  float fpu_reduction = 0.2f;
  float virtual_loss = 1.0f;
  bool determinization = true;
  bool semantic_groups = false;
  float intra_group_cpuct = 1.0f;
  int max_depth = 300;
  float draw_value = 0.0f;
  bool public_card_features = true; // +117 inputs
  bool physical_seat_feature = true; // +1 input
  // 1 while the player to move must return a token after a deck reservation
  // (Board::waiting_return), else 0. +1 input.
  bool return_phase_feature = true;
  float dirichlet_alpha = 0.3f;
  float dirichlet_epsilon = 0.25f;
  float unseen_action_prior = 1e-3f; // legal in this world, absent at expansion
  // Root rollouts after the tree budget: up to `rollout_candidates` distinct
  // decisions (best first) are continued greedily by the Policy in
  // `rollout_samples` shared sampled worlds for `rollout_horizon` plies; a
  // cutoff uses the Value of the acting side. 0 samples disables the phase.
  int rollout_samples = 0;
  int rollout_candidates = 6;
  int rollout_horizon = 48;
  int rollout_min_visits = 2;
  // Reshuffle the remaining (unknown to both players) deck order whenever the
  // side to move changes inside a simulation, as the Python search does.
  bool opponent_redeterminization = false;
  // Scale c_puct by the visit-weighted variance of child Q at the node.
  bool dynamic_cpuct = false;
  float dynamic_cpuct_variance_floor = 0.25f;
  float dynamic_cpuct_min_scale = 0.5f;
  float dynamic_cpuct_max_scale = 1.5f;
  // Reveal lookahead. Chance nodes at most `chance_enumeration_depth` plies
  // below the root (1 = reveals caused by root actions) reveal every card the
  // observer cannot see in that tier in its own child, evaluate all children
  // in one batch (one simulation of the budget) and start from the exact
  // expectation. 0 keeps purely sampled chance nodes.
  int chance_enumeration_depth = 0;
  // Mix of the acting side's worst reveal into that initial value:
  // 0 = expectation, 1 = worst case.
  float chance_risk_weight = 0.0f;
  // Back up later simulations through an enumerated chance node as
  // v - v0(reveal) + mean(v0) above the node, removing the variance of which
  // reveal the sampled world happened to produce.
  bool chance_control_variate = false;
  // Worker threads used by Session::collect/apply. Games are independent, so
  // each game's leaves are produced by one thread and concatenated in slot
  // order: rows, RNG streams and trees are identical for every thread count.
  int num_threads = 1;

  int state_dim() const {
    return TOTAL_FEATURES +
           (public_card_features ? PUBLIC_CARD_FEATURE_SIZE : 0) +
           (physical_seat_feature ? 1 : 0) + (return_phase_feature ? 1 : 0);
  }
};

// Decision before payment/return details over V4 ids: 139 groups. Groups
// 0..132 keep the layout of dlsplendor.search.semantic_actions (V3): take
// different 0..9, take same 10..14, reserve visible 15..26, reserve deck
// 27..29, purchase 30 + card id, noble 120 + id, pass 132. The post-deck-
// reservation returns are groups 133..138 (white, blue, green, red, black,
// gold).
inline constexpr int SEMANTIC_GROUP_COUNT = 139;

// The same groups over V3 ids (133 groups; kept for V3 data).
inline int semantic_group_id_v3(int action_id) {
  using E = ActionEncoderV3;
  if (action_id < E::OFFSET_TAKE_SAME)
    return (action_id - E::OFFSET_TAKE_DIFFERENT) / E::TAKE_DIFF_RETURN_PATTERNS;
  if (action_id < E::OFFSET_RESERVE_VISIBLE)
    return 10 + (action_id - E::OFFSET_TAKE_SAME) / E::TAKE_SAME_RETURN_PATTERNS;
  if (action_id < E::OFFSET_RESERVE_DECK)
    return 15 + (action_id - E::OFFSET_RESERVE_VISIBLE) / E::RESERVE_RETURN_PATTERNS;
  if (action_id < E::OFFSET_PURCHASE)
    return 27 + (action_id - E::OFFSET_RESERVE_DECK) / E::RESERVE_RETURN_PATTERNS;
  if (action_id < E::OFFSET_VISIT_NOBLE)
    return 30 + E::find_card_id(action_id - E::OFFSET_PURCHASE);
  if (action_id < E::OFFSET_PASS)
    return 120 + (action_id - E::OFFSET_VISIT_NOBLE);
  return 132;
}
inline int semantic_group_id(int action_id) {
  using E = ActionEncoderV4;
  using V3 = ActionEncoderV3;
  if (action_id < E::OFFSET_TAKE_SAME)
    return (action_id - E::OFFSET_TAKE_DIFFERENT) / V3::TAKE_DIFF_RETURN_PATTERNS;
  if (action_id < E::OFFSET_RESERVE_VISIBLE)
    return 10 + (action_id - E::OFFSET_TAKE_SAME) / V3::TAKE_SAME_RETURN_PATTERNS;
  if (action_id < E::OFFSET_RESERVE_DECK)
    return 15 + (action_id - E::OFFSET_RESERVE_VISIBLE) / V3::RESERVE_RETURN_PATTERNS;
  if (action_id < E::OFFSET_PURCHASE)
    return 27 + (action_id - E::OFFSET_RESERVE_DECK);
  if (action_id < E::OFFSET_VISIT_NOBLE)
    return 30 + V3::find_card_id(action_id - E::OFFSET_PURCHASE);
  if (action_id < E::OFFSET_RETURN_GEM)
    return 120 + (action_id - E::OFFSET_VISIT_NOBLE);
  if (action_id < E::OFFSET_PASS)
    return 133 + (action_id - E::OFFSET_RETURN_GEM);
  return 132;
}

enum class NodeKind : uint8_t { Decision = 0, Chance = 1, Terminal = 2 };

struct Edge {
  int32_t key = -1; // V3 action id, or revealed card id below a chance node
  float prior = 0.0f;
  float clean_prior = 0.0f; // prior before root noise (root edges only)
  int32_t child = -1;
  float chance_value = 0.0f; // enumerated reveal value, chance actor's view
};

struct Node {
  NodeKind kind = NodeKind::Decision;
  uint8_t player = 0; // perspective of value_sum
  bool expanded = false;
  bool pending = false;
  bool enumerated = false; // chance node whose reveals were all evaluated
  uint32_t visits = 0;
  int32_t virtual_visits = 0;
  double value_sum = 0.0;
  float terminal_value = 0.0f; // valid when kind == Terminal
  float chance_mean = 0.0f;    // mean enumerated value, chance actor's view
  std::vector<Edge> edges; // sorted by key

  double q() const { return visits ? value_sum / visits : 0.0; }
  const Edge *find(int32_t key) const {
    auto it = std::lower_bound(
        edges.begin(), edges.end(), key,
        [](const Edge &edge, int32_t value) { return edge.key < value; });
    return (it != edges.end() && it->key == key) ? &*it : nullptr;
  }
  Edge *find(int32_t key) {
    auto it = std::lower_bound(
        edges.begin(), edges.end(), key,
        [](const Edge &edge, int32_t value) { return edge.key < value; });
    return (it != edges.end() && it->key == key) ? &*it : nullptr;
  }
  // Same result as find(key). `cursor` keeps the lower bound of the previous
  // lookup, so a mostly ascending key sequence (legal actions are generated
  // in near id order) resolves by a short forward scan instead of a fresh
  // binary search; a backward step falls back to the binary search.
  const Edge *find_from(int32_t key, size_t &cursor) const {
    size_t index = cursor;
    if (index > 0 && edges[index - 1].key >= key) {
      index = static_cast<size_t>(
          std::lower_bound(edges.begin(), edges.begin() + index, key,
                           [](const Edge &edge, int32_t value) {
                             return edge.key < value;
                           }) -
          edges.begin());
    } else {
      while (index < edges.size() && edges[index].key < key)
        ++index;
    }
    cursor = index;
    return (index < edges.size() && edges[index].key == key) ? &edges[index]
                                                              : nullptr;
  }
  // Position of `key` in edges, or edges.size() when absent.
  size_t position(int32_t key) const {
    const Edge *edge = find(key);
    return edge == nullptr ? edges.size()
                           : static_cast<size_t>(edge - edges.data());
  }
  Edge &insert(int32_t key, float prior) {
    auto it = std::lower_bound(
        edges.begin(), edges.end(), key,
        [](const Edge &edge, int32_t value) { return edge.key < value; });
    Edge edge;
    edge.key = key;
    edge.prior = prior;
    return *edges.insert(it, edge);
  }
};

struct LegalAction {
  int32_t id;
  Action action;
};

struct PendingLeaf {
  int slot;
  int32_t node;              // tree node, or rollout world index when rollout
  std::vector<int32_t> path; // empty for rollout rows
  uint32_t generation;
  bool rollout = false;
  int32_t chance = -1; // enumerated child of this chance node (no backup)
  // Control-variate corrections applied above the listed path nodes when
  // this leaf's value is backed up (player-0 perspective).
  std::vector<std::pair<int32_t, float>> corrections;
};

struct Batch {
  int state_dim = 0;
  std::vector<float> features;      // rows * state_dim
  std::vector<int32_t> legal_ids;   // concatenated per row
  std::vector<int32_t> offsets;     // rows + 1
  std::vector<int32_t> slots;       // rows
  size_t rows() const { return slots.size(); }
};

struct Stats {
  uint64_t simulations = 0;
  uint64_t leaves = 0;
  uint64_t terminals = 0;
  uint64_t depth_limits = 0;
  uint64_t collisions = 0;
  uint64_t nodes = 0;
  uint64_t rollout_rows = 0;
  uint64_t rollout_games = 0;
  uint64_t chance_enumerations = 0;
  uint64_t chance_rows = 0;

  Stats &operator+=(const Stats &other) {
    simulations += other.simulations;
    leaves += other.leaves;
    terminals += other.terminals;
    depth_limits += other.depth_limits;
    collisions += other.collisions;
    nodes += other.nodes;
    rollout_rows += other.rollout_rows;
    rollout_games += other.rollout_games;
    chance_enumerations += other.chance_enumerations;
    chance_rows += other.chance_rows;
    return *this;
  }
};

class GameSearch {
public:
  GameSearch(const Config &config, const Game &root, int observer, uint64_t seed,
             bool root_noise, int num_simulations)
      : config_(config), root_(root.clone_light()), observer_(observer),
        rng_(seed), root_noise_(root_noise),
        budget_(num_simulations > 0 ? num_simulations : config.num_simulations) {
    if (observer < 0 || observer >= Board::NUM_PLAYERS)
      throw std::invalid_argument("observer must identify a player");
    if (root_.is_game_over())
      throw std::invalid_argument("root position is already terminal");
    // A forced-pass root is legal: its only V4 action is PASS (3120).
    nodes_.reserve(4096);
    root_index_ = new_node(NodeKind::Decision,
                           static_cast<uint8_t>(root_.current_player()));
  }

  bool tree_done() const { return completed_ >= budget_ && pending_count_ == 0; }
  bool done() const {
    if (!tree_done())
      return false;
    if (config_.rollout_samples <= 0)
      return true;
    return rollout_phase_ == RolloutPhase::Finished;
  }
  bool rollout_available() const {
    return rollout_phase_ == RolloutPhase::Finished && !rollout_candidates_.empty();
  }
  const std::vector<int32_t> &rollout_candidates() const { return rollout_candidates_; }
  int rollout_sample_count() const { return rollout_samples_; }
  // Row-major [candidate][sample] returns from the root player's perspective.
  const std::vector<float> &rollout_returns() const { return rollout_returns_; }
  const std::vector<uint8_t> &rollout_terminal() const { return rollout_terminal_; }
  int completed() const { return completed_; }
  int budget() const { return budget_; }
  uint32_t generation() const { return generation_; }
  const Node &root() const { return nodes_[root_index_]; }
  const Node &node(int32_t index) const { return nodes_[index]; }
  size_t node_count() const { return nodes_.size(); }

  // Run selections until `max_leaves` fresh leaves are pending or the budget
  // is exhausted. Terminal and depth-limited simulations back up immediately.
  void collect(int max_leaves, Batch &batch, std::vector<PendingLeaf> &pending,
               Stats &stats) {
    if (tree_done()) {
      if (config_.rollout_samples > 0) {
        if (rollout_phase_ == RolloutPhase::NotStarted)
          start_rollouts(stats);
        if (rollout_phase_ == RolloutPhase::Running)
          collect_rollouts(batch, pending, stats);
      }
      return;
    }
    int produced = 0;
    while (produced < max_leaves && completed_ + pending_count_ < budget_) {
      Game world = config_.determinization
                       ? root_.shuffled_clone(static_cast<uint8_t>(observer_),
                                              rng_())
                       : root_.clone_light();
      std::vector<int32_t> &path = path_;
      path.clear();
      path.push_back(root_index_);
      int32_t current = root_index_;
      int depth = 0;
      bool finished = false;
      corrections_.clear();
      int last_redetermined = world.current_player();
      while (true) {
        Node &node = nodes_[current];
        if (config_.opponent_redeterminization && node.kind == NodeKind::Decision &&
            node.expanded && world.current_player() != last_redetermined) {
          reshuffle_decks(world);
          last_redetermined = world.current_player();
        }
        if (node.kind == NodeKind::Terminal) {
          backpropagate(path, node.terminal_value, node.player);
          ++completed_;
          ++stats.terminals;
          ++stats.simulations;
          finished = true;
          break;
        }
        if (!node.expanded) {
          if (node.pending) {
            ++stats.collisions;
            return; // this game waits for the outstanding evaluation
          }
          // Fresh leaf: encode from this world, then hold it with virtual loss.
          encode_leaf(world, batch);
          batch.slots.push_back(slot_);
          for (int32_t index : path)
            ++nodes_[index].virtual_visits;
          node.pending = true;
          ++pending_count_;
          PendingLeaf leaf{slot_, current, path, generation_, false, -1, {}};
          leaf.corrections = corrections_;
          pending.push_back(std::move(leaf));
          ++produced;
          ++stats.leaves;
          finished = true;
          break;
        }
        if (depth >= config_.max_depth) {
          backpropagate(path, config_.draw_value, node.player);
          ++completed_;
          ++stats.depth_limits;
          ++stats.simulations;
          finished = true;
          break;
        }
        // Expanded decision node: choose among the actions legal in this world.
        legal_.clear();
        auto sink = [this](int id, const Action &action) {
          legal_.push_back(LegalAction{id, action});
        };
        ActionEncoderV4::for_each_legal_with_id(world, sink);
        if (legal_.empty()) {
          backpropagate(path, config_.draw_value, node.player);
          ++completed_;
          ++stats.simulations;
          finished = true;
          break;
        }
        size_t choice = legal_.size();
        if (current == root_index_ && !floors_.empty())
          choice = select_floor(node, legal_);
        if (choice == legal_.size())
          choice = config_.semantic_groups ? select_grouped(node, legal_)
                                           : select_flat(node, legal_);
        const int32_t chosen_id = legal_[choice].id;
        const Action chosen_action = legal_[choice].action;
        // Edge positions stay valid until this node's edge list is modified;
        // new_node() may move nodes_, but each node keeps its edges buffer.
        size_t chosen_edge = node.position(chosen_id);
        if (chosen_edge == node.edges.size()) {
          Edge &inserted = node.insert(chosen_id, config_.unseen_action_prior);
          chosen_edge = static_cast<size_t>(&inserted - node.edges.data());
        }
        const uint8_t actor = static_cast<uint8_t>(world.current_player());
        // Reveal lookahead needs the pre-action world; clone it only when the
        // chance node below this edge is still to be enumerated.
        const bool may_enumerate =
            depth < config_.chance_enumeration_depth &&
            reveals_to_observer(chosen_action, actor) &&
            chance_needs_enumeration(nodes_[current].edges[chosen_edge].child);
        if (may_enumerate)
          before_ = world.clone_light();
        const int32_t revealed = apply_with_reveal(world, chosen_action, actor);
        ++depth;
        const uint8_t mover = static_cast<uint8_t>(world.current_player());
        int32_t next;
        if (revealed != NO_REVEAL) {
          int32_t chance_index = nodes_[current].edges[chosen_edge].child;
          if (chance_index < 0) {
            chance_index = new_node(NodeKind::Chance, actor);
            nodes_[chance_index].expanded = true;
            nodes_[current].edges[chosen_edge].child = chance_index;
          }
          path.push_back(chance_index);
          if (may_enumerate) {
            Node &chance = nodes_[chance_index];
            if (chance.pending) {
              ++stats.collisions;
              return; // enumeration of this node is still being evaluated
            }
            if (enumerate_chance(chance_index, before_, chosen_action, actor,
                                 path, batch, pending, stats)) {
              ++produced;
              finished = true;
              break;
            }
          }
          if (config_.chance_control_variate && nodes_[chance_index].enumerated) {
            const Edge *outcome = nodes_[chance_index].find(revealed);
            if (outcome != nullptr && outcome->child >= 0) {
              // v - v0(reveal) + mean(v0), expressed for player 0.
              const float delta = nodes_[chance_index].chance_mean - outcome->chance_value;
              corrections_.emplace_back(chance_index, actor == 0 ? delta : -delta);
            }
          }
          size_t outcome_edge = nodes_[chance_index].position(revealed);
          if (outcome_edge == nodes_[chance_index].edges.size()) {
            Edge &inserted = nodes_[chance_index].insert(revealed, 0.0f);
            outcome_edge =
                static_cast<size_t>(&inserted - nodes_[chance_index].edges.data());
          }
          next = nodes_[chance_index].edges[outcome_edge].child;
          if (next < 0) {
            next = new_node(NodeKind::Decision, mover);
            nodes_[chance_index].edges[outcome_edge].child = next;
          }
        } else {
          next = nodes_[current].edges[chosen_edge].child;
          if (next < 0) {
            next = new_node(NodeKind::Decision, mover);
            nodes_[current].edges[chosen_edge].child = next;
          }
        }
        path.push_back(next);
        current = next;
        if (world.is_game_over()) {
          Node &leaf = nodes_[current];
          leaf.kind = NodeKind::Terminal;
          leaf.expanded = true;
          leaf.terminal_value = terminal_value(world, leaf.player);
          // loop continues to the Terminal branch above
        }
      }
      if (!finished)
        break;
    }
  }

  void apply(const PendingLeaf &leaf, const int32_t *legal_ids, size_t count,
             const float *priors, float value) {
    if (leaf.generation != generation_) {
      // Stale result after a reset; drop it silently.
      return;
    }
    if (leaf.rollout) {
      apply_rollout(leaf, legal_ids, count, priors, value);
      return;
    }
    Node &node = nodes_[leaf.node];
    if (!node.pending)
      throw std::logic_error("evaluation applied to a node that is not pending");
    if (leaf.chance >= 0) {
      install_priors(node, legal_ids, count, priors);
      node.expanded = true;
      node.pending = false;
      node.visits = 1;
      node.value_sum = value;
      Node &chance = nodes_[leaf.chance];
      Edge *edge = nullptr;
      for (Edge &candidate : chance.edges)
        if (candidate.child == leaf.node) {
          edge = &candidate;
          break;
        }
      if (edge == nullptr)
        throw std::logic_error("enumerated reveal lost its chance edge");
      edge->chance_value = node.player == chance.player ? value : -value;
      finish_enumeration(leaf.chance, 1);
      return;
    }
    if (!node.expanded) {
      install_priors(node, legal_ids, count, priors);
      if (leaf.node == root_index_ && root_noise_ && !root_noised_)
        add_root_noise(node);
      node.expanded = true;
    }
    node.pending = false;
    --pending_count_;
    for (int32_t index : leaf.path)
      --nodes_[index].virtual_visits;
    corrections_ = leaf.corrections;
    backpropagate(leaf.path, value, node.player);
    corrections_.clear();
    ++completed_;
  }

  void install_priors(Node &node, const int32_t *legal_ids, size_t count,
                      const float *priors) {
    node.edges.clear();
    node.edges.reserve(count);
    float total = 0.0f;
    for (size_t index = 0; index < count; ++index) {
      Edge edge;
      edge.key = legal_ids[index];
      edge.prior = std::max(priors[index], 0.0f);
      total += edge.prior;
      node.edges.push_back(edge);
    }
    std::sort(node.edges.begin(), node.edges.end(),
              [](const Edge &a, const Edge &b) { return a.key < b.key; });
    if (total > 0.0f)
      for (Edge &edge : node.edges)
        edge.prior /= total;
    else
      for (Edge &edge : node.edges)
        edge.prior = 1.0f / static_cast<float>(count);
    for (Edge &edge : node.edges)
      edge.clean_prior = edge.prior;
  }

  // Re-root the existing tree on `new_root` when the public action sequence
  // since the previous root leads to an expanded decision node. Reveals are
  // derived from the actual transitions (public board refills, the observer's
  // own deck reservations), never from the opponent's hidden information.
  // Retained visits stay in the statistics; the new budget counts fresh
  // simulations only, like the Python search's reused root.
  bool try_advance(const Game &new_root, const std::vector<int32_t> &actions,
                   uint64_t seed, bool root_noise, int num_simulations) {
    if (pending_count_ != 0 || actions.empty() || new_root.is_game_over())
      return false;
    Game replay = root_.clone_light();
    int32_t current = root_index_;
    std::vector<LegalAction> legal;
    for (int32_t action_id : actions) {
      const Node &node = nodes_[current];
      if (node.kind != NodeKind::Decision)
        return false;
      const Edge *edge = node.find(action_id);
      if (edge == nullptr || edge->child < 0)
        return false;
      legal.clear();
      auto sink = [&legal](int id, const Action &action) {
        legal.push_back(LegalAction{id, action});
      };
      ActionEncoderV4::for_each_legal_with_id(replay, sink);
      auto it = std::find_if(legal.begin(), legal.end(),
                             [action_id](const LegalAction &e) { return e.id == action_id; });
      if (it == legal.end())
        return false;
      const uint8_t actor = static_cast<uint8_t>(replay.current_player());
      const int32_t revealed = apply_with_reveal(replay, it->action, actor);
      int32_t next = edge->child;
      if (nodes_[next].kind == NodeKind::Chance) {
        if (revealed == NO_REVEAL)
          return false;
        const Edge *outcome = nodes_[next].find(revealed);
        if (outcome == nullptr || outcome->child < 0)
          return false;
        next = outcome->child;
      } else if (revealed != NO_REVEAL) {
        return false;
      }
      current = next;
    }
    Node &candidate = nodes_[current];
    if (candidate.kind != NodeKind::Decision || !candidate.expanded ||
        candidate.player != new_root.current_player() ||
        replay.board.observable_hash(static_cast<uint8_t>(observer_)) !=
            new_root.board.observable_hash(static_cast<uint8_t>(observer_)))
      return false;
    root_index_ = current;
    root_ = new_root.clone_light();
    rng_.seed(seed);
    root_noise_ = root_noise;
    root_noised_ = false;
    reused_visits_ = static_cast<int>(candidate.visits);
    floors_.clear();
    budget_ = num_simulations > 0 ? num_simulations : config_.num_simulations;
    completed_ = 0;
    rollout_phase_ = RolloutPhase::NotStarted;
    rollout_candidates_.clear();
    rollout_worlds_.clear();
    ++generation_;
    if (root_noise_)
      add_root_noise(candidate);
    return true;
  }

  int reused_visits() const { return reused_visits_; }

  void discard(const PendingLeaf &leaf) {
    if (leaf.generation != generation_)
      return;
    Node &node = nodes_[leaf.node];
    node.pending = false;
    --pending_count_;
    for (int32_t index : leaf.path)
      --nodes_[index].virtual_visits;
  }

  std::vector<std::pair<int32_t, uint32_t>> root_visits() const {
    std::vector<std::pair<int32_t, uint32_t>> result;
    for (const Edge &edge : root().edges)
      if (edge.child >= 0 && nodes_[edge.child].visits)
        result.emplace_back(edge.key, nodes_[edge.child].visits);
    return result;
  }

  std::vector<std::pair<int32_t, float>> root_action_values() const {
    std::vector<std::pair<int32_t, float>> result;
    const Node &r = root();
    for (const Edge &edge : r.edges)
      if (edge.child >= 0 && nodes_[edge.child].visits)
        result.emplace_back(edge.key, static_cast<float>(
                                          q_from_parent(r, nodes_[edge.child])));
    return result;
  }

  std::vector<std::pair<int32_t, float>> root_priors() const {
    std::vector<std::pair<int32_t, float>> result;
    for (const Edge &edge : root().edges)
      result.emplace_back(edge.key, edge.prior);
    return result;
  }

  std::vector<std::pair<int32_t, float>> root_clean_priors() const {
    std::vector<std::pair<int32_t, float>> result;
    for (const Edge &edge : root().edges)
      result.emplace_back(edge.key, edge.clean_prior);
    return result;
  }

  // Compulsory root comparisons: each action receives at least `visits`
  // additional root visits (on top of visits it already has) before ordinary
  // PUCT resumes. Mirrors MCTS._prepare_root_visit_floor.
  void set_root_visit_floor(const std::vector<std::pair<int32_t, int>> &requested) {
    floors_.clear();
    int total = 0;
    for (const auto &[action_id, visits] : requested) {
      if (visits < 0)
        throw std::invalid_argument("root visit floor must be non-negative");
      if (visits == 0)
        continue;
      total += visits;
      int existing = 0;
      const Edge *edge = root().find(action_id);
      if (edge != nullptr && edge->child >= 0)
        existing = static_cast<int>(nodes_[edge->child].visits);
      floors_.push_back(Floor{action_id, existing, existing + visits});
    }
    if (total > budget_)
      throw std::invalid_argument("root visit floors exceed the simulation budget");
  }

  // Visits that the floor itself forced: min(target - baseline, visits - baseline).
  std::vector<std::pair<int32_t, int>> root_visit_floor_allocated() const {
    std::vector<std::pair<int32_t, int>> result;
    for (const Floor &floor : floors_) {
      int visits = 0;
      const Edge *edge = root().find(floor.action);
      if (edge != nullptr && edge->child >= 0)
        visits = static_cast<int>(nodes_[edge->child].visits);
      const int forced = std::min(std::max(0, floor.target - floor.baseline),
                                  std::max(0, visits - floor.baseline));
      result.emplace_back(floor.action, forced);
    }
    return result;
  }

  float root_value() const { return static_cast<float>(root().q()); }
  int slot() const { return slot_; }
  void set_slot(int slot) { slot_ = slot; }
  void set_generation(uint32_t generation) { generation_ = generation; }
  int observer() const { return observer_; }

private:
  static constexpr int32_t NO_REVEAL = -2;

  int32_t new_node(NodeKind kind, uint8_t player) {
    Node node;
    node.kind = kind;
    node.player = player;
    nodes_.push_back(std::move(node));
    return static_cast<int32_t>(nodes_.size() - 1);
  }

  static float terminal_value(const Game &world, uint8_t player) {
    const int winner = world.winner();
    if (winner == -2)
      return 0.0f;
    return winner == player ? 1.0f : -1.0f;
  }

  // Apply in the sampled world and report the card the observer now sees:
  // the board refill after a visible purchase/reservation, or the observer's
  // own deck reservation. NO_REVEAL otherwise (including hidden opponent deck
  // reservations, which stay merged inside the information set).
  int32_t apply_with_reveal(Game &world, const Action &action, uint8_t actor) {
    int level = -1;
    int slot = -1;
    bool track_slot = false;
    if ((action.type == PURCHASE && !action.from_reserved) ||
        action.type == RESERVE_VISIBLE) {
      for (int l = 0; l < 3 && slot < 0; ++l)
        for (int s = 0; s < 4; ++s)
          if (world.board.visible[l][s] == action.card_id) {
            level = l;
            slot = s;
            break;
          }
      track_slot = slot >= 0;
    }
    std::array<int8_t, 3> reserved_before{};
    const bool track_reserve =
        action.type == RESERVE_DECK && actor == observer_;
    if (track_reserve)
      reserved_before = world.board.players[actor].reserved;
    bool applied;
    if (action.type == PASS)
      applied = world.apply_forced_pass(false);
    else
      applied = world.apply_trusted(action, false);
    if (!applied)
      throw std::logic_error("legal action failed to apply in a sampled world");
    if (track_slot)
      return static_cast<int32_t>(world.board.visible[level][slot]); // -1 if empty
    if (track_reserve) {
      const auto &after = world.board.players[actor].reserved;
      for (int i = 0; i < 3; ++i)
        if (after[i] != reserved_before[i] && after[i] >= 0)
          return static_cast<int32_t>(after[i]);
      return -1;
    }
    return NO_REVEAL;
  }

  void encode_leaf(const Game &world, Batch &batch) const {
    const int player = world.current_player();
    const auto base = StateEncoder::encode_canonical(world, player,
                                                     static_cast<int8_t>(player));
    batch.features.insert(batch.features.end(), base.begin(), base.end());
    if (config_.public_card_features) {
      const auto extra = StateEncoder::encode_public_card_statistics(
          world, player, static_cast<uint8_t>(player));
      batch.features.insert(batch.features.end(), extra.begin(), extra.end());
    }
    if (config_.physical_seat_feature)
      batch.features.push_back(static_cast<float>(2 * player - 1));
    if (config_.return_phase_feature)
      batch.features.push_back(world.board.waiting_return ? 1.0f : 0.0f);
    auto sink = [&batch](int id, const Action &) {
      batch.legal_ids.push_back(id);
    };
    ActionEncoderV4::for_each_legal_with_id(world, sink);
    batch.offsets.push_back(static_cast<int32_t>(batch.legal_ids.size()));
  }

  void reshuffle_decks(Game &world) {
    world.board.begin_unchecked_mutation();
    for (int level = 0; level < 3; ++level)
      std::shuffle(world.board.decks[level].begin(), world.board.decks[level].end(), rng_);
  }

  double cpuct_scale(const Node &node, const std::vector<LegalAction> &legal) const {
    if (!config_.dynamic_cpuct)
      return 1.0;
    double weight_sum = 0.0, mean = 0.0, m2 = 0.0;
    int count = 0;
    size_t cursor = 0;
    for (const LegalAction &entry : legal) {
      const Edge *edge = node.find_from(entry.id, cursor);
      if (edge == nullptr || edge->child < 0)
        continue;
      const Node &child = nodes_[edge->child];
      if (child.visits == 0)
        continue;
      const double q = q_from_parent(node, child);
      const double w = child.visits;
      weight_sum += w;
      const double delta = q - mean;
      mean += delta * w / weight_sum;
      m2 += w * delta * (q - mean);
      ++count;
    }
    const double variance = count >= 2 && weight_sum > 0.0 ? std::max(0.0, m2 / weight_sum) : 0.0;
    const double floor = config_.dynamic_cpuct_variance_floor;
    const double scale = floor > 0.0 ? std::sqrt((variance + floor) / floor) : std::sqrt(variance);
    return std::min<double>(config_.dynamic_cpuct_max_scale,
                            std::max<double>(config_.dynamic_cpuct_min_scale, scale));
  }

  struct Floor {
    int32_t action;
    int baseline;
    int target;
  };

  // Largest remaining deficit wins; ties prefer the higher prior, then the
  // lower action id. Returns legal.size() when every floor is satisfied.
  size_t select_floor(const Node &node, const std::vector<LegalAction> &legal) const {
    size_t best = legal.size();
    int best_deficit = 0;
    double best_prior = -1.0;
    for (const Floor &floor : floors_) {
      size_t index = legal.size();
      for (size_t i = 0; i < legal.size(); ++i)
        if (legal[i].id == floor.action) {
          index = i;
          break;
        }
      if (index == legal.size())
        continue;
      const Edge *edge = node.find(floor.action);
      int allocated = 0;
      double prior = 0.0;
      if (edge != nullptr) {
        prior = edge->prior;
        if (edge->child >= 0)
          allocated = static_cast<int>(nodes_[edge->child].visits) +
                      nodes_[edge->child].virtual_visits;
      }
      const int deficit = floor.target - allocated;
      if (deficit <= 0)
        continue;
      if (deficit > best_deficit ||
          (deficit == best_deficit &&
           (prior > best_prior || (prior == best_prior && floor.action < legal[best].id)))) {
        best = index;
        best_deficit = deficit;
        best_prior = prior;
      }
    }
    return best;
  }

  double q_from_parent(const Node &parent, const Node &child) const {
    const double q = child.q();
    return child.player == parent.player ? q : -q;
  }

  // Value of an edge as seen from `node`, mirroring the Python search:
  // an untouched edge uses FPU, a created child uses its Q (0 before the first
  // real visit) minus the virtual-loss penalty.
  void edge_statistics(const Node &node, const Edge *edge, double fpu,
                       double &q, int &visits, int &allocated) const {
    if (edge == nullptr || edge->child < 0) {
      q = fpu;
      visits = 0;
      allocated = 0;
      return;
    }
    const Node &child = nodes_[edge->child];
    visits = static_cast<int>(child.visits);
    allocated = visits + child.virtual_visits;
    q = q_from_parent(node, child);
    if (child.virtual_visits)
      q -= config_.virtual_loss * child.virtual_visits / std::max(1, allocated);
  }

  size_t select_flat(const Node &node, const std::vector<LegalAction> &legal) {
    edge_cache_.clear();
    double visited_mass = 0.0;
    size_t cursor = 0;
    for (const LegalAction &entry : legal) {
      const Edge *edge = node.find_from(entry.id, cursor);
      edge_cache_.push_back(edge);
      if (edge != nullptr && edge->child >= 0) {
        const Node &child = nodes_[edge->child];
        if (child.visits + child.virtual_visits > 0)
          visited_mass += edge->prior;
      }
    }
    const double fpu =
        node.q() - config_.fpu_reduction * std::sqrt(std::max(0.0, visited_mass));
    const double total = std::sqrt(
        std::max(1.0, static_cast<double>(node.visits + node.virtual_visits)));
    const double c_puct = config_.c_puct * cpuct_scale(node, legal);
    size_t best = 0;
    double best_score = -1e300;
    for (size_t index = 0; index < legal.size(); ++index) {
      const Edge *edge = edge_cache_[index];
      const double prior = edge ? edge->prior : config_.unseen_action_prior;
      double q;
      int visits, allocated;
      edge_statistics(node, edge, fpu, q, visits, allocated);
      const double score = q + c_puct * prior * total / (1.0 + allocated);
      if (score > best_score) {
        best_score = score;
        best = index;
      }
    }
    return best;
  }

  size_t select_grouped(const Node &node, const std::vector<LegalAction> &legal) {
    // Group statistics over the legal members present in this world.
    edge_cache_.clear();
    group_of_.clear();
    groups_.clear();
    size_t cursor = 0;
    for (const LegalAction &entry : legal) {
      const Edge *edge = node.find_from(entry.id, cursor);
      edge_cache_.push_back(edge);
      const int group = semantic_group_id(entry.id);
      size_t position = groups_.size();
      for (size_t g = 0; g < groups_.size(); ++g)
        if (groups_[g].group == group) {
          position = g;
          break;
        }
      if (position == groups_.size())
        groups_.push_back(GroupStat{group, 0.0, 0.0, 0, 0});
      group_of_.push_back(position);
      GroupStat &stat = groups_[position];
      stat.prior += edge ? edge->prior : config_.unseen_action_prior;
      if (edge != nullptr && edge->child >= 0) {
        const Node &child = nodes_[edge->child];
        stat.real += static_cast<int>(child.visits);
        stat.allocated += static_cast<int>(child.visits) + child.virtual_visits;
        if (child.visits)
          stat.value += q_from_parent(node, child) * child.visits;
      }
    }
    double visited_mass = 0.0;
    for (const GroupStat &stat : groups_)
      if (stat.allocated > 0)
        visited_mass += stat.prior;
    const double fpu =
        node.q() - config_.fpu_reduction * std::sqrt(std::max(0.0, visited_mass));
    const double total = std::sqrt(
        std::max(1.0, static_cast<double>(node.visits + node.virtual_visits)));
    const double c_puct = config_.c_puct * cpuct_scale(node, legal);
    size_t best_group = 0;
    double best_score = -1e300;
    for (size_t g = 0; g < groups_.size(); ++g) {
      const GroupStat &stat = groups_[g];
      const double q = stat.real ? stat.value / stat.real : fpu;
      const double score = q + c_puct * stat.prior * total / (1.0 + stat.allocated);
      if (score > best_score) {
        best_score = score;
        best_group = g;
      }
    }
    const GroupStat &chosen = groups_[best_group];
    const double group_q = chosen.real ? chosen.value / chosen.real : node.q();
    const double denominator = std::max(chosen.prior, 1e-12);
    double conditional_visited = 0.0;
    for (size_t index = 0; index < legal.size(); ++index) {
      if (group_of_[index] != best_group)
        continue;
      const Edge *edge = edge_cache_[index];
      if (edge != nullptr && edge->child >= 0) {
        const Node &child = nodes_[edge->child];
        if (child.visits + child.virtual_visits > 0)
          conditional_visited += edge->prior / denominator;
      }
    }
    const double member_fpu =
        group_q - config_.fpu_reduction * std::sqrt(std::max(0.0, conditional_visited));
    const double member_total =
        std::sqrt(std::max(1.0, static_cast<double>(chosen.allocated)));
    size_t best = legal.size();
    best_score = -1e300;
    for (size_t index = 0; index < legal.size(); ++index) {
      if (group_of_[index] != best_group)
        continue;
      const Edge *edge = edge_cache_[index];
      const double prior =
          (edge ? edge->prior : config_.unseen_action_prior) / denominator;
      double q;
      int visits, allocated;
      edge_statistics(node, edge, member_fpu, q, visits, allocated);
      const double score = q + c_puct * config_.intra_group_cpuct * prior *
                                   member_total / (1.0 + allocated);
      if (score > best_score) {
        best_score = score;
        best = index;
      }
    }
    return best;
  }

  // Backs up `value` (seen by `perspective`) along the path. Control-variate
  // corrections (corrections_) shift the value for every node at or above the
  // enumerated chance node they name, walking from the leaf towards the root.
  void backpropagate(const std::vector<int32_t> &path, float value,
                     uint8_t perspective) {
    if (corrections_.empty()) {
      for (int32_t index : path) {
        Node &node = nodes_[index];
        ++node.visits;
        node.value_sum += node.player == perspective ? value : -value;
      }
      return;
    }
    double value_p0 = perspective == 0 ? value : -value;
    for (size_t k = path.size(); k-- > 0;) {
      const int32_t index = path[k];
      for (const auto &correction : corrections_)
        if (correction.first == index)
          value_p0 += correction.second;
      Node &node = nodes_[index];
      ++node.visits;
      node.value_sum += node.player == 0 ? value_p0 : -value_p0;
    }
  }

  static bool reveals_to_observer_static(const Action &action, uint8_t actor,
                                         int observer) {
    if ((action.type == PURCHASE && !action.from_reserved) ||
        action.type == RESERVE_VISIBLE)
      return true;
    return action.type == RESERVE_DECK && actor == observer;
  }
  bool reveals_to_observer(const Action &action, uint8_t actor) const {
    return reveals_to_observer_static(action, actor, observer_);
  }
  bool chance_needs_enumeration(int32_t chance_index) const {
    return chance_index < 0 || !nodes_[chance_index].enumerated;
  }

  // Tier (0-based) refilled or drawn by a reveal-producing action.
  static int reveal_level(const Game &before, const Action &action) {
    if (action.type == RESERVE_DECK)
      return action.deck_level;
    for (int l = 0; l < 3; ++l)
      for (int s = 0; s < 4; ++s)
        if (before.board.visible[l][s] == action.card_id)
          return l;
    return -1;
  }

  // Put `card` where the next draw of `level` takes it, swapping it with the
  // current top of the deck (from the deck itself or the opponent's hidden
  // reservations, which the observer cannot tell apart).
  bool force_next_draw(Game &world, int level, int card) const {
    Board &board = world.board;
    auto &deck = board.decks[level];
    if (deck.empty())
      return false;
    board.begin_unchecked_mutation();
    const size_t top = deck.size() - 1;
    for (size_t i = 0; i < deck.size(); ++i)
      if (deck[i] == card) {
        std::swap(deck[i], deck[top]);
        return true;
      }
    PlayerState &opponent = board.players[1 - observer_];
    for (int slot = 0; slot < 3; ++slot)
      if (opponent.reserved_is_hidden[slot] && opponent.reserved[slot] == card) {
        opponent.reserved[slot] = static_cast<int8_t>(deck[top]);
        deck[top] = static_cast<uint8_t>(card);
        return true;
      }
    return false;
  }

  struct ChancePending {
    std::vector<int32_t> path; // root .. chance node
    int outstanding = 0;
  };

  // Enumerate the reveals below `chance_index` (an edge of the current node
  // taken by `action`). Returns true when the simulation ends here because
  // evaluations were requested; false when nothing could be enumerated (the
  // caller continues with the sampled reveal).
  bool enumerate_chance(int32_t chance_index, const Game &before, const Action &action,
                        uint8_t actor, const std::vector<int32_t> &path, Batch &batch,
                        std::vector<PendingLeaf> &pending, Stats &stats) {
    const int level = reveal_level(before, action);
    if (level < 0) {
      nodes_[chance_index].enumerated = true;
      return false;
    }
    const std::vector<uint8_t> pool =
        before.board.observable_card_pool(static_cast<uint8_t>(observer_), level + 1);
    if (pool.empty() || before.board.decks[level].empty()) {
      nodes_[chance_index].enumerated = true;
      return false;
    }
    ChancePending record;
    record.path = path;
    for (uint8_t card : pool) {
      Game world = before.clone_light();
      if (!force_next_draw(world, level, card))
        continue;
      const int32_t revealed = apply_with_reveal(world, action, actor);
      if (revealed != static_cast<int32_t>(card))
        throw std::logic_error("forced reveal did not surface the expected card");
      Node &chance = nodes_[chance_index];
      Edge *edge = chance.find(revealed);
      if (edge == nullptr)
        edge = &chance.insert(revealed, 0.0f);
      const uint8_t mover = static_cast<uint8_t>(world.current_player());
      if (edge->child < 0) {
        const int32_t child = new_node(NodeKind::Decision, mover);
        nodes_[chance_index].find(revealed)->child = child;
        edge = nodes_[chance_index].find(revealed);
      }
      Node &child = nodes_[edge->child];
      if (child.expanded || child.pending) {
        // Already known (tree reuse); use its current estimate.
        const double q = child.kind == NodeKind::Terminal ? child.terminal_value : child.q();
        edge->chance_value = static_cast<float>(child.player == actor ? q : -q);
        continue;
      }
      if (world.is_game_over()) {
        child.kind = NodeKind::Terminal;
        child.expanded = true;
        child.terminal_value = terminal_value(world, child.player);
        edge->chance_value =
            child.player == actor ? child.terminal_value : -child.terminal_value;
        continue;
      }
      encode_leaf(world, batch);
      batch.slots.push_back(slot_);
      child.pending = true;
      PendingLeaf leaf{slot_, edge->child, {}, generation_, false, -1, {}};
      leaf.chance = chance_index;
      pending.push_back(std::move(leaf));
      ++record.outstanding;
      ++stats.chance_rows;
    }
    ++stats.chance_enumerations;
    Node &chance = nodes_[chance_index];
    chance.pending = true;
    for (int32_t index : path)
      ++nodes_[index].virtual_visits;
    ++pending_count_;
    chance_pending_.push_back(std::make_pair(chance_index, std::move(record)));
    if (chance_pending_.back().second.outstanding == 0)
      finish_enumeration(chance_index, 0);
    return true;
  }

  // Called per evaluated child; completes the chance node once all arrived.
  void finish_enumeration(int32_t chance_index, int arrived) {
    for (size_t i = 0; i < chance_pending_.size(); ++i) {
      if (chance_pending_[i].first != chance_index)
        continue;
      ChancePending &record = chance_pending_[i].second;
      record.outstanding -= arrived;
      if (record.outstanding > 0)
        return;
      Node &chance = nodes_[chance_index];
      double mean = 0.0;
      double worst = 1.0;
      int count = 0;
      for (const Edge &edge : chance.edges) {
        if (edge.child < 0)
          continue;
        mean += edge.chance_value;
        worst = std::min<double>(worst, edge.chance_value);
        ++count;
      }
      mean = count ? mean / count : 0.0;
      if (!count)
        worst = 0.0;
      const double risk = std::max(0.0f, std::min(1.0f, config_.chance_risk_weight));
      const double aggregate = (1.0 - risk) * mean + risk * worst;
      chance.chance_mean = static_cast<float>(mean);
      chance.enumerated = true;
      chance.pending = false;
      std::vector<int32_t> path = std::move(record.path);
      chance_pending_.erase(chance_pending_.begin() + static_cast<long>(i));
      for (int32_t index : path)
        --nodes_[index].virtual_visits;
      --pending_count_;
      corrections_.clear();
      backpropagate(path, static_cast<float>(aggregate), chance.player);
      ++completed_;
      return;
    }
    throw std::logic_error("enumerated reveal arrived for an unknown chance node");
  }

  void add_root_noise(Node &root) {
    root_noised_ = true;
    if (root.edges.empty())
      return;
    std::gamma_distribution<double> gamma(config_.dirichlet_alpha, 1.0);
    std::vector<double> noise(root.edges.size());
    double total = 0.0;
    for (double &sample : noise) {
      sample = gamma(rng_);
      total += sample;
    }
    if (total <= 0.0)
      return;
    const double epsilon = config_.dirichlet_epsilon;
    for (size_t index = 0; index < root.edges.size(); ++index)
      root.edges[index].prior = static_cast<float>(
          (1.0 - epsilon) * root.edges[index].prior + epsilon * noise[index] / total);
  }


  enum class RolloutPhase : uint8_t { NotStarted = 0, Running = 1, Finished = 2 };

  // Python's canonical_unseen_world: sort each tier's unseen pool (deck plus
  // the opponent's hidden reservations of that tier) so seeded sampling does
  // not depend on the inaccessible true order/allocation.
  Game canonical_unseen_world() const {
    Game canonical = root_.clone_light();
    Board &board = canonical.board;
    board.begin_unchecked_mutation();
    const int opponent = 1 - observer_;
    PlayerState &player = board.players[opponent];
    for (int level = 0; level < 3; ++level) {
      std::vector<int> pool;
      for (size_t i = 0; i < board.decks[level].size(); ++i)
        pool.push_back(static_cast<int>(board.decks[level][i]));
      std::vector<int> slots;
      for (int slot = 0; slot < 3; ++slot) {
        const int card = player.reserved[slot];
        if (player.reserved_is_hidden[slot] && card >= 0 &&
            get_card(card).level == level + 1) {
          slots.push_back(slot);
          pool.push_back(card);
        }
      }
      std::sort(pool.begin(), pool.end());
      const size_t deck_size = pool.size() - slots.size();
      board.decks[level].clear();
      for (size_t i = 0; i < deck_size; ++i)
        board.decks[level].push_back_unchecked(static_cast<uint8_t>(pool[i]));
      for (size_t k = 0; k < slots.size(); ++k)
        player.reserved[slots[k]] = static_cast<int8_t>(pool[deck_size + k]);
    }
    return canonical;
  }

  void start_rollouts(Stats &stats) {
    rollout_phase_ = RolloutPhase::Finished;
    rollout_candidates_.clear();
    const Node &r = root();
    // Best action first (visits, then Q), then further visited decisions
    // whose semantic group differs, as in MCTS._root_rollout_candidate_ids.
    std::vector<std::pair<int32_t, uint32_t>> visited;
    for (const Edge &edge : r.edges)
      if (edge.child >= 0 && nodes_[edge.child].visits)
        visited.emplace_back(edge.key, nodes_[edge.child].visits);
    if (visited.size() < 2)
      return;
    auto q_of = [&](int32_t key) {
      return q_from_parent(r, nodes_[r.find(key)->child]);
    };
    int32_t best = visited[0].first;
    for (const auto &[key, visits] : visited) {
      const uint32_t best_visits = nodes_[r.find(best)->child].visits;
      if (visits > best_visits || (visits == best_visits && q_of(key) > q_of(best)))
        best = key;
    }
    std::sort(visited.begin(), visited.end(),
              [](const auto &a, const auto &b) {
                return a.second != b.second ? a.second > b.second : a.first < b.first;
              });
    rollout_candidates_.push_back(best);
    std::vector<int> groups{semantic_group_id(best)};
    for (const auto &[key, visits] : visited) {
      if (static_cast<int>(rollout_candidates_.size()) >= config_.rollout_candidates)
        break;
      const int group = semantic_group_id(key);
      if (visits < static_cast<uint32_t>(config_.rollout_min_visits) ||
          std::find(groups.begin(), groups.end(), group) != groups.end())
        continue;
      groups.push_back(group);
      rollout_candidates_.push_back(key);
    }
    if (rollout_candidates_.size() < 2) {
      rollout_candidates_.clear();
      return;
    }
    // Root actions by id.
    std::vector<LegalAction> root_legal;
    auto sink = [&root_legal](int id, const Action &action) {
      root_legal.push_back(LegalAction{id, action});
    };
    ActionEncoderV4::for_each_legal_with_id(root_, sink);
    std::vector<Action> candidate_actions;
    for (int32_t key : rollout_candidates_) {
      auto it = std::find_if(root_legal.begin(), root_legal.end(),
                             [key](const LegalAction &entry) { return entry.id == key; });
      if (it == root_legal.end())
        throw std::logic_error("rollout candidate is not a legal root action");
      candidate_actions.push_back(it->action);
    }
    const Game canonical = canonical_unseen_world();
    const size_t C = rollout_candidates_.size();
    rollout_samples_ = config_.rollout_samples;
    rollout_worlds_.clear();
    rollout_worlds_.reserve(C * rollout_samples_);
    for (int sample = 0; sample < rollout_samples_; ++sample) {
      const Game world =
          canonical.shuffled_clone(static_cast<uint8_t>(observer_), rng_());
      for (size_t c = 0; c < C; ++c) {
        Game child = world.clone_light();
        bool applied = candidate_actions[c].type == PASS
                           ? child.apply_forced_pass(false)
                           : child.apply_trusted(candidate_actions[c], false);
        if (!applied)
          throw std::logic_error("rollout root action failed in a sampled world");
        rollout_worlds_.push_back(std::move(child));
      }
    }
    const size_t total = rollout_worlds_.size();
    rollout_live_.assign(total, 1);
    rollout_returns_.assign(total, 0.0f);
    rollout_terminal_.assign(total, 0);
    rollout_ply_ = 0;
    rollout_phase_ = RolloutPhase::Running;
    ++stats.rollout_games;
  }

  void collect_rollouts(Batch &batch, std::vector<PendingLeaf> &pending, Stats &stats) {
    bool any_live = false;
    for (size_t index = 0; index < rollout_worlds_.size(); ++index) {
      if (!rollout_live_[index])
        continue;
      Game &world = rollout_worlds_[index];
      if (world.is_game_over()) {
        rollout_returns_[index] = terminal_value(world, static_cast<uint8_t>(observer_));
        rollout_terminal_[index] = 1;
        rollout_live_[index] = 0;
        continue;
      }
      any_live = true;
      encode_leaf(world, batch);
      batch.slots.push_back(slot_);
      PendingLeaf leaf;
      leaf.slot = slot_;
      leaf.node = static_cast<int32_t>(index);
      leaf.generation = generation_;
      leaf.rollout = true;
      pending.push_back(std::move(leaf));
      ++rollout_pending_;
      ++stats.rollout_rows;
    }
    if (!any_live)
      rollout_phase_ = RolloutPhase::Finished;
  }

  void apply_rollout(const PendingLeaf &leaf, const int32_t *legal_ids, size_t count,
                     const float *priors, float value) {
    const size_t index = static_cast<size_t>(leaf.node);
    Game &world = rollout_worlds_[index];
    --rollout_pending_;
    if (rollout_ply_ >= config_.rollout_horizon) {
      // Cutoff: bootstrap with the acting side's Value, root perspective.
      const float clipped = std::max(-1.0f, std::min(1.0f, value));
      rollout_returns_[index] =
          world.current_player() == observer_ ? clipped : -clipped;
      rollout_live_[index] = 0;
    } else {
      size_t best = 0;
      for (size_t i = 1; i < count; ++i)
        if (priors[i] > priors[best])
          best = i;
      const int32_t chosen = legal_ids[best];
      bool applied = false;
      auto sink = [&](int id, const Action &action) {
        if (!applied && id == chosen) {
          applied = action.type == PASS ? world.apply_forced_pass(false)
                                        : world.apply_trusted(action, false);
        }
      };
      ActionEncoderV4::for_each_legal_with_id(world, sink);
      if (!applied)
        throw std::logic_error("rollout policy chose an action that is not legal");
    }
    if (rollout_pending_ == 0) {
      ++rollout_ply_;
      bool any_live = false;
      for (uint8_t live : rollout_live_)
        any_live = any_live || live;
      if (!any_live)
        rollout_phase_ = RolloutPhase::Finished;
    }
  }

  RolloutPhase rollout_phase_ = RolloutPhase::NotStarted;
  std::vector<int32_t> rollout_candidates_;
  std::vector<Game> rollout_worlds_;
  std::vector<uint8_t> rollout_live_;
  std::vector<float> rollout_returns_;
  std::vector<uint8_t> rollout_terminal_;
  int rollout_samples_ = 0;
  int rollout_ply_ = 0;
  int rollout_pending_ = 0;

  Config config_;
  Game root_;
  int observer_;
  std::mt19937_64 rng_;
  bool root_noise_;
  bool root_noised_ = false;
  int budget_;
  int completed_ = 0;
  int reused_visits_ = 0;
  std::vector<Floor> floors_;
  int pending_count_ = 0;
  int slot_ = -1;
  uint32_t generation_ = 0;
  int32_t root_index_ = -1;
  std::vector<Node> nodes_;
  std::vector<LegalAction> legal_;
  Game before_;
  std::vector<std::pair<int32_t, float>> corrections_;
  std::vector<std::pair<int32_t, ChancePending>> chance_pending_;
  std::vector<const Edge *> edge_cache_;
  std::vector<int32_t> path_; // descent path scratch, reused per simulation
  std::vector<size_t> group_of_;
  struct GroupStat {
    int group;
    double prior = 0.0;
    double value = 0.0;
    int real = 0;
    int allocated = 0;
  };
  std::vector<GroupStat> groups_;
};

class Session {
public:
  explicit Session(const Config &config) : config_(config) {
    if (config.num_simulations < 1 || config.leaf_batch_size < 1)
      throw std::invalid_argument("simulations and leaf batch must be positive");
    if (config.max_depth < 1)
      throw std::invalid_argument("max_depth must be positive");
    if (config.num_threads < 1)
      throw std::invalid_argument("num_threads must be positive");
  }

  const Config &config() const { return config_; }
  int state_dim() const { return config_.state_dim(); }

  int add_game(const Game &root, int observer, uint64_t seed, bool root_noise,
               int num_simulations) {
    games_.emplace_back(config_, root, observer, seed, root_noise, num_simulations);
    const int slot = static_cast<int>(games_.size() - 1);
    games_.back().set_slot(slot);
    return slot;
  }

  void reset_game(int slot, const Game &root, int observer, uint64_t seed,
                  bool root_noise, int num_simulations) {
    check_slot(slot);
    // Drop pending leaves of the old tree: their results are ignored later
    // because the generation changes with the new GameSearch.
    for (auto &leaf : pending_)
      if (leaf.slot == slot)
        leaf.generation = ~0u;
    const uint32_t generation = games_[slot].generation() + 1;
    games_[slot] = GameSearch(config_, root, observer, seed, root_noise,
                              num_simulations);
    games_[slot].set_slot(slot);
    games_[slot].set_generation(generation);
  }

  // Reuse the slot's tree when possible; otherwise behave like reset_game.
  bool advance_game(int slot, const Game &root, const std::vector<int32_t> &actions,
                    int observer, uint64_t seed, bool root_noise, int num_simulations) {
    check_slot(slot);
    if (games_[slot].observer() == observer &&
        games_[slot].try_advance(root, actions, seed, root_noise, num_simulations))
      return true;
    reset_game(slot, root, observer, seed, root_noise, num_simulations);
    return false;
  }

  void set_root_visit_floor(int slot, const std::vector<std::pair<int32_t, int>> &floors) {
    check_slot(slot);
    games_[slot].set_root_visit_floor(floors);
  }

  size_t size() const { return games_.size(); }
  bool done(int slot) const {
    check_slot(slot);
    return games_[slot].done();
  }
  bool all_done() const {
    for (const auto &game : games_)
      if (!game.done())
        return false;
    return true;
  }
  const GameSearch &game(int slot) const {
    check_slot(slot);
    return games_[slot];
  }

  Batch collect() {
    if (!pending_.empty())
      throw std::logic_error("apply the previous batch before collecting again");
    Batch batch;
    batch.state_dim = config_.state_dim();
    batch.offsets.push_back(0);
    if (worker_count(games_.size()) <= 1) {
      for (auto &game : games_)
        if (!game.done())
          game.collect(config_.leaf_batch_size, batch, pending_, stats_);
    } else {
      collect_parallel(batch);
    }
    stats_.nodes = 0;
    for (const auto &game : games_)
      stats_.nodes += game.node_count();
    return batch;
  }

  size_t pending() const { return pending_.size(); }

  void apply(const int32_t *legal_ids, const int32_t *offsets, size_t rows,
             const float *priors, const float *values) {
    if (rows != pending_.size())
      throw std::invalid_argument("evaluation rows do not match pending leaves");
    if (worker_count(games_.size()) > 1 &&
        apply_parallel(legal_ids, offsets, rows, priors, values))
      return;
    for (size_t row = 0; row < rows; ++row) {
      const PendingLeaf &leaf = pending_[row];
      const size_t start = static_cast<size_t>(offsets[row]);
      const size_t count = static_cast<size_t>(offsets[row + 1]) - start;
      if (leaf.generation == ~0u)
        continue; // belonged to a tree that was reset
      games_[leaf.slot].apply(leaf, legal_ids + start, count, priors + start,
                              values[row]);
      if (leaf.chance < 0)
        ++stats_.simulations;
    }
    pending_.clear();
  }

  const Stats &stats() const { return stats_; }

private:
  void check_slot(int slot) const {
    if (slot < 0 || static_cast<size_t>(slot) >= games_.size())
      throw std::out_of_range("game slot out of range");
  }

  size_t worker_count(size_t tasks) const {
    return std::min(static_cast<size_t>(config_.num_threads), tasks);
  }

  // Runs task(i) for i in [0, tasks) on worker_count(tasks) threads (the
  // caller is one of them). Each task index runs exactly once; the exception
  // of the lowest failing index is rethrown after every worker has joined.
  template <typename Task> void run_tasks(size_t tasks, Task &&task) {
    std::vector<std::exception_ptr> errors(tasks);
    std::atomic<size_t> next{0};
    auto worker = [&]() {
      for (;;) {
        const size_t index = next.fetch_add(1, std::memory_order_relaxed);
        if (index >= tasks)
          return;
        try {
          task(index);
        } catch (...) {
          errors[index] = std::current_exception();
        }
      }
    };
    std::vector<std::thread> threads;
    const size_t extra = worker_count(tasks) - 1;
    threads.reserve(extra);
    try {
      for (size_t index = 0; index < extra; ++index)
        threads.emplace_back(worker);
    } catch (...) {
      // Could not start every helper: finish the work on the threads we have.
    }
    worker();
    for (std::thread &thread : threads)
      thread.join();
    for (const std::exception_ptr &error : errors)
      if (error)
        std::rethrow_exception(error);
  }

  // Each game appends to its own buffers; offsets inside a part are relative
  // to that part's legal ids and are rebased while concatenating in slot
  // order, which reproduces the sequential batch exactly.
  void collect_parallel(Batch &batch) {
    struct Part {
      Batch batch;
      std::vector<PendingLeaf> pending;
      Stats stats;
    };
    std::vector<Part> parts(games_.size());
    std::exception_ptr failure;
    try {
      run_tasks(games_.size(), [this, &parts](size_t index) {
        if (games_[index].done())
          return;
        Part &part = parts[index];
        part.batch.offsets.push_back(0);
        games_[index].collect(config_.leaf_batch_size, part.batch,
                              part.pending, part.stats);
      });
    } catch (...) {
      // Keep the leaves of every game that succeeded, as the sequential loop
      // keeps those collected before a failure, then report the error.
      failure = std::current_exception();
    }
    size_t feature_count = batch.features.size();
    size_t legal_count = batch.legal_ids.size();
    size_t row_count = batch.slots.size();
    for (const Part &part : parts) {
      feature_count += part.batch.features.size();
      legal_count += part.batch.legal_ids.size();
      row_count += part.batch.slots.size();
    }
    batch.features.reserve(feature_count);
    batch.legal_ids.reserve(legal_count);
    batch.slots.reserve(row_count);
    batch.offsets.reserve(row_count + 1);
    pending_.reserve(pending_.size() + row_count);
    for (Part &part : parts) {
      const int32_t base = static_cast<int32_t>(batch.legal_ids.size());
      batch.features.insert(batch.features.end(), part.batch.features.begin(),
                            part.batch.features.end());
      batch.legal_ids.insert(batch.legal_ids.end(), part.batch.legal_ids.begin(),
                             part.batch.legal_ids.end());
      for (size_t row = 1; row < part.batch.offsets.size(); ++row)
        batch.offsets.push_back(base + part.batch.offsets[row]);
      batch.slots.insert(batch.slots.end(), part.batch.slots.begin(),
                         part.batch.slots.end());
      for (PendingLeaf &leaf : part.pending)
        pending_.push_back(std::move(leaf));
      stats_ += part.stats;
    }
    if (failure)
      std::rethrow_exception(failure);
  }

  // Rows of one game are contiguous (collect concatenates per slot) and are
  // applied in row order by a single thread, so every tree sees the same
  // update sequence as the sequential loop.
  // Returns false, without applying anything, when a game's rows are not
  // contiguous; the caller then uses the sequential loop.
  bool apply_parallel(const int32_t *legal_ids, const int32_t *offsets,
                      size_t rows, const float *priors, const float *values) {
    std::vector<std::pair<size_t, size_t>> segments; // [begin, end) rows
    std::vector<bool> seen(games_.size(), false);
    for (size_t row = 0; row < rows;) {
      const int slot = pending_[row].slot;
      if (slot < 0 || static_cast<size_t>(slot) >= games_.size() || seen[slot])
        return false;
      seen[slot] = true;
      size_t end = row + 1;
      while (end < rows && pending_[end].slot == slot)
        ++end;
      segments.emplace_back(row, end);
      row = end;
    }
    std::vector<uint64_t> simulations(segments.size(), 0);
    std::exception_ptr failure;
    try {
      run_tasks(segments.size(), [&](size_t index) {
        for (size_t row = segments[index].first; row < segments[index].second;
             ++row) {
          const PendingLeaf &leaf = pending_[row];
          const size_t start = static_cast<size_t>(offsets[row]);
          const size_t count = static_cast<size_t>(offsets[row + 1]) - start;
          if (leaf.generation == ~0u)
            continue; // belonged to a tree that was reset
          games_[leaf.slot].apply(leaf, legal_ids + start, count,
                                  priors + start, values[row]);
          if (leaf.chance < 0)
            ++simulations[index];
        }
      });
    } catch (...) {
      failure = std::current_exception();
    }
    for (uint64_t count : simulations)
      stats_.simulations += count;
    if (failure)
      std::rethrow_exception(failure); // pending_ kept, as in the sequential loop
    pending_.clear();
    return true;
  }

  Config config_;
  std::vector<GameSearch> games_;
  std::vector<PendingLeaf> pending_;
  Stats stats_;
};

} // namespace csplendor::v3search

#endif // CSPLENDOR_MCTS_V3_H
