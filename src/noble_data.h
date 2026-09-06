#ifndef CSPLENDOR_NOBLE_DATA_H
#define CSPLENDOR_NOBLE_DATA_H

#include "types.h"
#include "resource_bundle.h"
#include <array>
#include <cstdint>
#include <limits>
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

constexpr bool noble_packed_requirements_match_arrays() {
  for (int i = 0; i < NOBLE_COUNT; ++i) {
    if (cli::ResourceBundle::pack(NOBLES[i].requirement) !=
        NOBLES[i].packed_requirement)
      return false;
  }
  return true;
}

static_assert(noble_packed_requirements_match_arrays(),
              "every noble packed_requirement must match its requirement array");

constexpr uint8_t max_noble_requirement() noexcept {
  uint8_t result = 0;
  for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
    for (int color = 0; color < 5; ++color) {
      if (NOBLES[noble_id].requirement[color] > result)
        result = NOBLES[noble_id].requirement[color];
    }
  }
  return result;
}

static constexpr uint8_t MAX_NOBLE_REQUIREMENT = max_noble_requirement();
using NobleMaskByColor =
    std::array<std::array<uint16_t, MAX_NOBLE_REQUIREMENT + 1>, 5>;

constexpr NobleMaskByColor make_noble_mask_by_color() noexcept {
  NobleMaskByColor table{};
  for (int color = 0; color < 5; ++color) {
    for (uint8_t bonus = 0; bonus <= MAX_NOBLE_REQUIREMENT; ++bonus) {
      uint16_t mask = 0;
      for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
        if (bonus >= NOBLES[noble_id].requirement[color])
          mask |= static_cast<uint16_t>(uint16_t{1} << noble_id);
      }
      table[color][bonus] = mask;
    }
  }
  return table;
}

static constexpr NobleMaskByColor NOBLE_MASK_BY_COLOR =
    make_noble_mask_by_color();

constexpr uint8_t clamp_noble_bonus(uint8_t bonus) noexcept {
  return bonus > MAX_NOBLE_REQUIREMENT ? MAX_NOBLE_REQUIREMENT : bonus;
}

constexpr uint16_t
noble_eligibility_mask_from_bonuses(
    const std::array<uint8_t, 5> &bonuses) noexcept {
  uint16_t mask = static_cast<uint16_t>((uint16_t{1} << NOBLE_COUNT) - 1U);
  for (int color = 0; color < 5; ++color) {
    mask &= NOBLE_MASK_BY_COLOR[color]
                                [clamp_noble_bonus(bonuses[color])];
  }
  return mask;
}

constexpr uint16_t
reference_noble_eligibility_mask(
    const std::array<uint8_t, 5> &bonuses) noexcept {
  uint16_t mask = 0;
  for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
    bool eligible = true;
    for (int color = 0; color < 5; ++color) {
      if (bonuses[color] < NOBLES[noble_id].requirement[color]) {
        eligible = false;
        break;
      }
    }
    if (eligible)
      mask |= static_cast<uint16_t>(uint16_t{1} << noble_id);
  }
  return mask;
}

constexpr bool noble_mask_entries_are_exact() noexcept {
  for (int color = 0; color < 5; ++color) {
    for (uint8_t bonus = 0; bonus <= MAX_NOBLE_REQUIREMENT; ++bonus) {
      for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
        const bool table_has_noble =
            (NOBLE_MASK_BY_COLOR[color][bonus] &
             static_cast<uint16_t>(uint16_t{1} << noble_id)) != 0;
        if (table_has_noble !=
            (bonus >= NOBLES[noble_id].requirement[color]))
          return false;
      }
    }
  }

  return true;
}

constexpr uint32_t NOBLE_MASK_BASE = MAX_NOBLE_REQUIREMENT + 1U;
constexpr uint32_t NOBLE_MASK_COMBINATIONS =
    NOBLE_MASK_BASE * NOBLE_MASK_BASE * NOBLE_MASK_BASE *
    NOBLE_MASK_BASE * NOBLE_MASK_BASE;

constexpr bool noble_mask_combinations_are_exact(uint32_t begin,
                                                uint32_t end) noexcept {
  if (begin > end || end > NOBLE_MASK_COMBINATIONS)
    return false;
  for (uint32_t encoded = begin; encoded < end; ++encoded) {
    uint32_t remaining = encoded;
    std::array<uint8_t, 5> bonuses{};
    for (int color = 0; color < 5; ++color) {
      bonuses[color] = static_cast<uint8_t>(remaining % NOBLE_MASK_BASE);
      remaining /= NOBLE_MASK_BASE;
    }
    if (noble_eligibility_mask_from_bonuses(bonuses) !=
        reference_noble_eligibility_mask(bonuses))
      return false;
  }

  return true;
}

constexpr bool noble_mask_out_of_range_is_exact() noexcept {
  for (int out_of_range_color = 0; out_of_range_color < 5;
       ++out_of_range_color) {
    std::array<uint8_t, 5> bonuses{};
    for (int color = 0; color < 5; ++color)
      bonuses[color] = MAX_NOBLE_REQUIREMENT;
    bonuses[out_of_range_color] = std::numeric_limits<uint8_t>::max();
    if (noble_eligibility_mask_from_bonuses(bonuses) !=
        reference_noble_eligibility_mask(bonuses))
      return false;
  }
  return true;
}

// Keep the full verifier available, but do not evaluate all combinations in
// one constant expression: Clang/AppleClang/MSVC have per-evaluation budgets.
constexpr bool noble_mask_table_is_exact() noexcept {
  return noble_mask_entries_are_exact() &&
         noble_mask_combinations_are_exact(0, NOBLE_MASK_COMBINATIONS) &&
         noble_mask_out_of_range_is_exact();
}

static_assert(MAX_NOBLE_REQUIREMENT == 4,
              "review the eligibility table range when noble data changes");
static_assert(noble_mask_entries_are_exact(),
              "every noble mask table entry must match the reference rules");
static_assert(noble_mask_out_of_range_is_exact(),
              "clamped noble masks must match the reference rules");

namespace noble_mask_verification_detail {
constexpr uint32_t CHUNK_SIZE = 25;
static_assert(NOBLE_MASK_COMBINATIONS == 3125, "review exhaustive coverage");
static_assert(NOBLE_MASK_COMBINATIONS % CHUNK_SIZE == 0,
              "verification chunks must cover the complete range");

// Instantiation recursively covers [0, End) with disjoint adjacent chunks.
// Each specialization has its OWN static_assert evaluation (25 tuples),
// including the last [3100, 3125); sizeof forces every base to instantiate.
template <uint32_t End>
struct Chunks : Chunks<End - CHUNK_SIZE> {
  static_assert(noble_mask_combinations_are_exact(End - CHUNK_SIZE, End),
                "noble eligibility masks must match the reference rules");
};
template <> struct Chunks<0> {};
static_assert(sizeof(Chunks<NOBLE_MASK_COMBINATIONS>) > 0,
              "instantiate all independent verification chunks");
} // namespace noble_mask_verification_detail

inline bool is_valid_noble_id(int id) { return id >= 0 && id < NOBLE_COUNT; }

inline const Noble &get_noble(int id) {
  if (!is_valid_noble_id(id))
    throw std::out_of_range("noble id out of range");
  return NOBLES[id];
}

#endif // CSPLENDOR_NOBLE_DATA_H
