// Parallel mate search (doc/mate_usage.md「並列の詰み探索」): the shared table,
// its growth, the scheduler over synthetic AND/OR trees, and the solver's
// verdicts against the serial search. Run under ThreadSanitizer in CI.
#include "game.h"
#include "parallel_mate_solver.h"
#include "reveal_verified_solver.h"
#include "solver_flat_table.h"

#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using csplendor::solver_internal::ConcurrentFlatMemoTable;
using csplendor::solver_internal::ConcurrentTableGrowth;
using csplendor::solver_internal::ForceStatus;
using csplendor::solver_internal::ParallelMateSearch;
using csplendor::solver_internal::RevealSearchCancellationToken;
using csplendor::solver_internal::SPLIT_BLANK;
using csplendor::solver_internal::SearchLimitExceeded;
using csplendor::solver_internal::SplitExpansion;

void require(bool condition, const std::string &message) {
  if (!condition)
    throw std::runtime_error(message);
}

struct Key {
  uint64_t value = 0;
  bool empty() const noexcept { return value == 0; }
  bool operator==(const Key &other) const noexcept {
    return value == other.value;
  }
};

struct KeyHash {
  size_t operator()(const Key &key) const noexcept {
    return static_cast<size_t>(key.value * 0x9e3779b97f4a7c15ull);
  }
};

struct Value {
  uint64_t value = 0;
};

using Table = ConcurrentFlatMemoTable<Key, Value, KeyHash>;

void test_table_single_thread() {
  Table table(1000);
  require(table.find(Key{7}) == nullptr, "empty table misses");
  require(table.insert(Key{7}, Value{70}), "first insert");
  require(!table.insert(Key{7}, Value{71}), "a present key keeps its entry");
  require(table.find(Key{7})->value == 70, "the first entry stays");
  for (uint64_t k = 100; k < 2000; ++k)
    table.insert(Key{k}, Value{k});
  require(table.size() == 1000, "inserts stop at the entry limit");
  require(table.dropped() > 0, "the rest are dropped");
  require(table.capacity() == Table::serial_table::capacity_for(1000),
          "without growth the arrays have their final size");
}

// Threads insert overlapping keys (each key's value is fixed) and read them
// back while the arrays grow; every key ends up readable with its value.
void test_table_growth_under_threads() {
  ConcurrentTableGrowth growth;
  const size_t limit = 400000;
  Table table(limit, &growth);
  const size_t final_capacity = Table::serial_table::capacity_for(limit);
  require(table.capacity() <= final_capacity / 8 &&
              table.capacity() >= Table::MIN_CAPACITY,
          "starts on the ladder below C / 8");
  const int threads = 8;
  const uint64_t keys = 300000;
  std::atomic<bool> wrong{false};
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t)
    pool.emplace_back([&, t] {
      growth.attach();
      std::mt19937_64 rng(static_cast<uint64_t>(t));
      for (uint64_t i = 0; i < keys / 2; ++i) {
        const uint64_t k = 1 + (rng() % keys);
        table.insert(Key{k}, Value{k * 3});
        const Value *found = table.find(Key{k});
        if (found == nullptr || found->value != k * 3)
          wrong.store(true);
        growth.safepoint();
        if (i % 4096 == 0) { // threads come and go
          growth.detach();
          growth.attach();
        }
      }
      growth.detach();
    });
  for (std::thread &thread : pool)
    thread.join();
  require(!wrong.load(), "a key read back with a wrong value or missing");
  require(table.capacity() == final_capacity,
          "the arrays grew to C (from C / 8)");
  require(table.size() <= limit, "never beyond the entry limit");
  size_t found = 0;
  for (uint64_t k = 1; k <= keys; ++k) {
    const Value *value = table.find(Key{k});
    if (value != nullptr) {
      require(value->value == k * 3, "values survive the growth");
      ++found;
    }
  }
  require(found + table.dropped() >= table.size(), "entries are kept");
}

std::atomic<int> allocations_left{0};

