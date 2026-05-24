#ifndef CSPLENDOR_PLAYER_H
#define CSPLENDOR_PLAYER_H

#include "card_data.h"
#include "noble_data.h"
#include "resource_bundle.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <vector>

struct PlayerState {
  std::array<uint8_t, 6> gems = {
      0};                   // Emerald, Sapphire, Ruby, Diamond, Onyx, Gold
  uint64_t packed_gems = 0; // Packed 5 colors (excluding Gold)
  std::array<uint8_t, 5> bonuses = {0}; // Bonuses from purchased cards
  uint64_t packed_bonuses = 0;
  uint8_t points = 0;
  std::array<int8_t, 3> reserved = {-1, -1, -1}; // IDs of reserved cards
  std::array<bool, 3> reserved_is_hidden = {false, false,
                                            false}; // Hidden from others
  uint8_t reserved_count = 0;
  uint8_t purchased_count = 0;
  std::vector<uint8_t> purchased_cards;
  std::vector<uint8_t> acquired_nobles; // IDs of acquired nobles

  // Noble eligibility bitmask - bit i is set if player can visit noble i
  uint16_t noble_eligibility_mask = 0;

  void sync_packed() {
    uint64_t g = 0;
    uint64_t b = 0;
    for (int i = 0; i < 5; ++i) {
      g |= (uint64_t(gems[i]) << (i * 12));
      b |= (uint64_t(bonuses[i]) << (i * 12));
    }
    packed_gems = g;
    packed_bonuses = b;

    // Update noble eligibility mask
    update_noble_eligibility();
  }

  void update_noble_eligibility() {
    noble_eligibility_mask = 0;
    for (int noble_id = 0; noble_id < NOBLE_COUNT; ++noble_id) {
      const Noble &noble = get_noble(noble_id);
      if (cli::ResourceBundle::needed_gold(noble.packed_requirement,
                                           packed_bonuses, 0) == 0) {
        noble_eligibility_mask |= (uint16_t(1) << noble_id);
      }
    }
  }

  int total_gems() const {
    int sum = 0;
    for (int i = 0; i < 6; ++i)
      sum += gems[i];
    return sum;
  }

  bool can_reserve() const { return reserved_count < 3; }

  bool can_afford(const Card &card,
                  std::array<uint8_t, 5> *gold_usage = nullptr) const {
    int gold_needed = cli::ResourceBundle::needed_gold(
        card.packed_cost, packed_bonuses, packed_gems);

    if (gold_needed <= gems[GOLD]) {
      // Calculate gold usage if needed for legacy compatibility
      if (gold_usage) {
        for (int i = 0; i < 5; ++i) {
          int cost = std::max(0, (int)card.cost[i] - (int)bonuses[i]);
          if (gems[i] < cost) {
            (*gold_usage)[i] = cost - gems[i];
          } else {
            (*gold_usage)[i] = 0;
          }
        }
      }
      return true;
    }
    return false;
  }
};

#endif // CSPLENDOR_PLAYER_H
