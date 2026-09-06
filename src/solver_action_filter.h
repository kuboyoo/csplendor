#ifndef CSPLENDOR_SOLVER_ACTION_FILTER_H
#define CSPLENDOR_SOLVER_ACTION_FILTER_H

#include "action.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
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

  const auto take_base_less = [](const ScoredTake &left, const ScoredTake &right) {
    if (left.first != right.first)
      return left.first > right.first;
    return left.second < right.second;
  };
  // Instantiate a capturing comparator only for the hint-enabled path.
  // Keep the original comparison operations shared by both specializations.
  const auto take_less = [&] {
    if constexpr (PreferHint) {
      return [preferred_code, take_base_less](const ScoredTake &left, const ScoredTake &right) {
        const bool left_hint = left.second.code == preferred_code;
        const bool right_hint = right.second.code == preferred_code;
        if (left_hint != right_hint)
          return left_hint;
        return take_base_less(left, right);
      };
    } else {
      return take_base_less;
    }
  }();
  const auto reserve_base_less = [](const OrderedAction &left, const OrderedAction &right) {
    return left < right;
  };
  const auto reserve_less = [&] {
    if constexpr (PreferHint) {
      return [preferred_code, reserve_base_less](const OrderedAction &left, const OrderedAction &right) {
        const bool left_hint = left.code == preferred_code;
        const bool right_hint = right.code == preferred_code;
        if (left_hint != right_hint)
          return left_hint;
        return reserve_base_less(left, right);
      };
    } else {
      return reserve_base_less;
    }
  }();
  if constexpr (!PreferHint) {
    static_assert(std::is_empty_v<decltype(take_less)>, "no-hint comparator must not capture");
    static_assert(std::is_empty_v<decltype(reserve_less)>, "no-hint comparator must not capture");
  }

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
