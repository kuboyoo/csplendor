#ifndef CSPLENDOR_PORTABLE_RNG_H
#define CSPLENDOR_PORTABLE_RNG_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
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
    const uint64_t threshold = (uint64_t{0} - bound) % bound;
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

// Game(seed) historically used libstdc++'s std::shuffle on Linux. Its output
// is observable through deck order, snapshots, and seeded replay fixtures.
// This downscaling and paired shuffle preserve that sequence without depending
// on the host standard library, so libc++ and MSVC produce the same layout.
inline uint32_t portable_mt19937_bounded(std::mt19937 &rng, uint32_t bound) {
  if (bound == 0)
    throw std::invalid_argument("portable mt19937 bound must be positive");

  uint64_t product = static_cast<uint64_t>(rng()) * bound;
  uint32_t low = static_cast<uint32_t>(product);
  if (low < bound) {
    const uint32_t threshold = static_cast<uint32_t>(0U - bound) % bound;
    while (low < threshold) {
      product = static_cast<uint64_t>(rng()) * bound;
      low = static_cast<uint32_t>(product);
    }
  }
  return static_cast<uint32_t>(product >> 32);
}

template <typename RandomIt>
void portable_mt19937_shuffle(RandomIt first, RandomIt last,
                              std::mt19937 &rng) {
  const auto distance = last - first;
  if (distance <= 1)
    return;

  const std::size_t range = static_cast<std::size_t>(distance);
  constexpr std::size_t generator_range =
      std::numeric_limits<uint32_t>::max();
  if (range > generator_range)
    throw std::length_error("portable mt19937 shuffle range is too large");

  if (generator_range / range < range) {
    for (std::size_t index = 1; index < range; ++index) {
      using std::swap;
      swap(first[index],
           first[portable_mt19937_bounded(
               rng, static_cast<uint32_t>(index + 1))]);
    }
    return;
  }

  std::size_t index = 1;
  if ((range % 2) == 0) {
    using std::swap;
    swap(first[index], first[portable_mt19937_bounded(rng, 2)]);
    ++index;
  }

  while (index < range) {
    const std::size_t first_bound = index + 1;
    const std::size_t second_bound = first_bound + 1;
    const std::size_t combined_bound = first_bound * second_bound;
    const uint32_t combined = portable_mt19937_bounded(
        rng, static_cast<uint32_t>(combined_bound));
    using std::swap;
    swap(first[index], first[combined / second_bound]);
    ++index;
    swap(first[index], first[combined % second_bound]);
    ++index;
  }
}

#endif // CSPLENDOR_PORTABLE_RNG_H
