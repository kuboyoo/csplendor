#ifndef CSPLENDOR_PARALLEL_MATE_SOLVER_H
#define CSPLENDOR_PARALLEL_MATE_SOLVER_H

// Parallel depth-first mate search (doc/mate_usage.md「並列の詰み探索」,
// doc/parallel_search_design.md 第 2 節): the root's moves x reveals (level 1)
// and, under each level-1 child, its moves x reveals (level 2) become work
// handed out in depth-first order; each level-2 child is one ordinary
// depth-first search over a table shared by all threads. Header-only, and
// without threads under Emscripten unless it is built with pthreads: there
// the search runs on the calling thread alone.

#include "solver_types.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define CSPLENDOR_MATE_THREADS 0
#else
#define CSPLENDOR_MATE_THREADS 1
#include <thread>
#endif

#if CSPLENDOR_MATE_THREADS && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif CSPLENDOR_MATE_THREADS && defined(__APPLE__)
#include <sys/sysctl.h>
#elif CSPLENDOR_MATE_THREADS && defined(__linux__)
#include <fstream>
#include <sched.h>
#include <set>
#include <utility>
#endif

namespace csplendor {

// Physical cores this process may run on (SMT siblings counted once; on
// Apple silicon the performance cores), 0 when unknown.
inline unsigned physical_core_count() noexcept {
#if !CSPLENDOR_MATE_THREADS
  return 1;
#elif defined(_WIN32)
  DWORD bytes = 0;
  GetLogicalProcessorInformation(nullptr, &bytes);
  if (bytes == 0)
    return 0;
  std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> info(
      bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
  if (!GetLogicalProcessorInformation(info.data(), &bytes))
    return 0;
  unsigned cores = 0;
  for (const auto &item : info)
    cores += item.Relationship == RelationProcessorCore;
  return cores;
#elif defined(__APPLE__)
  int cores = 0;
  size_t size = sizeof(cores);
  if (sysctlbyname("hw.perflevel0.physicalcpu", &cores, &size, nullptr, 0) != 0 &&
      sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) != 0)
    return 0;
  return cores > 0 ? static_cast<unsigned>(cores) : 0;
#elif defined(__linux__)
  try {
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0)
      return 0;
    std::set<std::pair<int, int>> cores;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (!CPU_ISSET(cpu, &allowed))
        continue;
      const std::string dir =
          "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/topology/";
      int package = -1, core = -1;
      std::ifstream(dir + "physical_package_id") >> package;
      std::ifstream(dir + "core_id") >> core;
      if (core < 0)
        return static_cast<unsigned>(CPU_COUNT(&allowed)); // no topology
      cores.insert({package, core});
    }
    return static_cast<unsigned>(cores.size());
  } catch (...) {
    return 0;
  }
#else
  return std::thread::hardware_concurrency();
#endif
}

inline constexpr unsigned MATE_JOBS_MAX = 8;

// The threads of a parallel mate search when the caller passes 0
// (doc/parallel_search_design.md 第 5.1 節): CSPLENDOR_MATE_JOBS when set to
// a positive number, otherwise clamp(physical cores - 2, 1, 8) (the tree
// search and the network keep two cores; on Apple silicon all performance
// cores, the efficiency cores taking the rest). 1 without threads.
inline unsigned default_mate_jobs() noexcept {
#if !CSPLENDOR_MATE_THREADS
  return 1;
#else
  if (const char *env = std::getenv("CSPLENDOR_MATE_JOBS")) {
    const long value = std::strtol(env, nullptr, 10);
    if (value > 0)
      return static_cast<unsigned>(value);
  }
  unsigned cores = physical_core_count();
  if (cores == 0)
    cores = std::thread::hardware_concurrency();
#if defined(__APPLE__)
  const unsigned spare = 0;
#else
  const unsigned spare = 2;
#endif
  return std::clamp(cores > spare ? cores - spare : 1u, 1u, MATE_JOBS_MAX);
#endif
}

// 0 = default_mate_jobs(); always 1 without threads.
inline unsigned resolve_mate_jobs(unsigned jobs) noexcept {
#if !CSPLENDOR_MATE_THREADS
  (void)jobs;
  return 1;
#else
  return jobs == 0 ? default_mate_jobs() : jobs;
#endif
}

} // namespace csplendor

