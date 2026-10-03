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
// Bit-exact std::mt19937 whose seeding and twisting are performed lazily.
// A seeded std::mt19937 initialises all 624 state words and twists the whole
// block before its first output, although a deck shuffle consumes only a few
// dozen values.  Output k is a function of words k, k+1 and k+397 of the
// in-place twisted state, so the generator initialises seed words only up to
// the furthest word the next output reads and twists one word per output.
// The produced sequence is identical to std::mt19937 for every seed and
// output position (verified against the standard engine in the unit tests).
class LazyMt19937 {
public:
  using result_type = uint32_t;

  explicit LazyMt19937(uint32_t seed) noexcept {
    state_[0] = seed;
    seeded_ = 1;
  }

  static constexpr result_type min() noexcept { return 0; }
  static constexpr result_type max() noexcept { return 0xffffffffU; }

  result_type operator()() noexcept {
    const std::size_t index = index_;
    const std::size_t next = index + 1 == kStateSize ? 0 : index + 1;
    const std::size_t shifted = index + kShift >= kStateSize
                                    ? index + kShift - kStateSize
                                    : index + kShift;
    // Before the first full block completes, output k reads seed word k+397
    // (k < 227) or, once indices wrap, already-twisted words plus the tail of
    // the seed block.  Initialise exactly the prefix that can be read.
    const std::size_t needed =
        index < kStateSize - kShift ? index + kShift : kStateSize - 1;
    if (seeded_ <= needed)
      seed_through(needed);

    const uint32_t mixed =
        (state_[index] & kUpperMask) | (state_[next] & kLowerMask);
    uint32_t value =
        state_[shifted] ^ (mixed >> 1) ^ ((mixed & 1U) ? kMatrix : 0U);
    state_[index] = value;
    index_ = next;

    value ^= value >> 11;
    value ^= (value << 7) & 0x9d2c5680U;
    value ^= (value << 15) & 0xefc60000U;
    value ^= value >> 18;
    return value;
  }

private:
  static constexpr std::size_t kStateSize = 624;
  static constexpr std::size_t kShift = 397;
  static constexpr uint32_t kMatrix = 0x9908b0dfU;
  static constexpr uint32_t kUpperMask = 0x80000000U;
  static constexpr uint32_t kLowerMask = 0x7fffffffU;

  void seed_through(std::size_t last) noexcept {
    for (std::size_t i = seeded_; i <= last; ++i) {
      const uint32_t previous = state_[i - 1];
      state_[i] = 1812433253U * (previous ^ (previous >> 30)) +
                  static_cast<uint32_t>(i);
    }
    seeded_ = last + 1;
  }

  uint32_t state_[kStateSize];
  std::size_t seeded_ = 0;
  std::size_t index_ = 0;
};

template <typename Mt19937>
inline uint32_t portable_mt19937_bounded(Mt19937 &rng, uint32_t bound) {
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

template <typename RandomIt, typename Mt19937>
void portable_mt19937_shuffle(RandomIt first, RandomIt last, Mt19937 &rng) {
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