void *failing_calloc(size_t count, size_t size) {
  if (allocations_left.fetch_sub(1) <= 0)
    return nullptr;
  return std::calloc(count, size);
}

// A growth whose allocation fails keeps the array and drops new entries; no
// thread waits forever.
void test_table_growth_allocation_failure() {
  ConcurrentTableGrowth growth;
  allocations_left.store(2); // the first array only
  Table table(400000, &growth, &failing_calloc);
  const size_t start = table.capacity();
  std::vector<std::thread> pool;
  for (int t = 0; t < 4; ++t)
    pool.emplace_back([&, t] {
      growth.attach();
      for (uint64_t k = 1; k <= 60000; ++k) {
        table.insert(Key{k + static_cast<uint64_t>(t) * 60000}, Value{k});
        growth.safepoint();
      }
      growth.detach();
    });
  for (std::thread &thread : pool)
    thread.join();
  require(table.capacity() == start, "the failed growth kept the array");
  require(table.size() <= start / 10 * 7 + 4, "the array fills to load 0.7");
  require(table.flushes() > 0, "then it is flushed and refilled");
  require(table.size() > 0, "and keeps storing");
}

// At its entry limit the table is flushed at a safepoint (as the serial
// table), and every entry read back is right.
void test_table_flush_at_the_limit() {
  ConcurrentTableGrowth growth;
  const size_t limit = 5000;
  Table table(limit, &growth);
  std::atomic<bool> wrong{false};
  std::vector<std::thread> pool;
  for (int t = 0; t < 6; ++t)
    pool.emplace_back([&, t] {
      growth.attach();
      std::mt19937_64 rng(static_cast<uint64_t>(t) + 100);
      for (int i = 0; i < 40000; ++i) {
        const uint64_t k = 1 + rng() % 100000;
        table.insert(Key{k}, Value{k + 1});
        const Value *found = table.find(Key{k});
        if (found != nullptr && found->value != k + 1)
          wrong.store(true);
        growth.safepoint();
      }
      growth.detach();
    });
  for (std::thread &thread : pool)
    thread.join();
  require(!wrong.load(), "a flushed table never returns a wrong entry");
  require(table.flushes() > 0, "the full table was flushed");
  require(table.size() <= limit + 6, "never far beyond the limit");
}

// ---- The scheduler over synthetic trees --------------------------------

ForceStatus combine_and(const std::vector<ForceStatus> &values) {
  bool unknown = false;
  for (ForceStatus value : values) {
    if (value == ForceStatus::REFUTED)
      return ForceStatus::REFUTED;
    unknown = unknown || value == ForceStatus::UNKNOWN;
  }
  return unknown ? ForceStatus::UNKNOWN : ForceStatus::PROVEN;
}

ForceStatus combine_or(const std::vector<ForceStatus> &values) {
  bool unknown = false;
  for (ForceStatus value : values) {
    if (value == ForceStatus::PROVEN)
      return ForceStatus::PROVEN;
    unknown = unknown || value == ForceStatus::UNKNOWN;
  }
  return unknown ? ForceStatus::UNKNOWN : ForceStatus::REFUTED;
}

// A two-level tree: node 0 is the root; its outcomes are frontier nodes,
// whose outcomes are leaves. A frontier may be decided at its expansion.
struct FakeTree {
  struct Branch {
    std::vector<int> children; // node ids (root) or leaf ids (frontiers)
    bool blank = false;
    ForceStatus blank_value = ForceStatus::REFUTED;
    bool complete = true;
  };
  struct NodeData {
    bool or_node = false;
    bool decided = false;
    ForceStatus status = ForceStatus::UNKNOWN;
    std::vector<Branch> branches;
  };
  std::vector<NodeData> nodes;
  std::vector<ForceStatus> leaves;

