#ifndef CSPLENDOR_MCTS_TREE_KEY_H
#define CSPLENDOR_MCTS_TREE_KEY_H

#include <cstddef>
#include <cstdint>

enum class TreeDomain : uint8_t {
  Exact = 0,
  Observable = 1,
  LegacyExact = 2,
};

static constexpr uint32_t MCTS_TREE_KEY_VERSION = 1;
static constexpr uint8_t MCTS_NO_OBSERVER = 0xff;
static constexpr uint8_t MCTS_MODE_SIMPLE_PAYMENT = 1U << 0;
static constexpr uint8_t MCTS_MODE_BLANK_REFILL = 1U << 1;

struct TreeKey {
  uint64_t position_hash = 0;
  uint32_t key_version = MCTS_TREE_KEY_VERSION;
  uint8_t observer = MCTS_NO_OBSERVER;
  TreeDomain domain = TreeDomain::Exact;
  uint8_t mode_bits = 0;

  bool operator==(const TreeKey &other) const noexcept {
    return position_hash == other.position_hash &&
           key_version == other.key_version && observer == other.observer &&
           domain == other.domain && mode_bits == other.mode_bits;
  }

  bool operator!=(const TreeKey &other) const noexcept {
    return !(*this == other);
  }
};

// Repository-owned field-wise hash.  Replay digests must serialize the fields
// explicitly and must not depend on this process-local bucket hash.
struct TreeKeyHash {
  std::size_t operator()(const TreeKey &key) const noexcept {
    uint64_t value = key.position_hash;
    value ^= static_cast<uint64_t>(key.key_version) +
             0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    const uint64_t tail = static_cast<uint64_t>(key.observer) |
                          (static_cast<uint64_t>(key.domain) << 8) |
                          (static_cast<uint64_t>(key.mode_bits) << 16);
    value ^= tail + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return static_cast<std::size_t>(value);
  }
};

inline TreeKey make_legacy_tree_key(uint64_t hash) noexcept {
  return {hash, 0, MCTS_NO_OBSERVER, TreeDomain::LegacyExact, 0};
}

#endif // CSPLENDOR_MCTS_TREE_KEY_H
