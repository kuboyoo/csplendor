#ifndef CSPLENDOR_PORTABLE_RNG_H
#define CSPLENDOR_PORTABLE_RNG_H

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

// Repository-owned SplitMix64 stream.  Unlike std::shuffle/distributions, its
// output is fixed across standard-library implementations.
class PortableRng {
public:
  explicit PortableRng(uint64_t seed) noexcept : state_(seed) {}

  uint64_t next_u64() noexcept {
    uint64_t value = (state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
  }

  uint64_t uniform_bounded(uint64_t bound) {
    if (bound == 0)
      throw std::invalid_argument("portable RNG bound must be positive");
    const uint64_t threshold = static_cast<uint64_t>(-bound) % bound;
    for (;;) {
      const uint64_t value = next_u64();
      if (value >= threshold)
        return value % bound;
    }
  }

private:
  uint64_t state_;
};

template <typename RandomIt>
void portable_shuffle(RandomIt first, RandomIt last, PortableRng &rng) {
  const auto distance = last - first;
  if (distance <= 1)
    return;
  for (std::size_t remaining = static_cast<std::size_t>(distance);
       remaining > 1; --remaining) {
    const std::size_t other =
        static_cast<std::size_t>(rng.uniform_bounded(remaining));
    using std::swap;
    swap(first[remaining - 1], first[other]);
  }
}

#endif // CSPLENDOR_PORTABLE_RNG_H
