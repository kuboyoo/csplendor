#ifndef CSPLENDOR_SOLVER_PATH_H
#define CSPLENDOR_SOLVER_PATH_H

#include "perf_counters.h"

#include <cstddef>
#include <unordered_set>
#include <vector>

namespace csplendor::solver_internal {

// Keep OFF/ON benchmark builds code-identical so the experiment changes only
// the selected path container.  The legacy set remains available as a precise
// oracle and rollback switch.
#ifdef CSPLENDOR_SOLVER_PATH_STACK
inline const volatile bool solver_path_stack_enabled = true;
#else
inline const volatile bool solver_path_stack_enabled = false;
#endif

inline void record_solver_path_depth(size_t depth) noexcept {
  CSPLENDOR_PERF_INC(SolverPathDepthSamples);
  CSPLENDOR_PERF_ADD(SolverPathDepthSum, depth);
  CSPLENDOR_PERF_MAX(SolverPathDepthMax, depth);
  if (depth == 0)
    CSPLENDOR_PERF_INC(SolverPathDepth0);
  else if (depth <= 2)
    CSPLENDOR_PERF_INC(SolverPathDepth1To2);
  else if (depth <= 4)
    CSPLENDOR_PERF_INC(SolverPathDepth3To4);
  else if (depth <= 8)
    CSPLENDOR_PERF_INC(SolverPathDepth5To8);
  else
    CSPLENDOR_PERF_INC(SolverPathDepth9Plus);
}

template <typename Key, typename Hash> class RecursionPath {
public:
  using key_type = Key;

  explicit RecursionPath(size_t reserve_capacity = 32,
                         bool bounded_depth = true)
      : use_stack_(solver_path_stack_enabled && bounded_depth) {
    if (use_stack_) {
      CSPLENDOR_PERF_INC(SolverTemporaryVectorAllocations);
      stack_.reserve(reserve_capacity);
    } else {
      CSPLENDOR_PERF_INC(SolverTemporarySetAllocations);
    }
  }

  bool contains(const Key &key) const {
    CSPLENDOR_PERF_INC(SolverPathFinds);
    record_solver_path_depth(size());
    if (!use_stack_)
      return set_.find(key) != set_.end();

#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    size_t comparisons = 0;
#endif
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      ++comparisons;
#endif
      if (*it == key) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
        CSPLENDOR_PERF_ADD(SolverPathLinearComparisons, comparisons);
#endif
        return true;
      }
    }
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    CSPLENDOR_PERF_ADD(SolverPathLinearComparisons, comparisons);
#endif
    return false;
  }

  bool empty() const noexcept {
    return use_stack_ ? stack_.empty() : set_.empty();
  }

  size_t size() const noexcept {
    return use_stack_ ? stack_.size() : set_.size();
  }

  void push(const Key &key) {
    CSPLENDOR_PERF_INC(SolverPathInserts);
    if (use_stack_)
      stack_.push_back(key);
    else
      set_.insert(key);
  }

  void pop(const Key &key) noexcept {
    CSPLENDOR_PERF_INC(SolverPathErases);
    if (use_stack_)
      stack_.pop_back();
    else
      set_.erase(key);
  }

private:
  bool use_stack_ = false;
  std::vector<Key> stack_;
  std::unordered_set<Key, Hash> set_;
};

template <typename Path> class ScopedPathEntry {
public:
  ScopedPathEntry(Path &path, const typename Path::key_type &key)
      : path_(path), key_(key) {
    path_.push(key_);
  }
  ScopedPathEntry(Path &, typename Path::key_type &&) = delete;

  ScopedPathEntry(const ScopedPathEntry &) = delete;
  ScopedPathEntry &operator=(const ScopedPathEntry &) = delete;

  ~ScopedPathEntry() { path_.pop(key_); }

private:
  Path &path_;
  const typename Path::key_type &key_;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_PATH_H
