#ifndef CSPLENDOR_SOLVER_LINE_TABLE_H
#define CSPLENDOR_SOLVER_LINE_TABLE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace csplendor::solver_internal {

// Bounded table of the proof transplantation (doc/mate_usage.md「移植の表の
// 上限」): attacker-line hash -> action code, 16-byte slots, linear probing
// confined to a window of WINDOW slots from the home slot. The capacity
// doubles from MIN_SLOTS at load 0.5 up to max_slots (a power of two); at the
// cap a key whose window is full overwrites its home slot. The table only
// orders moves, so a lost entry changes node counts, never a verdict.
class LineTable {
public:
  struct Slot {
    uint64_t key = 0; // 0 = free
    uint64_t code = 0;
  };

  static constexpr size_t MIN_SLOTS = size_t{1} << 16;
  static constexpr size_t WINDOW = 8;

  // Rounded down to a power of two, at least MIN_SLOTS.
  void set_max_slots(size_t slots) noexcept {
    max_slots_ = MIN_SLOTS;
    while (max_slots_ <= slots / 2)
      max_slots_ *= 2;
  }

  size_t size() const noexcept { return count_; }
  size_t capacity() const noexcept { return slots_.size(); }

  void clear() noexcept {
    slots_.clear();
    count_ = 0;
  }

  const Slot *find(uint64_t key) const noexcept {
    if (slots_.empty())
      return nullptr;
    key = stored(key);
    const size_t mask = slots_.size() - 1;
    for (size_t w = 0, i = home(key, mask); w < WINDOW; ++w, i = (i + 1) & mask) {
      if (slots_[i].key == key)
        return &slots_[i];
      if (slots_[i].key == 0)
        return nullptr;
    }
    return nullptr;
  }

  void put(uint64_t key, uint64_t code) {
    key = stored(key);
    if (slots_.empty())
      slots_.assign(MIN_SLOTS < max_slots_ ? MIN_SLOTS : max_slots_, Slot{});
    else if ((count_ + 1) * 2 > slots_.size() && can_grow())
      grow();
    while (!place(key, code)) {
      if (!can_grow()) {
        // At the cap: the newest entry takes the home slot.
        slots_[home(key, slots_.size() - 1)] = Slot{key, code};
        return;
      }
      grow();
    }
  }

private:
  static uint64_t stored(uint64_t key) noexcept { return key == 0 ? 1 : key; }

  static size_t home(uint64_t key, size_t mask) noexcept {
    return static_cast<size_t>(key * 0x9E3779B97F4A7C15ull >> 20) & mask;
  }

  bool can_grow() const noexcept { return slots_.size() < max_slots_; }

  // Stores into the window (updating an equal key); false when it is full.
  bool place(uint64_t key, uint64_t code) noexcept {
    const size_t mask = slots_.size() - 1;
    for (size_t w = 0, i = home(key, mask); w < WINDOW; ++w, i = (i + 1) & mask) {
      Slot &slot = slots_[i];
      if (slot.key == key) {
        slot.code = code;
        return true;
      }
      if (slot.key == 0) {
        slot = Slot{key, code};
        ++count_;
        return true;
      }
    }
    return false;
  }

  void grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(old.size() * 2, Slot{});
    count_ = 0;
    for (const Slot &slot : old)
      if (slot.key != 0 && !place(slot.key, slot.code))
        slots_[home(slot.key, slots_.size() - 1)] = slot; // only orders moves
  }

  std::vector<Slot> slots_;
  size_t count_ = 0;
  size_t max_slots_ = MIN_SLOTS;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_LINE_TABLE_H
