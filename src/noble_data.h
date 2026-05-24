#ifndef CSPLENDOR_NOBLE_DATA_H
#define CSPLENDOR_NOBLE_DATA_H

#include "types.h"
#include <stdexcept>

static constexpr int NOBLE_COUNT = 12;

// All 12 nobles in Splendor
// Requirement order: [White (D), Blue (S), Green (E), Red (R), Black (O)]
constexpr Noble NOBLES[NOBLE_COUNT] = {
    {0, 3, {0, 0, 4, 4, 0}, 274945015808ULL},  // Green, Red
    {1, 3, {0, 0, 0, 4, 4}, 1126174784749568ULL},  // Red, Black
    {2, 3, {0, 4, 4, 0, 0}, 67125248ULL},  // Blue, Green
    {3, 3, {4, 0, 0, 0, 4}, 1125899906842628ULL},  // White, Black
    {4, 3, {4, 4, 0, 0, 0}, 16388ULL},  // White, Blue
    {5, 3, {4, 0, 0, 4, 0}, 274877906948ULL},  // White, Red
    {6, 3, {3, 0, 0, 3, 3}, 844631088562179ULL},  // White, Red, Black
    {7, 3, {3, 3, 3, 0, 0}, 50343939ULL},  // White, Blue, Green
    {8, 3, {0, 0, 3, 3, 3}, 844631138893824ULL},  // Green, Red, Black
    {9, 3, {0, 3, 3, 3, 0}, 206208774144ULL},  // Blue, Green, Red
    {10, 3, {3, 3, 0, 0, 3}, 844424930144259ULL}, // White, Blue, Black
    {11, 3, {0, 3, 3, 0, 3}, 844424980475904ULL}  // Blue, Green, Black
};

inline bool is_valid_noble_id(int id) { return id >= 0 && id < NOBLE_COUNT; }

inline const Noble &get_noble(int id) {
  if (!is_valid_noble_id(id))
    throw std::out_of_range("noble id out of range");
  return NOBLES[id];
}

#endif // CSPLENDOR_NOBLE_DATA_H
