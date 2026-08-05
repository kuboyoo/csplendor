#ifndef CSPLENDOR_MCTS_PARALLEL_SESSION_H
#define CSPLENDOR_MCTS_PARALLEL_SESSION_H

#include "mcts_bounded_queue.h"
#include "mcts_concurrent_tree.h"
#include "mcts_parallel_trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mcts_parallel::detail {

struct SimulationTicket {
  SimulationTicket(uint64_t ticket_id, uint64_t generation)
      : simulation_id(ticket_id), tree_generation(generation) {}

  SimulationTicket(const SimulationTicket &) = delete;
  SimulationTicket &operator=(const SimulationTicket &) = delete;

  const uint64_t simulation_id;
  const uint64_t tree_generation;
  ReservedPath path;
  std::atomic<TicketState> state{TicketState::Created};
  CompletionKind completion = CompletionKind::None;
  std::atomic<uint64_t> pending_id{0};
  DeterministicTraceTicketData trace_data{};
};

using ActiveTicketRegistry =
    std::unordered_map<uint64_t, std::shared_ptr<SimulationTicket>>;

enum class WorkerEventKind : uint8_t {
  EvaluationOwner = 0,
  DirectCompletion = 1,
  Waiting = 2,
  Invalid = 3,
};

struct WorkerEvent {
  WorkerEventKind kind = WorkerEventKind::Invalid;
  std::shared_ptr<SimulationTicket> ticket;
  NodeHandle node;
  std::shared_ptr<PendingEvaluation> pending;
  Value value{};
  CompletionKind completion = CompletionKind::None;
};

class SessionRegistry final {
public:
  struct PendingEntry {
    NodeHandle node;
    std::shared_ptr<PendingEvaluation> pending;
  };

  SessionRegistry() = default;
  SessionRegistry(const SessionRegistry &) = delete;
  SessionRegistry &operator=(const SessionRegistry &) = delete;

  void register_pending(const NodeHandle &node,
                        const std::shared_ptr<PendingEvaluation> &pending) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto inserted =
        pending_by_id_.emplace(pending->id, PendingEntry{node, pending}).second;
    if (!inserted)
      throw std::logic_error("duplicate pending evaluation identifier");
  }

  void erase_pending(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_by_id_.erase(id);
  }

  template <typename Function>
  void drain_pending(Function &&function) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto &entry : pending_by_id_)
      function(entry.second);
    pending_by_id_.clear();
  }

private:
  std::mutex mutex_;
  std::unordered_map<uint64_t, PendingEntry> pending_by_id_;
};

class WorkerFailureState final {
public:
  WorkerFailureState() = default;
  WorkerFailureState(const WorkerFailureState &) = delete;
  WorkerFailureState &operator=(const WorkerFailureState &) = delete;

  void publish(std::exception_ptr value) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!error_)
      error_ = std::move(value);
  }

  std::exception_ptr snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

private:
  mutable std::mutex mutex_;
  std::exception_ptr error_;
};

// Owns every resource whose lifetime is scoped to one throughput search.
// Search semantics stay in ParallelMCTSSearcher; this controller guarantees
// that queues are closed and worker threads are joined on every exit path.
class ParallelSessionController final {
public:
  explicit ParallelSessionController(size_t queue_capacity)
      : ledger_(std::make_shared<SearchLedger>()), work_queue_(queue_capacity),
        event_queue_(queue_capacity) {}

  ParallelSessionController(const ParallelSessionController &) = delete;
  ParallelSessionController &
  operator=(const ParallelSessionController &) = delete;
  ParallelSessionController(ParallelSessionController &&) = delete;
  ParallelSessionController &operator=(ParallelSessionController &&) = delete;

  ~ParallelSessionController() noexcept {
    request_stop(true);
    join_workers_noexcept();
  }

  void reserve_workers(size_t count) { workers_.reserve(count); }

  template <typename Function> void start_worker(Function &&function) {
    workers_.emplace_back(std::forward<Function>(function));
  }

  void request_stop(bool close_event_queue) noexcept {
    stop_requested_.store(true, std::memory_order_release);
    work_queue_.close();
    if (close_event_queue)
      event_queue_.close();
  }

  void join_workers() {
    for (auto &worker : workers_) {
      if (worker.joinable())
        worker.join();
    }
    workers_.clear();
    event_queue_.close();
  }

  ActiveTicketRegistry &active_tickets() noexcept { return active_tickets_; }
  BoundedQueue<std::shared_ptr<SimulationTicket>> &work_queue() noexcept {
    return work_queue_;
  }
  BoundedQueue<WorkerEvent> &event_queue() noexcept { return event_queue_; }
  SessionRegistry &registry() noexcept { return registry_; }
  WorkerFailureState &worker_failure() noexcept { return worker_failure_; }
  std::atomic<bool> &stop_requested() noexcept { return stop_requested_; }
  const std::shared_ptr<SearchLedger> &ledger() const noexcept {
    return ledger_;
  }

private:
  void join_workers_noexcept() noexcept {
    try {
      join_workers();
    } catch (...) {
      // A joinable std::thread cannot be destroyed. Under the controller's
      // ownership rules join() is only called by the coordinator, so reaching
      // this path indicates an unrecoverable lifecycle violation.
      std::terminate();
    }
  }

  std::shared_ptr<SearchLedger> ledger_;
  ActiveTicketRegistry active_tickets_;
  std::vector<std::thread> workers_;
  BoundedQueue<std::shared_ptr<SimulationTicket>> work_queue_;
  BoundedQueue<WorkerEvent> event_queue_;
  SessionRegistry registry_;
  WorkerFailureState worker_failure_;
  std::atomic<bool> stop_requested_{false};
};

} // namespace mcts_parallel::detail

#endif // CSPLENDOR_MCTS_PARALLEL_SESSION_H