  ForceStatus value(int id) const {
    const NodeData &node = nodes[static_cast<size_t>(id)];
    if (node.decided)
      return node.status;
    std::vector<ForceStatus> actions;
    for (const Branch &branch : node.branches) {
      if (!branch.complete || branch.children.empty()) {
        actions.push_back(ForceStatus::UNKNOWN);
        continue;
      }
      std::vector<ForceStatus> outcomes;
      for (int child : branch.children)
        outcomes.push_back(id == 0 ? value(child)
                                   : leaves[static_cast<size_t>(child)]);
      // The serial blank cut: tried once outcome 0 is not refuted; proven,
      // the action takes outcome 0's value.
      if (branch.blank && outcomes[0] != ForceStatus::REFUTED &&
          branch.blank_value == ForceStatus::PROVEN)
        actions.push_back(outcomes[0]);
      else
        actions.push_back(combine_and(outcomes));
    }
    return node.or_node ? combine_or(actions) : combine_and(actions);
  }
};

FakeTree random_tree(std::mt19937_64 &rng) {
  FakeTree tree;
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<uint64_t>(n)); };
  auto leaf_value = [&] {
    const int roll = pick(100);
    return roll < 3 ? ForceStatus::UNKNOWN
                    : roll < 55 ? ForceStatus::PROVEN : ForceStatus::REFUTED;
  };
  tree.nodes.push_back({});
  tree.nodes[0].or_node = pick(2) == 0;
  const int root_branches = 1 + pick(5);
  for (int b = 0; b < root_branches; ++b) {
    FakeTree::Branch branch;
    branch.complete = pick(20) != 0;
    const int outcomes = branch.complete ? 1 + pick(3) : 0;
    for (int j = 0; j < outcomes; ++j) {
      FakeTree::NodeData frontier;
      frontier.or_node = pick(2) == 0;
      frontier.decided = pick(6) == 0;
      frontier.status = leaf_value();
      if (!frontier.decided) {
        const int branches = 1 + pick(5);
        for (int c = 0; c < branches; ++c) {
          FakeTree::Branch inner;
          inner.complete = pick(30) != 0;
          const int leaves = inner.complete ? 1 + pick(4) : 0;
          for (int k = 0; k < leaves; ++k) {
            inner.children.push_back(static_cast<int>(tree.leaves.size()));
            tree.leaves.push_back(leaf_value());
          }
          inner.blank = leaves > 1 && pick(3) == 0;
          frontier.branches.push_back(inner);
        }
      }
      branch.children.push_back(static_cast<int>(tree.nodes.size()));
      tree.nodes.push_back(frontier);
    }
    branch.blank = outcomes > 1 && pick(3) == 0;
    tree.nodes[0].branches.push_back(branch);
  }
  // A blank child may prove its action only when no outcome is refuted
  // (blank dominance; unknown outcomes are repetitions); otherwise it fails.
  // Frontiers first: the root's outcomes take their values.
  for (size_t id = tree.nodes.size(); id-- > 0;)
    for (auto &branch : tree.nodes[id].branches) {
      if (!branch.blank)
        continue;
      std::vector<ForceStatus> outcomes;
      for (int child : branch.children)
        outcomes.push_back(id == 0 ? tree.value(child)
                                   : tree.leaves[static_cast<size_t>(child)]);
      branch.blank_value = combine_and(outcomes) != ForceStatus::REFUTED &&
                                   pick(2) == 0
                               ? ForceStatus::PROVEN
                               : ForceStatus::REFUTED;
    }
  return tree;
}

struct FakeWorker {
  using Node = int;
  using Move = int;
  using Key = int;
  using Expansion = SplitExpansion<Move, Key>;

  const FakeTree *tree = nullptr;
  std::shared_ptr<RevealSearchCancellationToken> token;
  std::atomic<int> *stores = nullptr;
  int fail_leaf = -1;           // throws std::runtime_error at this leaf
  int limit_leaf = -1;          // throws a time limit at this leaf
  int attached = 0;

