#ifndef CSPLENDOR_VISIBLE_ONLY_SOLVER_H
#define CSPLENDOR_VISIBLE_ONLY_SOLVER_H

#include "game.h"
#include "solver_types.h"
#include <cstdint>
#include <memory>

class VisibleOnlySolver {
public:
  VisibleOnlySolver(uint64_t max_nodes, double time_limit_seconds);
  ~VisibleOnlySolver();

  VisibleOnlySolver(const VisibleOnlySolver &other);
  VisibleOnlySolver &operator=(const VisibleOnlySolver &other);
  VisibleOnlySolver(VisibleOnlySolver &&other) noexcept;
  VisibleOnlySolver &operator=(VisibleOnlySolver &&other) noexcept;

  VisibleOnlySearchResult solve(const Game &input);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

#endif // CSPLENDOR_VISIBLE_ONLY_SOLVER_H