namespace csplendor::solver_internal {

// Below this depth a search runs on one thread (its subtrees are too small
// to pay for the split).
inline constexpr int PARALLEL_MATE_MIN_DEPTH = 3;
// The outcome index of an action's blank child (doc/mate_usage.md「blank
// 支配カット」): handed out once the first concrete outcome is proven, as the
// serial search tries it only then; proven, it proves the action.
inline constexpr int SPLIT_BLANK = -1;

// A node a worker expanded: decided before its moves (`decided`), or its
// moves in the serial search's order, each with the outcomes (reveals) the
// serial search would visit and whether it would try the blank cut after the
// first. `Move` and `Key` belong to the worker.
template <typename Move, typename Key> struct SplitExpansion {
  struct Branch {
    Move move;
    std::vector<int> outcomes;
    bool blank = false;
    bool complete = true; // false: the move could not be materialized
  };
  bool decided = false;
  ForceStatus status = ForceStatus::UNKNOWN;
  bool or_node = false; // the attacker moves: one proven move proves it
  Key key{};
  std::vector<Branch> branches;
};

// The scheduler of one parallel search over workers of type `Worker`, each
// owned by one thread (workers[0] by the caller):
//
//   using Node, Move, Key;                       // a node to resume from
//   void expand(const Node &, Expansion &);      // may throw SearchLimitExceeded
//   Node child(const Node &, const Expansion &, size_t branch, int outcome);
//   ForceStatus search(const Node &, const Expansion &, size_t branch,
//                      int outcome);            // may throw SearchLimitExceeded
//   void store(const Expansion &, ForceStatus, size_t branch, int outcome);
//   void set_cancellation_token(std::shared_ptr<RevealSearchCancellationToken>);
//   void attach(); void detach(); // around any use of the shared tables
//
// `outcome` is an index into the branch's outcomes or SPLIT_BLANK. Results
// combine as in the serial search: an action is the AND of its outcomes, an
// attacker node the OR and a defender node the AND of its actions. The first
// proof of an OR node and the first refutation of an AND node decide it and
// cancel its token, whose children are the tokens of all work below it.
//
// Work is handed out in the serial search's order, and a node's or an
// action's later children wait until its first child is resolved (the
// "young brothers wait" rule): the first child usually decides an OR node
// (proof) or an AND node (refutation), so searching its brothers at the same
// time is mostly wasted. A blank child waits until the first outcome is
// proven, as in the serial search.
template <typename Worker> class ParallelMateSearch {
public:
  using Node = typename Worker::Node;
  using Expansion = SplitExpansion<typename Worker::Move, typename Worker::Key>;

  struct Result {
    ForceStatus status = ForceStatus::UNKNOWN;
    bool decided = false;       // false: a search limit stopped it
    std::string unknown_reason; // the limit, when not decided
  };

  // Runs on workers[0] (this thread) and one new thread per further worker;
  // returns once every thread has finished. Exceptions other than search
  // limits are rethrown here.
  Result run(const std::vector<Worker *> &workers, const Node &root,
             std::shared_ptr<RevealSearchCancellationToken> user_token) {
    user_token_ = std::move(user_token);
    root_token_ = std::make_shared<Token>(user_token_);
    Worker &first = *workers.front();
    first.set_cancellation_token(root_token_);
    Attached attached(first);
    try {
      first.expand(root, root_expansion_);
    } catch (const SearchLimitExceeded &exc) {
      return Result{ForceStatus::UNKNOWN, false, exc.what()};
    }
    if (root_expansion_.decided)
      return Result{root_expansion_.status, true, {}};

    root_item_ = make_node_item(nullptr, 0, root_token_, &root, &root_expansion_);
    for (auto &action : root_item_->children)
      for (size_t j = 0; j < action->outcome_count; ++j) {
        action->frontiers.push_back(std::make_unique<Frontier>());
        action->frontiers.back()->token = std::make_shared<Token>(action->token);
      }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      report_incomplete(first, *root_item_);
    }
    attached.release();

#if CSPLENDOR_MATE_THREADS
    std::vector<std::thread> threads;
    struct Joiner {
      std::vector<std::thread> &threads;
      ParallelMateSearch &search;
      ~Joiner() {
        search.stop(); // a failed launch must not leave work behind
        for (std::thread &thread : threads)
          thread.join();
      }
    } joiner{threads, *this};
    threads.reserve(workers.size() - 1);
    for (size_t k = 1; k < workers.size(); ++k)
      threads.emplace_back([this, worker = workers[k]] { guarded_work(*worker); });
#endif
    guarded_work(first);
#if CSPLENDOR_MATE_THREADS
    for (std::thread &thread : threads)
      thread.join();
    threads.clear();
#endif
    if (failure_)
      std::rethrow_exception(failure_);
    if (root_item_->decided.load(std::memory_order_relaxed))
      return Result{root_item_->status, true, {}};
    if (limit_reason_.empty() && user_token_ && user_token_->is_cancelled())
      limit_reason_ = "search cancelled";
    if (limit_reason_.empty())
      throw std::logic_error("parallel mate search stopped undecided");
    return Result{ForceStatus::UNKNOWN, false, limit_reason_};
  }

private:
  using Token = RevealSearchCancellationToken;
  struct Frontier;

