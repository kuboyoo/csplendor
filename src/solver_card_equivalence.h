#ifndef CSPLENDOR_SOLVER_CARD_EQUIVALENCE_H
#define CSPLENDOR_SOLVER_CARD_EQUIVALENCE_H

#include "card_data.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace csplendor::solver_internal {

#ifdef CSPLENDOR_CARD_EQUIVALENCE_CLASSES
inline const volatile bool card_equivalence_classes_enabled = true;
#else
inline const volatile bool card_equivalence_classes_enabled = false;
#endif

constexpr bool same_card_equivalence_tuple(const Card &left,
                                           const Card &right) noexcept {
  if (left.level != right.level || left.points != right.points ||
      left.bonus != right.bonus)
    return false;
  for (size_t color = 0; color < left.cost.size(); ++color) {
    if (left.cost[color] != right.cost[color])
      return false;
  }
  return true;
}

struct CardEquivalenceClassTable {
  std::array<uint8_t, CARD_COUNT> class_ids{};
  uint8_t class_count = 0;
};

static_assert(CARD_COUNT <= 128,
              "card equivalence mask requires at most two words");

constexpr CardEquivalenceClassTable make_card_equivalence_class_table() {
  CardEquivalenceClassTable table{};
  for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
    bool matched = false;
    for (int previous = 0; previous < card_id; ++previous) {
      if (same_card_equivalence_tuple(CARDS[card_id], CARDS[previous])) {
        table.class_ids[card_id] = table.class_ids[previous];
        matched = true;
        break;
      }
    }
    if (!matched)
      table.class_ids[card_id] = table.class_count++;
  }
  return table;
}

inline constexpr CardEquivalenceClassTable CARD_EQUIVALENCE_CLASSES =
    make_card_equivalence_class_table();

constexpr bool card_equivalence_classes_match_tuples() noexcept {
  for (int left = 0; left < CARD_COUNT; ++left) {
    for (int right = 0; right < CARD_COUNT; ++right) {
      const bool same_class = CARD_EQUIVALENCE_CLASSES.class_ids[left] ==
                              CARD_EQUIVALENCE_CLASSES.class_ids[right];
      if (same_class != same_card_equivalence_tuple(CARDS[left], CARDS[right]))
        return false;
    }
  }
  return true;
}

static_assert(CARD_EQUIVALENCE_CLASSES.class_count > 0);
static_assert(CARD_EQUIVALENCE_CLASSES.class_count <= 128,
              "card equivalence class count exceeds its mask");
static_assert(card_equivalence_classes_match_tuples(),
              "card equivalence classes must exactly match card tuples");

inline uint8_t card_equivalence_class(int card_id) {
  (void)get_card(card_id); // Preserve the legacy invalid-id exception.
  return CARD_EQUIVALENCE_CLASSES.class_ids[static_cast<size_t>(card_id)];
}

class CardEquivalenceMask {
public:
  bool insert(uint8_t class_id) noexcept {
    const size_t word = class_id >> 6;
    const uint64_t bit = uint64_t{1} << (class_id & 63U);
    const bool inserted = (words_[word] & bit) == 0;
    words_[word] |= bit;
    return inserted;
  }

private:
  std::array<uint64_t, 2> words_{};
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_CARD_EQUIVALENCE_H
