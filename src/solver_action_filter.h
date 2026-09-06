#ifndef CSPLENDOR_SOLVER_ACTION_FILTER_H
#define CSPLENDOR_SOLVER_ACTION_FILTER_H

#include "action.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace csplendor::solver_internal {

#ifdef CSPLENDOR_COMPACT_FORCED_ACTIONS
inline const volatile bool compact_forced_actions_enabled = true;
#else
inline const volatile bool compact_forced_actions_enabled = false;
#endif

template <typename Value, size_t Capacity, typename Less>
void insert_bounded_best(std::array<Value, Capacity> &values, size_t &size,
                         Value candidate, Less less) {
  size_t position = 0;
  while (position < size && !less(candidate, values[position]))
    ++position;
  if (position >= Capacity)
    return;

  const size_t new_size = size < Capacity ? size + 1 : size;
  for (size_t index = new_size - 1; index > position; --index)
    values[index] = std::move(values[index - 1]);
  values[position] = std::move(candidate);
  size = new_size;
}

template <size_t TakeLimit, size_t ReserveLimit, bool PreferHint = false,
          typename OrderedAction,
          typename TakeScore>
std::vector<OrderedAction>
compact_forced_attacker_actions(const std::vector<OrderedAction> &actions,
                                TakeScore take_score,
                                uint64_t preferred_code = UINT64_MAX) {
  static_assert(TakeLimit > 0 && ReserveLimit > 0);
  using ScoredTake = std::pair<int, OrderedAction>;
  std::array<ScoredTake, TakeLimit> takes{};
  std::array<OrderedAction, ReserveLimit> reserves{};
  size_t take_count = 0;
  size_t reserve_count = 0;

  const auto take_less = [preferred_code](const ScoredTake &left,
                                        const ScoredTake &right) {
    // Match the main/reference path's rotate-before-truncate semantics.
    // The no-hint instantiation retains the original comparison operations.
    if constexpr (PreferHint) {
      const bool left_hint = left.second.code == preferred_code;
      const bool right_hint = right.second.code == preferred_code;
      if (left_hint != right_hint)
        return left_hint;
    }
    if (left.first != right.first)
      return left.first > right.first;
    return left.second < right.second;
  };
  const auto reserve_less = [preferred_code](const OrderedAction &left,
                                           const OrderedAction &right) {
    if constexpr (PreferHint) {
      const bool left_hint = left.code == preferred_code;
      const bool right_hint = right.code == preferred_code;
      if (left_hint != right_hint)
        return left_hint;
    }
    return left < right;
  };

  std::vector<OrderedAction> filtered;
  filtered.reserve(actions.size());
  for (const OrderedAction &ordered : actions) {
    const Action action = Action::unpack(ordered.code);
    switch (action.type) {
    case PURCHASE:
    case VISIT_NOBLE:
      filtered.push_back(ordered);
      break;
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      insert_bounded_best(takes, take_count,
                          ScoredTake{take_score(action), ordered}, take_less);
      break;
    case RESERVE_VISIBLE:
      insert_bounded_best(reserves, reserve_count, ordered, reserve_less);
      break;
    default:
      break;
    }
  }

  for (size_t index = 0; index < take_count; ++index)
    filtered.push_back(takes[index].second);
  for (size_t index = 0; index < reserve_count; ++index)
    filtered.push_back(reserves[index]);
  for (const OrderedAction &ordered : actions) {
    const ActionType type = Action::unpack(ordered.code).type;
    if (type != PURCHASE && type != VISIT_NOBLE && type != TAKE_DIFFERENT &&
        type != TAKE_SAME && type != RESERVE_VISIBLE)
      filtered.push_back(ordered);
  }
  return filtered;
}

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_ACTION_FILTER_H
