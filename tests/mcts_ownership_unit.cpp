#include "mcts.h"
#include "mcts_config_validator.h"
#include "mcts_parallel_searcher.h"
#include "mcts_parallel_session.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace {

using mcts_internal::MCTSConfigValidator;
using mcts_parallel::detail::ParallelSessionController;
using mcts_parallel::detail::SimulationTicket;

void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}

template <typename Function>
void require_invalid(Function &&function, const char *message) {
  try {
    function();
  } catch (const std::invalid_argument &) {
    return;
  }
  throw std::runtime_error(message);
}

void test_owner_types_are_unique() {
  static_assert(!std::is_copy_constructible<MCTS>::value,
                "MCTS must uniquely own its sequential and parallel trees");
  static_assert(!std::is_move_constructible<MCTS>::value,
                "moving a live tree owner would invalidate search handles");
  static_assert(!std::is_copy_constructible<ParallelSessionController>::value,
                "a session must uniquely own its worker threads");
  static_assert(!std::is_move_constructible<ParallelSessionController>::value,
                "moving queues while workers reference them is unsafe");
}

void test_parallel_config_validator_boundaries() {
  MCTSConfig config;
  MCTSConfigValidator::validate_parallel(config);

  config.cpuct = std::numeric_limits<float>::quiet_NaN();
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "non-finite cpuct was accepted");
  config = MCTSConfig{};
  config.fpu = std::numeric_limits<float>::infinity();
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "non-finite FPU was accepted");
  config = MCTSConfig{};
  config.forced_playouts_k = -0.1f;
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "negative forced-playout coefficient was accepted");
  config = MCTSConfig{};
  config.num_simulations = -1;
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "negative simulation count was accepted");
  config = MCTSConfig{};
  config.dirichlet_epsilon = 1.01f;
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "out-of-range Dirichlet mixing was accepted");
  config = MCTSConfig{};
  config.dirichlet_alpha = 0.0f;
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "non-positive active Dirichlet alpha was accepted");
  config.use_dirichlet_noise = false;
  MCTSConfigValidator::validate_parallel(config);
  config = MCTSConfig{};
  config.num_determinizations = 2;
  require_invalid([&] { MCTSConfigValidator::validate_parallel(config); },
                  "multiple hidden worlds were accepted");
  config.use_determinization = false;
  MCTSConfigValidator::validate_parallel(config);
}

void test_session_closes_queues_and_joins_workers() {
  ParallelSessionController session(4);
  session.reserve_workers(3);
  std::atomic<int> processed{0};
  for (int worker = 0; worker < 3; ++worker) {
    session.start_worker([&] {
      std::shared_ptr<SimulationTicket> ticket;
      while (session.work_queue().pop(ticket))
        processed.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (uint64_t id = 0; id < 4; ++id) {
    require(
        session.work_queue().push(std::make_shared<SimulationTicket>(id, 0)),
        "open session queue rejected work");
  }
  session.request_stop(false);
  session.join_workers();
  require(processed.load(std::memory_order_relaxed) == 4,
          "session did not drain queued work before joining");
  require(session.work_queue().closed() && session.event_queue().closed(),
          "session left a queue open after joining workers");
}

void test_session_destructor_is_cleanup_fallback() {
  std::atomic<int> started{0};
  std::atomic<int> stopped{0};
  std::shared_ptr<mcts_parallel::SearchLedger> retained_ledger;
  {
    ParallelSessionController session(2);
    retained_ledger = session.ledger();
    session.reserve_workers(2);
    for (int worker = 0; worker < 2; ++worker) {
      session.start_worker([&] {
        started.fetch_add(1, std::memory_order_release);
        while (!session.stop_requested().load(std::memory_order_acquire))
          std::this_thread::yield();
        stopped.fetch_add(1, std::memory_order_release);
      });
    }
    while (started.load(std::memory_order_acquire) != 2)
      std::this_thread::yield();
    retained_ledger->issued.store(7, std::memory_order_relaxed);
  }
  require(stopped.load(std::memory_order_acquire) == 2,
          "session destructor left a worker running");
  require(retained_ledger->issued.load(std::memory_order_relaxed) == 7,
          "worker-shared ledger lifetime was not independent of controller");
}

} // namespace

int main() {
  try {
    test_owner_types_are_unique();
    test_parallel_config_validator_boundaries();
    test_session_closes_queues_and_joins_workers();
    test_session_destructor_is_cleanup_fallback();
  } catch (const std::exception &error) {
    std::cerr << "mcts ownership test failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