  // Keeps a worker attached to the shared tables for a scope.
  class Attached {
  public:
    explicit Attached(Worker &worker) : worker_(&worker) { worker.attach(); }
    ~Attached() { release(); }
    Attached(const Attached &) = delete;
    Attached &operator=(const Attached &) = delete;
    void release() {
      if (worker_ != nullptr)
        worker_->detach();
      worker_ = nullptr;
    }

  private:
    Worker *worker_;
  };

  // An AND/OR combination: a node over its actions or an action over its
  // outcomes. The atomics are written under mutex_ and read by the scan; the
  // plain fields after them change only under mutex_.
  struct Item {
    Item *parent = nullptr;
    int index = 0; // in the parent: the action's branch or the node's outcome
    bool node = false;
    bool or_node = false;
    std::shared_ptr<Token> token;
    const Node *at = nullptr;             // node items
    const Expansion *expansion = nullptr; // node items
    size_t branch = 0;                    // action items
    size_t outcome_count = 0;             // action items
    bool has_blank = false;               // action items
    std::vector<std::unique_ptr<Item>> children;      // node items: the actions
    std::vector<std::unique_ptr<Frontier>> frontiers; // root actions

    std::atomic<bool> decided{false};
    std::atomic<bool> first_resolved{false}; // child 0 came back
    std::atomic<bool> first_proven{false};   // actions: outcome 0 proven
    std::atomic<size_t> next_outcome{0};     // actions: claim cursor
    std::atomic<bool> blank_claimed{false};  // actions

    size_t remaining = 0;
    bool unknown = false;
    bool blank_proven = false;
    ForceStatus status = ForceStatus::UNKNOWN;
    int decided_outcome = 0; // actions: the refuting outcome, else 0
  };

  // A child of a root action's outcome, expanded by the worker that claims
  // it; its fields are written before `ready` is set.
  struct Frontier {
    std::shared_ptr<Token> token;
    std::atomic<bool> ready{false};
    Node node;
    Expansion expansion;
    std::unique_ptr<Item> item; // null when the expansion decided it
  };

  std::unique_ptr<Item> make_node_item(Item *parent, int index,
                                       std::shared_ptr<Token> token,
                                       const Node *at,
                                       const Expansion *expansion) {
    auto item = std::make_unique<Item>();
    item->parent = parent;
    item->index = index;
    item->node = true;
    item->or_node = expansion->or_node;
    item->token = std::move(token);
    item->at = at;
    item->expansion = expansion;
    item->remaining = expansion->branches.size();
    for (size_t b = 0; b < expansion->branches.size(); ++b) {
      const auto &branch = expansion->branches[b];
      auto action = std::make_unique<Item>();
      action->parent = item.get();
      action->index = static_cast<int>(b);
      action->token = std::make_shared<Token>(item->token);
      action->branch = b;
      action->outcome_count = branch.complete ? branch.outcomes.size() : 0;
      action->has_blank = branch.blank;
      action->remaining = action->outcome_count;
      item->children.push_back(std::move(action));
    }
    return item;
  }

  // Actions without outcomes are unknown (the serial search counts a move it
  // cannot materialize as unknown).
  void report_incomplete(Worker &worker, Item &node) {
    for (auto &action : node.children)
      if (action->outcome_count == 0)
        decide(worker, *action, ForceStatus::UNKNOWN, 0);
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    root_token_->request_cancel();
    changed_.notify_all();
  }

