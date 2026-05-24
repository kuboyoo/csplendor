#ifndef CSPLENDOR_ACTION_H
#define CSPLENDOR_ACTION_H

#include "types.h"
#include <algorithm>
#include <array> // Added for std::array
#include <sstream>
#include <string>

struct Action {
  ActionType type = ACTION_TYPE_COUNT; // Initialized to an invalid type

  // For TAKE_DIFFERENT, TAKE_SAME
  std::array<uint8_t, 5> take = {0}; // [E, S, R, D, O]

  // For RESERVE_VISIBLE, RESERVE_DECK, PURCHASE
  int8_t card_id = -1;        // Card ID
  int8_t deck_level = -1;     // 0, 1, 2
  bool from_reserved = false; // Purchasing from reserved cards

  // For PURCHASE: How gold is used for each color
  std::array<uint8_t, 5> gold_as = {0};

  // For token return (when exceeding 10 gems)
  std::array<uint8_t, 6> return_gems = {0}; // [E, S, R, D, O, G]

  // For noble choice (if multiple are eligible)
  int8_t noble_choice = -1; // ID of the noble chosen

  bool operator==(const Action &other) const {
    if (type != other.type)
      return false;
    for (int i = 0; i < 5; ++i)
      if (take[i] != other.take[i])
        return false;
    if (card_id != other.card_id)
      return false;
    if (deck_level != other.deck_level)
      return false;
    if (from_reserved != other.from_reserved)
      return false;
    for (int i = 0; i < 5; ++i)
      if (gold_as[i] != other.gold_as[i])
        return false;
    for (int i = 0; i < 6; ++i)
      if (return_gems[i] != other.return_gems[i])
        return false;
    if (noble_choice != other.noble_choice)
      return false;
    return true;
  }

  std::string to_string() const {
    std::stringstream ss;
    switch (type) {
    case TAKE_DIFFERENT:
      ss << "TAKE_DIFFERENT: ";
      for (int i = 0; i < 5; ++i)
        if (take[i])
          ss << (int)take[i] << "x" << i << " ";
      break;
    case TAKE_SAME:
      ss << "TAKE_SAME: ";
      for (int i = 0; i < 5; ++i)
        if (take[i])
          ss << (int)take[i] << "x" << i << " ";
      break;
    case RESERVE_VISIBLE:
      ss << "RESERVE_VISIBLE: C" << (int)card_id;
      break;
    case RESERVE_DECK:
      ss << "RESERVE_DECK: L" << (int)deck_level + 1;
      break;
    case PURCHASE:
      ss << "PURCHASE: C" << (int)card_id << (from_reserved ? " (R)" : "");
      break;
    }

    bool has_return = false;
    for (int i = 0; i < 6; ++i)
      if (return_gems[i])
        has_return = true;
    if (has_return) {
      ss << " RETURN: ";
      for (int i = 0; i < 6; ++i)
        if (return_gems[i])
          ss << (int)return_gems[i] << "x" << i << " ";
    }

    if (noble_choice != -1) {
      ss << " NOBLE: N" << (int)noble_choice;
    }

    return ss.str();
  }
};

// Fixed-size move list for MoveGenerator - avoids heap allocations
static constexpr size_t MAX_MOVES = 2048;

struct MoveList {
  std::array<Action, MAX_MOVES> data;
  uint16_t count = 0;

  void clear() { count = 0; }
  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  void push_back(const Action &a) {
    if (count < MAX_MOVES)
      data[count++] = a;
  }

  Action &operator[](size_t i) { return data[i]; }
  const Action &operator[](size_t i) const { return data[i]; }

  Action *begin() { return data.data(); }
  Action *end() { return data.data() + count; }
  const Action *begin() const { return data.data(); }
  const Action *end() const { return data.data() + count; }
};

#endif // CSPLENDOR_ACTION_H
