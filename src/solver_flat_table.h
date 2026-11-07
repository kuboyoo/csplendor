#ifndef CSPLENDOR_SOLVER_FLAT_TABLE_H
#define CSPLENDOR_SOLVER_FLAT_TABLE_H

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <new>
#include <vector>

namespace csplendor::solver_internal {

// The home slot of a hash: the high 32 bits of the mixed hash scaled to the
// capacity (which stays far below 2^32), a range reduction that needs no
// power-of-two capacity.
inline size_t flat_table_home(uint64_t h, size_t capacity) noexcept {
  h ^= h >> 30;
  h *= 0xbf58476d1ce4e5b9ull;
  h ^= h >> 27;
  h *= 0x94d049bb133111ebull;
  h ^= h >> 31;
  return static_cast<size_t>(((h >> 32) * capacity) >> 32);
}

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
  using serial_table = FlatMemoTable;
  static constexpr bool concurrent = false;

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
  size_t home(const Key &key) const noexcept {
    return flat_table_home(static_cast<uint64_t>(Hash{}(key)), slots_.size());
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

// Growth of the tables shared by the threads of a parallel search
// (doc/mate_usage.md「並列の詰み探索」). A table starts small and asks for a
// larger array when it fills up; the threads using the tables move the
// entries together once every one of them is at a safepoint, where it holds
// no pointer into a table. A thread is attached while it may touch the
// tables (searching, or reporting a result) and calls safepoint() often
// (every node); a detached thread never delays a growth.
class ConcurrentTableGrowth {
public:
  class Table {
  public:
    // Under the growth: allocate the larger array (false: no growth wanted),
    // move part `part` of `parts` of the entries (threads run the parts at
    // once), and switch to the larger array.
    virtual bool prepare_growth() = 0;
    virtual void migrate(size_t part, size_t parts) noexcept = 0;
    virtual void finish_growth() noexcept = 0;

  protected:
    ~Table() = default;
  };

  void add(Table &table) { tables_.push_back(&table); }
  void request() noexcept { requested_.store(true, std::memory_order_relaxed); }

  void attach() {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return !migrating_; });
    ++attached_;
  }

  void detach() {
    std::unique_lock<std::mutex> lock(mutex_);
    --attached_;
    if (requested_.load(std::memory_order_relaxed) && !migrating_ &&
        arrived_ > 0 && arrived_ == attached_)
      start();
  }

  void safepoint() {
    if (!requested_.load(std::memory_order_relaxed))
      return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (!requested_.load(std::memory_order_relaxed))
      return; // another thread's growth just ended
    const uint64_t generation = generation_;
    ++arrived_;
    if (arrived_ == attached_)
      start();
    changed_.wait(lock,
                  [&] { return migrating_ || generation_ != generation; });
    if (generation_ != generation)
      return;
    const size_t part = next_part_++;
    const size_t parts = parts_;
    lock.unlock();
    for (Table *table : tables_)
      table->migrate(part, parts);
    lock.lock();
    if (++finished_ == parts_) {
      for (Table *table : tables_)
        table->finish_growth();
      migrating_ = false;
      end();
    } else {
      changed_.wait(lock, [&] { return generation_ != generation; });
    }
  }

private:
  // Under mutex_, every attached thread at a safepoint (arrived_ of them).
  void start() {
    requested_.store(false, std::memory_order_relaxed);
    bool grows = false;
    for (Table *table : tables_)
      grows = table->prepare_growth() || grows;
    if (!grows)
      return end();
    migrating_ = true;
    parts_ = arrived_;
    next_part_ = 0;
    finished_ = 0;
    changed_.notify_all();
  }

  void end() {
    arrived_ = 0;
    ++generation_;
    changed_.notify_all();
  }

  std::vector<Table *> tables_;
  std::atomic<bool> requested_{false};
  std::mutex mutex_;
  std::condition_variable changed_;
  size_t attached_ = 0;
  size_t arrived_ = 0;
  bool migrating_ = false;
  size_t parts_ = 0;
  size_t next_part_ = 0;
  size_t finished_ = 0;
  uint64_t generation_ = 0;
};