  void expand(const Node &node, Expansion &out) {
    const auto &data = tree->nodes[static_cast<size_t>(node)];
    out = Expansion();
    out.decided = data.decided;
    out.status = data.status;
    out.or_node = data.or_node;
    out.key = node;
    for (size_t b = 0; b < data.branches.size(); ++b) {
      Expansion::Branch branch;
      branch.move = static_cast<int>(b);
      branch.outcomes = data.branches[b].children;
      branch.blank = data.branches[b].blank;
      branch.complete = data.branches[b].complete;
      out.branches.push_back(branch);
    }
  }
  Node child(const Node &, const Expansion &expansion, size_t branch,
             int outcome) {
    return expansion.branches[branch].outcomes[static_cast<size_t>(outcome)];
  }
  ForceStatus search(const Node &node, const Expansion &expansion,
                     size_t branch, int outcome) {
    for (int spin = 0; spin < 200; ++spin)
      if (token->is_cancelled())
        throw SearchLimitExceeded("search cancelled");
    const auto &data = tree->nodes[static_cast<size_t>(node)];
    if (outcome == SPLIT_BLANK)
      return data.branches[branch].blank_value;
    const int leaf = expansion.branches[branch].outcomes[static_cast<size_t>(outcome)];
    if (leaf == fail_leaf)
      throw std::runtime_error("injected failure");
    if (leaf == limit_leaf)
      throw SearchLimitExceeded("time limit exceeded");
    return tree->leaves[static_cast<size_t>(leaf)];
  }
  void store(const Expansion &, ForceStatus status, size_t, int) {
    require(status != ForceStatus::UNKNOWN, "only decided nodes are stored");
    stores->fetch_add(1);
  }
  void set_cancellation_token(std::shared_ptr<RevealSearchCancellationToken> t) {
    token = std::move(t);
  }
  void attach() { ++attached; }
  void detach() { --attached; }
};

ParallelMateSearch<FakeWorker>::Result
run_fake(const FakeTree &tree, int threads,
         std::shared_ptr<RevealSearchCancellationToken> user = {},
         int fail_leaf = -1, int limit_leaf = -1) {
  std::atomic<int> stores{0};
  std::vector<FakeWorker> workers(static_cast<size_t>(threads));
  std::vector<FakeWorker *> handles;
  for (FakeWorker &worker : workers) {
    worker.tree = &tree;
    worker.stores = &stores;
    worker.fail_leaf = fail_leaf;
    worker.limit_leaf = limit_leaf;
    handles.push_back(&worker);
  }
  ParallelMateSearch<FakeWorker> search;
  const auto result = search.run(handles, 0, std::move(user));
  for (const FakeWorker &worker : workers)
    require(worker.attached == 0, "every attach has its detach");
  return result;
}

void test_scheduler_matches_the_tree_value() {
  std::mt19937_64 rng(20261108);
  for (int round = 0; round < 400; ++round) {
    const FakeTree tree = random_tree(rng);
    const ForceStatus expected = tree.value(0);
    for (int threads : {1, 2, 8}) {
      const auto result = run_fake(tree, threads);
      require(result.decided, "a search without limits decides the root");
      require(result.status == expected,
              "round " + std::to_string(round) + " threads " +
                  std::to_string(threads) + ": the root value differs");
    }
  }
}

void test_scheduler_limits_and_failures() {
  std::mt19937_64 rng(7);
  FakeTree tree;
  do
    tree = random_tree(rng);
  while (tree.leaves.size() < 6 || tree.nodes[1].decided);
  auto cancelled = std::make_shared<RevealSearchCancellationToken>();
  cancelled->request_cancel();
  const auto by_caller = run_fake(tree, 4, cancelled);
  require(!by_caller.decided && by_caller.unknown_reason == "search cancelled",
          "a cancelled caller gets an undecided root");

  // A limit in the first leaf (always searched) stops the search.
  FakeTree first_leaf_limit = tree;
  const auto limited = run_fake(first_leaf_limit, 4, {}, -1,
                                tree.nodes[1].branches[0].children.empty()
                                    ? -1
                                    : tree.nodes[1].branches[0].children[0]);
  if (!tree.nodes[1].branches[0].children.empty())
    require(!limited.decided && limited.unknown_reason == "time limit exceeded",
            "a limit leaves the root undecided with its reason");

  bool rethrown = false;
  try {
    if (!tree.nodes[1].branches[0].children.empty())
      run_fake(tree, 4, {}, tree.nodes[1].branches[0].children[0]);
    else
      rethrown = true;
  } catch (const std::runtime_error &exc) {
    rethrown = std::string(exc.what()) == "injected failure";
  }
  require(rethrown, "a worker's failure is rethrown by run()");
}

