#ifndef CSPLENDOR_FIXED_STACK_H
#define CSPLENDOR_FIXED_STACK_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

// Fixed-capacity contiguous storage used by Board decks and noble lists.
// data/count remain public for source and layout compatibility with existing
// C++ consumers. New internal code should use try_push_back() when capacity is
// data-dependent, or push_back_unchecked() after proving capacity locally.
template <typename T, std::size_t MaxSize> struct FixedStack {
  static_assert(MaxSize <= std::numeric_limits<uint8_t>::max(),
                "FixedStack count cannot represent MaxSize");

  std::array<T, MaxSize> data;
  uint8_t count = 0;

  static constexpr std::size_t capacity() noexcept { return MaxSize; }

  void clear() noexcept { count = 0; }
  bool empty() const noexcept { return count == 0; }
  bool full() const noexcept { return count >= MaxSize; }
  std::size_t size() const noexcept { return count; }

  bool try_push_back(T value) noexcept(
      noexcept(std::declval<T &>() = std::move(value))) {
    if (full())
      return false;
    data[count] = std::move(value);
    ++count;
    return true;
  }

  void push_back_unchecked(T value) noexcept(
      noexcept(std::declval<T &>() = std::move(value))) {
    data[count] = std::move(value);
    ++count;
  }

  // Compatibility wrapper: historically overflow was silently ignored.
  void push_back(T value) noexcept(noexcept(try_push_back(std::move(value)))) {
    static_cast<void>(try_push_back(std::move(value)));
  }

  bool try_pop_back() noexcept {
    if (empty())
      return false;
    --count;
    return true;
  }

  // Compatibility wrapper: historically popping an empty stack was a no-op.
  void pop_back() noexcept { static_cast<void>(try_pop_back()); }

  T back() const { return data[count - 1]; }
  T &back() { return data[count - 1]; }

  T *begin() noexcept { return data.data(); }
  T *end() noexcept { return data.data() + count; }
  const T *begin() const noexcept { return data.data(); }
  const T *end() const noexcept { return data.data() + count; }

  T &operator[](std::size_t index) { return data[index]; }
  const T &operator[](std::size_t index) const { return data[index]; }

  void erase(T *iterator) {
    if (iterator >= begin() && iterator < end()) {
      std::move(iterator + 1, end(), iterator);
      --count;
    }
  }

  void remove(T value) {
    auto iterator = std::find(begin(), end(), value);
    if (iterator != end())
      erase(iterator);
  }
};

#endif // CSPLENDOR_FIXED_STACK_H
