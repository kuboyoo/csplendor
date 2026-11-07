#ifndef CSPLENDOR_SOLVER_FLAT_TABLE_H
#define CSPLENDOR_SOLVER_FLAT_TABLE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace csplendor::solver_internal {

// Open-addressing transposition table with linear probing
// (doc/mate_usage.md「置換表の開番地法化」). One slot is one key and one entry,
// 64 bytes for the solver's keys, so a probe costs one cache line and no
// allocation. `Key::empty()` marks a free slot (true for a default key), so
// no slot carries an occupancy byte and clear() is one fill.
//
// The capacity is any size: the index is the high 32 bits of mix(hash)
// scaled to the capacity. With an entry limit L (the flush cap) the capacity
// never exceeds C = capacity_for(L) = ceil(L / MAX_LOAD) and grows along
// C >> k for k >= 3, then jumps to C: the table holds L entries without a
// doubling past the cap, and the one rehash that copies a large table (old
// and new arrays alive together) starts from at most C / 8. Without a limit
// it doubles from MIN_CAPACITY.
template <typename Key, typename Entry, typename Hash> class FlatMemoTable {
public:
  using key_type = Key;
  using mapped_type = Entry;

  struct alignas(64) Slot {
    Key key;
    Entry entry;
  };

  static constexpr size_t MIN_CAPACITY = 1024;
  // Maximum load factor LOAD_NUM / LOAD_DEN.
  static constexpr size_t LOAD_NUM = 7;
  static constexpr size_t LOAD_DEN = 10;

  static constexpr size_t capacity_for(size_t entries) noexcept {
    const size_t raw = (entries * LOAD_DEN + LOAD_NUM - 1) / LOAD_NUM;
    return raw < MIN_CAPACITY ? MIN_CAPACITY : raw;
  }

  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  size_t capacity() const noexcept { return slots_.size(); }
  size_t memory_bytes() const noexcept { return slots_.size() * sizeof(Slot); }

  // 0 = unbounded. Only shapes the growth; the caller flushes at the limit.
  void set_entry_limit(size_t entries) noexcept { limit_ = entries; }

  Entry *find(const Key &key) noexcept {
    if (slots_.empty())
      return nullptr;
    for (size_t i = home(key);; i = next(i)) {
      Slot &slot = slots_[i];
      if (slot.key.empty())
        return nullptr;
      if (slot.key == key)
        return &slot.entry;
    }
  }

  const Entry *find(const Key &key) const noexcept {
    return const_cast<FlatMemoTable *>(this)->find(key);
  }

  // `key` must be absent.
  Entry &insert(const Key &key, const Entry &entry) {
    if ((size_ + 1) * LOAD_DEN > slots_.size() * LOAD_NUM)
      rehash(aligned_capacity(slots_.size() + 1));
    return place(key, entry);
  }

  void clear() noexcept {
    if (size_ != 0)
      std::fill(slots_.begin(), slots_.end(), Slot{});
    size_ = 0;
  }

  // Removes every entry with pred(key, entry) true; backward-shift deletion
  // keeps the probe chains intact. An element moved into a scanned slot was
  // already tested there (shifts only move entries toward their home).
  template <typename Pred> void erase_if(Pred pred) {
    for (size_t i = 0; i < slots_.size(); ++i) {
      while (!slots_[i].key.empty() && pred(slots_[i].key, slots_[i].entry))
        erase_at(i);
    }
  }

  template <typename Fn> void for_each(Fn fn) const {
    for (const Slot &slot : slots_)
      if (!slot.key.empty())
        fn(slot.key, slot.entry);
  }

private:
  static uint64_t mix(uint64_t h) noexcept {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
  }

  // The high 32 bits of the mixed hash scaled to the capacity (which stays
  // far below 2^32): a range reduction that needs no power-of-two capacity.
  size_t home(const Key &key) const noexcept {
    const uint64_t h = mix(static_cast<uint64_t>(Hash{}(key)));
    return static_cast<size_t>(((h >> 32) * slots_.size()) >> 32);
  }

  size_t next(size_t i) const noexcept {
    return ++i == slots_.size() ? 0 : i;
  }

  // The capacity sequence: C = capacity_for(limit) and C >> k (k >= 3) when
  // limited, powers of two from MIN_CAPACITY otherwise; the smallest member
  // not below `want`.
  size_t aligned_capacity(size_t want) const noexcept {
    if (limit_ == 0) {
      size_t capacity = MIN_CAPACITY;
      while (capacity < want)
        capacity *= 2;
      return capacity;
    }
    size_t capacity = capacity_for(limit_);
    if (capacity < want)
      return want; // the limit was lowered below the live table
    if (want > (capacity >> 3) || (capacity >> 3) < MIN_CAPACITY)
      return capacity;
    capacity >>= 3;
    while ((capacity >> 1) >= want && (capacity >> 1) >= MIN_CAPACITY)
      capacity >>= 1;
    return capacity;
  }

  Entry &place(const Key &key, const Entry &entry) noexcept {
    size_t i = home(key);
    while (!slots_[i].key.empty())
      i = next(i);
    slots_[i].key = key;
    slots_[i].entry = entry;
    ++size_;
    return slots_[i].entry;
  }

  void rehash(size_t new_capacity) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.resize(new_capacity);
    size_ = 0;
    for (const Slot &slot : old)
      if (!slot.key.empty())
        place(slot.key, slot.entry);
  }

  void erase_at(size_t hole) noexcept {
    for (size_t k = next(hole);; k = next(k)) {
      if (slots_[k].key.empty())
        break;
      const size_t h = home(slots_[k].key);
      // The element at k may move to `hole` unless its home lies cyclically in
      // (hole, k].
      const bool home_between =
          hole <= k ? (h > hole && h <= k) : (h > hole || h <= k);
      if (!home_between) {
        slots_[hole] = slots_[k];
        hole = k;
      }
    }
    slots_[hole] = Slot{};
    --size_;
  }

  std::vector<Slot> slots_;
  size_t size_ = 0;
  size_t limit_ = 0;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_FLAT_TABLE_H
