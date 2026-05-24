#ifndef CSPLENDOR_RESOURCE_BUNDLE_H
#define CSPLENDOR_RESOURCE_BUNDLE_H

#include <cstdint>

namespace cli {

/**
 * Packs 5 gem colors into a 64-bit integer using 12 bits per color.
 * Colors are ordered: Emerald, Sapphire, Ruby, Diamond, Onyx.
 * 12 bits allows values up to 4095, which is more than enough for Splendor (max
 * 7-15). Using 12 bits instead of 8 bits prevents carries from affecting
 * adjacent fields during subtraction of up to 2048.
 */
struct ResourceBundle {
  uint64_t data = 0;

  static constexpr uint64_t MASK_FIELD = 0xFFFULL;
  static constexpr uint64_t MASK_ALL = 0x0FFFFFFFFFFFFFFFULL;
  static constexpr uint64_t BIT_11 = 0x800800800800800ULL;
  static constexpr uint64_t LOW_11 = 0x7FF7FF7FF7FF7FFULL;

  ResourceBundle() : data(0) {}
  explicit ResourceBundle(uint64_t d) : data(d) {}

  static ResourceBundle from_array(const uint8_t arr[5]) {
    uint64_t d = 0;
    for (int i = 0; i < 5; ++i) {
      d |= (uint64_t(arr[i]) << (i * 12));
    }
    return ResourceBundle(d);
  }

  void to_array(uint8_t arr[5]) const {
    for (int i = 0; i < 5; ++i) {
      arr[i] = (uint8_t)((data >> (i * 12)) & MASK_FIELD);
    }
  }

  // Returns the total gold needed to cover the cost given bonuses and gems.
  // Sum of max(0, cost - bonus - gems)
  static int needed_gold(uint64_t packed_cost, uint64_t packed_bonus,
                         uint64_t packed_gems) {
    // Offset by 0x800 (2048) to handle "negative" results without underflowing
    // 12 bits.
    uint64_t temp = (packed_cost + BIT_11) - packed_bonus - packed_gems;

    // If (cost - bonus - gems) > 0, the 11th bit of the field remains 1
    // (0x8xx). If (cost - bonus - gems) <= 0, the 11th bit becomes 0 (0x7xx or
    // less).

    // Create a mask for fields where bit 11 is set.
    uint64_t mask = temp & BIT_11;
    // Propagate the 11th bit to the lower 11 bits of each field.
    // A simple way is: (mask >> 11) * 0x7FF, but multiplication is slow.
    // We can use: ((mask >> 11) | (mask >> 10) | ... | (mask >> 1)) is also
    // complex. Actually, since our values are very small (< 15), we can just
    // use the property that if bit 11 is set, it's (0x800 + diff). If it's NOT
    // set, it's (< 0x800).

    // Horizontal sum fields that have bit 11 set.
    int total = 0;
    for (int i = 0; i < 5; ++i) {
      uint64_t field = (temp >> (i * 12)) & MASK_FIELD;
      if (field & 0x800) {
        total += (int)(field & 0x7FF);
      }
    }
    return total;
  }

  // Faster needed_gold using bit-manipulation if possible.
  // However, the loop above is only 5 iterations and very pipeline-friendly.
  // For now, let's keep it simple and correct.
};

} // namespace cli

#endif // CSPLENDOR_RESOURCE_BUNDLE_H
