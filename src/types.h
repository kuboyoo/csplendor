#ifndef CSPLENDOR_TYPES_H
#define CSPLENDOR_TYPES_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

enum GemType : uint8_t {
  DIAMOND = 0,  // White
  SAPPHIRE = 1, // Blue
  EMERALD = 2,  // Green
  RUBY = 3,     // Red
  ONYX = 4,     // Black
  GOLD = 5,     // Wild
  NUM_GEM_COLORS = 5,
  NUM_GEM_TYPES = 6
};

struct Card {
  uint8_t id;                  // 0-89
  uint8_t level;               // 1-3
  uint8_t points;              // 0-5
  GemType bonus;               // Bonus color
  std::array<uint8_t, 5> cost; // [White, Blue, Green, Red, Black]
  uint64_t packed_cost;        // Packed 5x12 bits
};

struct Noble {
  uint8_t id;                         // 0-9
  uint8_t points;                     // Always 3
  std::array<uint8_t, 5> requirement; // [White, Blue, Green, Red, Black]
  uint64_t packed_requirement;        // Packed 5x12 bits
};

enum ActionType : uint8_t {
  TAKE_DIFFERENT = 0,
  TAKE_SAME = 1,
  RESERVE_VISIBLE = 2,
  RESERVE_DECK = 3,
  PURCHASE = 4,
  VISIT_NOBLE = 5,
  ACTION_TYPE_COUNT = 6
};

#endif // CSPLENDOR_TYPES_H