  void guarded_work(Worker &worker) noexcept {
    try {
      work(worker);
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!failure_)
          failure_ = std::current_exception();
      }
      stop();
    }
  }

  // Attached while it works, detached while it waits (so that it never
  // delays a growth of the shared tables).
  void work(Worker &worker) {
    while (!root_token_->is_cancelled()) {
      const uint64_t seen = generation_.load(std::memory_order_acquire);
      {
        Attached attached(worker);
        if (run_available(worker))
          continue;
      }
      std::unique_lock<std::mutex> lock(mutex_);
      if (running_ == 0 && generation_.load(std::memory_order_relaxed) == seen)
        return; // nothing runs and nothing is available: no work will appear
      // Bounded: the caller's token cancels without notifying.
      changed_.wait_for(lock, std::chrono::milliseconds(1), [&] {
        return generation_.load(std::memory_order_relaxed) != seen ||
               root_token_->is_cancelled();
      });
    }
  }

  // Claims and runs the first available work in the serial order.
  bool run_available(Worker &worker) {
    Item &root = *root_item_;
    for (size_t b = 0; b < root.children.size(); ++b) {
      if (b > 0 && !root.first_resolved.load(std::memory_order_acquire))
        return false;
      Item &action = *root.children[b];
      if (action.decided.load(std::memory_order_acquire))
        continue;
      if (claim_blank(action)) {
        run_leaf(worker, action, SPLIT_BLANK);
        return true;
      }
      for (size_t j = 0; j < action.frontiers.size(); ++j) {
        Frontier &frontier = *action.frontiers[j];
        if (!frontier.ready.load(std::memory_order_acquire) ||
            !frontier.item ||
            frontier.item->decided.load(std::memory_order_acquire))
          continue;
        if (run_available_leaf(worker, *frontier.item))
          return true;
      }
      size_t j = 0;
      if (claim_outcome(action, j)) {
        expand_frontier(worker, action, j);
        return true;
      }
    }
    return false;
  }

  bool run_available_leaf(Worker &worker, Item &node) {
    for (size_t b = 0; b < node.children.size(); ++b) {
      if (b > 0 && !node.first_resolved.load(std::memory_order_acquire))
        return false;
      Item &action = *node.children[b];
      if (action.decided.load(std::memory_order_acquire))
        continue;
      size_t j = 0;
      if (claim_blank(action)) {
        run_leaf(worker, action, SPLIT_BLANK);
        return true;
      }
      if (claim_outcome(action, j)) {
        run_leaf(worker, action, static_cast<int>(j));
        return true;
      }
    }
    return false;
  }

  // The next outcome of `action`: outcome 0 at once, the others once it is
  // resolved. Counts the claim as running.
  bool claim_outcome(Item &action, size_t &outcome) {
    size_t next = action.next_outcome.load(std::memory_order_relaxed);
    while (next < action.outcome_count) {
      if (next > 0 && !action.first_resolved.load(std::memory_order_acquire))
        return false;
      if (action.next_outcome.compare_exchange_weak(next, next + 1,
                                                    std::memory_order_relaxed)) {
        outcome = next;
        begin_work();
        return true;
      }
    }
    return false;
  }

  bool claim_blank(Item &action) {
    if (!action.has_blank ||
        !action.first_proven.load(std::memory_order_acquire) ||
        action.blank_claimed.load(std::memory_order_relaxed) ||
        action.blank_claimed.exchange(true, std::memory_order_relaxed))
      return false;
    begin_work();
    return true;
  }

  void begin_work() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++running_;
  }

  // Under mutex_: one claimed work finished (its results are reported).
  void end_work() {
    --running_;
    generation_.fetch_add(1, std::memory_order_release);
    changed_.notify_all();
  }

  void expand_frontier(Worker &worker, Item &action, size_t outcome) {
    Frontier &frontier = *action.frontiers[outcome];
    const Item &root = *action.parent;
    const int index = static_cast<int>(outcome);
    worker.set_cancellation_token(frontier.token);
    bool expanded = true;
    try {
      frontier.node =
          worker.child(*root.at, *root.expansion, action.branch, index);
      worker.expand(frontier.node, frontier.expansion);
    } catch (const SearchLimitExceeded &exc) {
      note_limit(exc);
      expanded = false;
    }
    if (expanded && !frontier.expansion.decided)
      frontier.item = make_node_item(&action, index, frontier.token,
                                     &frontier.node, &frontier.expansion);
    frontier.ready.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(mutex_);
    if (expanded && frontier.expansion.decided)
      report(worker, action, index, frontier.expansion.status);
    else if (frontier.item)
      report_incomplete(worker, *frontier.item);
    end_work();
  }

  void run_leaf(Worker &worker, Item &action, int outcome) {
    const Item &node = *action.parent;
    worker.set_cancellation_token(action.token);
    bool searched = !action.token->is_cancelled();
    ForceStatus status = ForceStatus::UNKNOWN;
    if (searched) {
      try {
        status = worker.search(*node.at, *node.expansion, action.branch,
                               outcome);
      } catch (const SearchLimitExceeded &exc) {
        note_limit(exc);
        searched = false;
      }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (searched)
      report(worker, action, outcome, status);
    end_work();
  }

  // A limit inside some work: a sibling's decision cancelled it (nothing to
  // do), or the search is out of nodes/time or cancelled by the caller (stop
  // every thread; the root stays undecided).
  void note_limit(const SearchLimitExceeded &exc) {
    const bool by_caller = user_token_ && user_token_->is_cancelled();
    const std::string reason = by_caller ? "search cancelled" : exc.what();
    if (!by_caller && reason == "search cancelled")
      return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (limit_reason_.empty())
        limit_reason_ = reason;
    }
    stop();
  }

  // Under mutex_: one child of `item` came back with `status`.
  void report(Worker &worker, Item &item, int index, ForceStatus status) {
    if (item.decided.load(std::memory_order_relaxed))
      return;
    if (index == 0)
      item.first_resolved.store(true, std::memory_order_release);
    if (!item.node) {
      if (index == SPLIT_BLANK) {
        item.blank_proven = status == ForceStatus::PROVEN;
        if (item.blank_proven)
          decide(worker, item, ForceStatus::PROVEN, 0); // outcome 0 is proven
        return;
      }
      if (status == ForceStatus::REFUTED)
        return decide(worker, item, ForceStatus::REFUTED, index);
      if (status == ForceStatus::UNKNOWN)
        item.unknown = true;
      if (index == 0 && status == ForceStatus::PROVEN)
        item.first_proven.store(true, std::memory_order_release);
      if (--item.remaining == 0)
        decide(worker, item,
               item.unknown ? ForceStatus::UNKNOWN : ForceStatus::PROVEN, 0);
      return;
    }
    const ForceStatus wins =
        item.or_node ? ForceStatus::PROVEN : ForceStatus::REFUTED;
    if (status == wins)
      return decide(worker, item, status, index);
    if (status == ForceStatus::UNKNOWN)
      item.unknown = true;
    if (--item.remaining == 0)
      decide(worker, item,
             item.unknown ? ForceStatus::UNKNOWN
                          : (item.or_node ? ForceStatus::REFUTED
                                          : ForceStatus::PROVEN),
             0);
  }

  // Under mutex_. A decided node stores its entry as the serial search would:
  // the deciding action (else the first) with the refuting outcome of a
  // defender's refutation, else the action's first outcome.
  void decide(Worker &worker, Item &item, ForceStatus status, int index) {
    item.status = status;
    item.decided.store(true, std::memory_order_release);
    item.token->request_cancel();
    if (item.node) {
      if (status != ForceStatus::UNKNOWN) {
        const Item &action = *item.children[static_cast<size_t>(index)];
        const int outcome =
            !item.or_node && status == ForceStatus::REFUTED
                ? action.decided_outcome
                : 0;
        worker.store(*item.expansion, status, action.branch, outcome);
      }
    } else {
      item.decided_outcome = status == ForceStatus::REFUTED ? index : 0;
    }
    if (item.parent != nullptr)
      report(worker, *item.parent, item.index, status);
    else
      changed_.notify_all(); // the root: every thread may stop
  }

  std::shared_ptr<Token> user_token_;
  std::shared_ptr<Token> root_token_;
  Expansion root_expansion_;
  std::unique_ptr<Item> root_item_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::atomic<uint64_t> generation_{0};
  size_t running_ = 0; // claimed work not finished (under mutex_)
  std::string limit_reason_;
  std::exception_ptr failure_;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_PARALLEL_MATE_SOLVER_H
