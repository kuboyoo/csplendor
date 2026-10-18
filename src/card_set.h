#ifndef CSPLENDOR_CARD_SET_H
#define CSPLENDOR_CARD_SET_H

#include "card_data.h"
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

// A set of card IDs as two 64-bit words. It keeps no insertion order and
// iterates in ascending ID order. Out-of-range IDs are ignored by add/remove
// and are never contained.
struct CardSet {
  static_assert(CARD_COUNT <= 128, "CardSet holds at most 128 card IDs");

  uint64_t low = 0;
  uint64_t high = 0;

  CardSet() = default;
  // Brace lists name card IDs (`cards = {3, 17}`), never the raw words.
  CardSet(std::initializer_list<int> card_ids) noexcept {
    for (int card_id : card_ids)
      add(card_id);
  }

  static bool valid(int card_id) noexcept {
    return card_id >= 0 && card_id < CARD_COUNT;
  }

  void clear() noexcept {
    low = 0;
    high = 0;
  }
  bool empty() const noexcept { return (low | high) == 0; }
  std::size_t size() const noexcept { return popcount(low) + popcount(high); }

  void add(int card_id) noexcept {
    if (valid(card_id))
      word(card_id) |= bit(card_id);
  }
  void remove(int card_id) noexcept {
    if (valid(card_id))
      word(card_id) &= ~bit(card_id);
  }
  bool contains(int card_id) const noexcept {
    return valid(card_id) && (word(card_id) & bit(card_id)) != 0;
  }

  // Cards in this set and not in `other`.
  CardSet minus(const CardSet &other) const noexcept {
    CardSet result;
    result.low = low & ~other.low;
    result.high = high & ~other.high;
    return result;
  }

  bool operator==(const CardSet &other) const noexcept {
    return low == other.low && high == other.high;
  }
  bool operator!=(const CardSet &other) const noexcept {
    return !(*this == other);
  }

  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = uint8_t;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = uint8_t;

    iterator(uint64_t low, uint64_t high) noexcept : low_(low), high_(high) {}
    uint8_t operator*() const noexcept {
      return low_ != 0 ? static_cast<uint8_t>(lowest(low_))
                       : static_cast<uint8_t>(64 + lowest(high_));
    }
    iterator &operator++() noexcept {
      if (low_ != 0)
        low_ &= low_ - 1;
      else
        high_ &= high_ - 1;
      return *this;
    }
    iterator operator++(int) noexcept {
      iterator previous = *this;
      ++*this;
      return previous;
    }
    bool operator==(const iterator &other) const noexcept {
      return low_ == other.low_ && high_ == other.high_;
    }
    bool operator!=(const iterator &other) const noexcept {
      return !(*this == other);
    }

  private:
    uint64_t low_;
    uint64_t high_;
  };

  iterator begin() const noexcept { return iterator(low, high); }
  iterator end() const noexcept { return iterator(0, 0); }

private:
  uint64_t &word(int card_id) noexcept { return card_id < 64 ? low : high; }
  uint64_t word(int card_id) const noexcept {
    return card_id < 64 ? low : high;
  }
  static uint64_t bit(int card_id) noexcept {
    return uint64_t{1} << (card_id & 63);
  }

  static std::size_t popcount(uint64_t bits) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    return static_cast<std::size_t>(__popcnt64(bits));
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<std::size_t>(__builtin_popcountll(bits));
#else
    std::size_t count = 0;
    for (; bits != 0; bits &= bits - 1)
      ++count;
    return count;
#endif
  }

  // Index of the lowest set bit; `bits` must be nonzero.
  static unsigned lowest(uint64_t bits) noexcept {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long index = 0;
    _BitScanForward64(&index, bits);
    return static_cast<unsigned>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return static_cast<unsigned>(__builtin_ctzll(bits));
#else
    unsigned index = 0;
    while ((bits & (uint64_t{1} << index)) == 0)
      ++index;
    return index;
#endif
  }
};

#endif // CSPLENDOR_CARD_SET_H
