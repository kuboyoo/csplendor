#ifndef CSPLENDOR_REPETITION_WINDOW_H
#define CSPLENDOR_REPETITION_WINDOW_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// The repetition window (Board::repetition_keys): repetition_position_key()
// of each position since the last irreversible decision. Nearly all windows
// are short, so up to INLINE_CAPACITY keys live inside the object and copying
// a Board does not allocate; longer windows move to the heap. The keys are
// always contiguous (begin()/end()).
class RepetitionWindow {
public:
  static constexpr std::size_t INLINE_CAPACITY = 8;

  RepetitionWindow() = default;
  RepetitionWindow(const uint64_t *first, const uint64_t *last) {
    assign(first, last);
  }

  std::size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }
  const uint64_t *begin() const noexcept { return data(); }
  const uint64_t *end() const noexcept { return data() + size_; }
  uint64_t back() const noexcept { return data()[size_ - 1]; }
  uint64_t operator[](std::size_t index) const noexcept { return data()[index]; }

  // Whether holding `size` keys needs no allocation (rollback paths).
  bool fits_without_allocation(std::size_t size) const noexcept {
    return size <= INLINE_CAPACITY || heap_.capacity() >= size;
  }
  void reserve(std::size_t size) {
    if (size > INLINE_CAPACITY)
      heap_.reserve(size);
  }

  void clear() noexcept {
    size_ = 0;
    heap_.clear();
  }

  void push_back(uint64_t key) {
    if (size_ < INLINE_CAPACITY) {
      inline_[size_++] = key;
      return;
    }
    if (size_ == INLINE_CAPACITY)
      heap_.assign(inline_.begin(), inline_.end());
    heap_.push_back(key);
    ++size_;
  }

  void assign(const uint64_t *first, const uint64_t *last) {
    const auto size = static_cast<std::size_t>(last - first);
    if (size <= INLINE_CAPACITY) {
      std::copy(first, last, inline_.begin());
      heap_.clear();
    } else {
      heap_.assign(first, last);
    }
    size_ = size;
  }

  std::vector<uint64_t> to_vector() const { return {begin(), end()}; }

  friend bool operator==(const RepetitionWindow &lhs,
                         const RepetitionWindow &rhs) noexcept {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
  }
  friend bool operator!=(const RepetitionWindow &lhs,
                         const RepetitionWindow &rhs) noexcept {
    return !(lhs == rhs);
  }

private:
  const uint64_t *data() const noexcept {
    return size_ > INLINE_CAPACITY ? heap_.data() : inline_.data();
  }

  std::array<uint64_t, INLINE_CAPACITY> inline_{};
  std::size_t size_ = 0;
  std::vector<uint64_t> heap_; // all keys while size_ > INLINE_CAPACITY
};

#endif // CSPLENDOR_REPETITION_WINDOW_H