// ---- The solver ----------------------------------------------------------

Game random_endgame(uint64_t seed, int target) {
  Game game(seed);
  std::mt19937_64 rng(seed);
  while (game.board.players[0].points < target &&
         game.board.players[1].points < target) {
    const auto actions = game.legal_actions();
    if (actions.empty() || game.is_game_over())
      break;
    game.apply(actions[rng() % actions.size()], false);
  }
  return game;
}

void test_solver_verdicts_match_the_serial_search() {
  int compared = 0;
  for (uint64_t seed = 1; seed <= 24; ++seed) {
    const Game game = random_endgame(seed, 9 + static_cast<int>(seed % 4));
    if (game.is_game_over())
      continue;
    for (int attacker = 0; attacker < 2; ++attacker)
      for (bool exact : {true, false}) {
        RevealVerifiedSolver serial(attacker, 3, 0, 0.0, {}, false, 100000,
                                    500000, UINT64_MAX, false, 0, true, exact);
        serial.set_max_memo_states(200000);
        const auto expected = serial.solve(game);
        RevealVerifiedSolver parallel = serial;
        parallel.set_jobs(4);
        const auto result = parallel.solve(game);
        require(result.jobs == 4, "depth 3 runs on the requested threads");
        require(result.unknown_reason.empty(), "no limits, no unknown");
        require(result.proven == expected.proven,
                "seed " + std::to_string(seed) + ": the verdict differs");
        if (result.proven) {
          require(result.line.empty() == expected.line.empty(),
                  "seed " + std::to_string(seed) +
                      ": a proof has a principal line when the serial one has");
          if (result.line.empty())
            continue; // decided before the root's moves
          RevealVerifiedSolver recheck(attacker, 3, 0, 0.0, {}, false, 100000,
                                       500000, result.line.front().action_code,
                                       false, 0, true, exact);
          require(recheck.solve(game).proven,
                  "the serial search proves the parallel line's first move");
        }
        ++compared;
      }
  }
  require(compared >= 40, "enough positions compared");
}

void test_solver_limits() {
  const Game game = random_endgame(3, 11);
  RevealVerifiedSolver limited(0, 4, 2000, 0.0, {}, false, 100000, 500000,
                               UINT64_MAX, false, 0, true, true);
  limited.set_jobs(4);
  limited.set_max_memo_states(100000);
  const auto nodes = limited.solve(game);
  require(nodes.unknown_reason.empty() ||
              nodes.unknown_reason == "node limit exceeded",
          "a node limit is reported as such");
  require(nodes.stats.nodes <= 2000 + 4 * 64 + 64,
          "the threads share the node limit");
  auto token = std::make_shared<RevealSearchCancellationToken>();
  token->request_cancel();
  RevealVerifiedSolver cancelled(0, 4, 0, 0.0, {}, false, 100000, 500000,
                                 UINT64_MAX, false, 0, true, true, token);
  cancelled.set_jobs(4);
  cancelled.set_max_memo_states(100000);
  require(cancelled.solve(game).unknown_reason == "search cancelled",
          "a cancelled token stops the parallel search");
}

} // namespace

int main() {
  try {
    test_table_single_thread();
    test_table_growth_under_threads();
    test_table_growth_allocation_failure();
    test_table_flush_at_the_limit();
    test_scheduler_matches_the_tree_value();
    test_scheduler_limits_and_failures();
    test_solver_verdicts_match_the_serial_search();
    test_solver_limits();
  } catch (const std::exception &exc) {
    std::cerr << "parallel_mate_unit: " << exc.what() << "\n";
    return 1;
  }
  std::cout << "parallel_mate_unit: ok\n";
  return 0;
}