// The table shared by the threads of one parallel mate search. Same slots,
// keys, entries and probing as FlatMemoTable; every slot also has a state
// byte (in an array beside the slots):
//
//   EMPTY --CAS--> WRITING --release--> READY
//
// A writer claims an EMPTY slot with a CAS, writes the key and the entry, then
// publishes READY. A reader takes a READY slot (acquire) and compares its key;
// it passes over a WRITING slot, whose key it cannot read yet (a miss at
// worst). Entries never change once READY. Two threads storing the same key
// at once may both succeed into different slots; find() returns the first,
// and both hold the same decided value (only decided entries are stored).
//
// The arrays start at INITIAL_CAPACITY slots, allocated zeroed so that pages
// are committed only when written, and double (through `growth`) once 60%
// full, up to capacity_for(max_entries). The table never flushes or
// erases: an insert beyond max_entries entries is dropped, which only costs
// re-searching that node. A pointer from find() stays valid until the
// caller's next safepoint.
template <typename Key, typename Entry, typename Hash>
class ConcurrentFlatMemoTable final : public ConcurrentTableGrowth::Table {
public:
  using key_type = Key;
  using mapped_type = Entry;
  using serial_table = FlatMemoTable<Key, Entry, Hash>;
  using Slot = typename serial_table::Slot;
  static constexpr bool concurrent = true;
  static constexpr size_t INITIAL_CAPACITY = size_t{1} << 15;

  // Without `growth` the arrays have their final size from the start.
  ConcurrentFlatMemoTable(size_t max_entries,
                          ConcurrentTableGrowth *growth = nullptr)
      : limit_(std::max<size_t>(1, max_entries)),
        final_capacity_(serial_table::capacity_for(limit_)), growth_(growth) {
    current_.store(new Array(growth_ ? std::min(final_capacity_, INITIAL_CAPACITY)
                                     : final_capacity_),
                   std::memory_order_relaxed);
    if (growth_)
      growth_->add(*this);
  }

  ~ConcurrentFlatMemoTable() {
    delete current_.load(std::memory_order_relaxed);
    delete next_;
  }

  ConcurrentFlatMemoTable(const ConcurrentFlatMemoTable &) = delete;
  ConcurrentFlatMemoTable &operator=(const ConcurrentFlatMemoTable &) = delete;

  size_t size() const noexcept { return size_.load(std::memory_order_relaxed); }
  size_t capacity() const noexcept {
    return current_.load(std::memory_order_acquire)->capacity;
  }
  size_t entry_limit() const noexcept { return limit_; }
  size_t memory_bytes() const noexcept { return capacity() * (sizeof(Slot) + 1); }
  // Inserts refused because the table held entry_limit() entries.
  uint64_t dropped() const noexcept {
    return dropped_.load(std::memory_order_relaxed);
  }

  const Entry *find(const Key &key) const noexcept {
    const Array &array = *current_.load(std::memory_order_acquire);
    size_t i = flat_table_home(static_cast<uint64_t>(Hash{}(key)), array.capacity);
    for (size_t probes = 0; probes < array.capacity; ++probes, i = array.next(i)) {
      const uint8_t state = array.states[i].load(std::memory_order_acquire);
      if (state == EMPTY)
        return nullptr;
      if (state == READY && array.slots[i].key == key)
        return &array.slots[i].entry;
    }
    return nullptr;
  }

