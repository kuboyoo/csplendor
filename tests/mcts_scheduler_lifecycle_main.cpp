#include "mcts_parallel_scheduler_suites.h"
#include "support/native_test.h"

int main() {
  return csplendor::test::run_suite(
      "mcts_scheduler_lifecycle", run_mcts_scheduler_lifecycle_suite);
}