  // False when `key` is already READY or the table is full (dropped).
  bool insert(const Key &key, const Entry &entry) noexcept {
    if (size_.load(std::memory_order_relaxed) >= limit_) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    Array &array = *current_.load(std::memory_order_acquire);
    if (!place(array, key, entry)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const size_t size = size_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (growth_ && array.capacity < final_capacity_ &&
        size > grow_at(array.capacity))
      growth_->request();
    return true;
  }

  // Only while no thread inserts.
  template <typename Fn> void for_each(Fn fn) const {
    const Array &array = *current_.load(std::memory_order_acquire);
    for (size_t i = 0; i < array.capacity; ++i)
      if (array.states[i].load(std::memory_order_acquire) == READY)
        fn(array.slots[i].key, array.slots[i].entry);
  }

  bool prepare_growth() override {
    const Array &array = *current_.load(std::memory_order_relaxed);
    if (array.capacity >= final_capacity_ ||
        size_.load(std::memory_order_relaxed) <= grow_at(array.capacity))
      return false;
    next_ = new Array(std::min(final_capacity_, array.capacity * 2));
    return true;
  }

  void migrate(size_t part, size_t parts) noexcept override {
    if (next_ == nullptr)
      return;
    const Array &array = *current_.load(std::memory_order_relaxed);
    const size_t begin = array.capacity * part / parts;
    const size_t end = array.capacity * (part + 1) / parts;
    for (size_t i = begin; i < end; ++i)
      if (array.states[i].load(std::memory_order_relaxed) == READY)
        place(*next_, array.slots[i].key, array.slots[i].entry);
  }

  void finish_growth() noexcept override {
    if (next_ == nullptr)
      return;
    delete current_.exchange(next_, std::memory_order_release);
    next_ = nullptr;
  }

private:
  static constexpr uint8_t EMPTY = 0;
  static constexpr uint8_t WRITING = 1;
  static constexpr uint8_t READY = 2;

  // A growth doubles the array once it holds more than 0.6 x its slots.
  static constexpr size_t grow_at(size_t capacity) noexcept {
    return capacity / 10 * 6;
  }

  struct Array {
    explicit Array(size_t slot_count) : capacity(slot_count) {
      // One extra slot to align the slots to a cache line; calloc leaves the
      // pages uncommitted until first written.
      raw_slots = std::calloc(capacity + 1, sizeof(Slot));
      states = static_cast<std::atomic<uint8_t> *>(
          std::calloc(capacity, sizeof(std::atomic<uint8_t>)));
      if (raw_slots == nullptr || states == nullptr) {
        std::free(raw_slots);
        std::free(states);
        throw std::bad_alloc();
      }
      const uintptr_t address = reinterpret_cast<uintptr_t>(raw_slots);
      slots = reinterpret_cast<Slot *>((address + alignof(Slot) - 1) &
                                       ~uintptr_t{alignof(Slot) - 1});
    }
    ~Array() {
      std::free(raw_slots);
      std::free(states);
    }
    Array(const Array &) = delete;
    Array &operator=(const Array &) = delete;
    size_t next(size_t i) const noexcept { return ++i == capacity ? 0 : i; }

    size_t capacity;
    void *raw_slots = nullptr;
    Slot *slots = nullptr;
    std::atomic<uint8_t> *states = nullptr;
  };
  static_assert(sizeof(std::atomic<uint8_t>) == 1 &&
                    std::atomic<uint8_t>::is_always_lock_free,
                "slot states are single lock-free bytes (zero = EMPTY)");

  // Claims a slot for `key` in `array`; false when `key` is READY there or
  // no slot is free.
  static bool place(Array &array, const Key &key, const Entry &entry) noexcept {
    size_t i = flat_table_home(static_cast<uint64_t>(Hash{}(key)), array.capacity);
    for (size_t probes = 0; probes < array.capacity;) {
      uint8_t state = array.states[i].load(std::memory_order_acquire);
      if (state == EMPTY &&
          array.states[i].compare_exchange_strong(state, WRITING,
                                                  std::memory_order_acquire)) {
        array.slots[i].key = key;
        array.slots[i].entry = entry;
        array.states[i].store(READY, std::memory_order_release);
        return true;
      }
      if (state == EMPTY)
        continue; // lost the CAS: look at the same slot again
      if (state == READY && array.slots[i].key == key)
        return false;
      ++probes;
      i = array.next(i);
    }
    return false;
  }

  size_t limit_;
  size_t final_capacity_;
  ConcurrentTableGrowth *growth_;
  std::atomic<Array *> current_{nullptr};
  Array *next_ = nullptr; // during a growth
  std::atomic<size_t> size_{0};
  std::atomic<uint64_t> dropped_{0};
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_FLAT_TABLE_H
