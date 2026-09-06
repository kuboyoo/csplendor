#ifndef CSPLENDOR_SOLVER_TT_TYPES_H
#define CSPLENDOR_SOLVER_TT_TYPES_H

#include "card_data.h"
#include "perf_counters.h"
#include "solver_types.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace csplendor::solver_internal {

namespace solver_tt_detail {

constexpr uint64_t reveal_metadata(const StateKeyCore &core, uint8_t reserved0,
                                   uint8_t reserved1) noexcept {
  uint64_t meta = static_cast<uint64_t>(core.points0);
  meta |= static_cast<uint64_t>(core.points1) << 8;
  meta |= static_cast<uint64_t>(core.purchased0) << 16;
  meta |= static_cast<uint64_t>(core.purchased1) << 24;
  meta |= static_cast<uint64_t>(reserved0) << 32;
  meta |= static_cast<uint64_t>(reserved1) << 40;
  meta |= static_cast<uint64_t>(core.final_round ? 1 : 0) << 48;
  meta |= static_cast<uint64_t>(static_cast<uint8_t>(core.winner + 2)) << 49;
  return meta;
}

inline uint8_t entry_flags(ForceStatus status, bool has_action,
                           bool replayable) noexcept {
  return static_cast<uint8_t>(status) |
         static_cast<uint8_t>(has_action ? 1U << 2 : 0U) |
         static_cast<uint8_t>(replayable ? 1U << 3 : 0U);
}

inline uint16_t checked_u16_action_count(size_t value) {
  if (value > std::numeric_limits<uint16_t>::max())
    throw std::overflow_error("solver TT action count exceeds uint16_t");
  return static_cast<uint16_t>(value);
}

inline int8_t checked_reveal_card(int value) {
  if (value < -1 || value >= CARD_COUNT)
    throw std::out_of_range("solver TT reveal card is out of range");
  return static_cast<int8_t>(value);
}

inline int8_t checked_visible_score(int value) noexcept {
  assert(value >= -1 && value <= 1);
  return static_cast<int8_t>(value);
}

} // namespace solver_tt_detail

static_assert(CARD_COUNT <= std::numeric_limits<int8_t>::max(),
              "card IDs must fit the compact reveal-card field");

class RevealStateKey {
public:
  RevealStateKey() = default;

  RevealStateKey(const StateKeyCore &core, uint64_t unseen_low,
                 uint64_t unseen_high, uint64_t acquired_hidden_low,
                 uint64_t acquired_hidden_high, uint8_t reserved0,
                 uint8_t reserved1) noexcept
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      : board_hash_(core.board_hash), unseen_low_(unseen_low),
        unseen_high_(unseen_high), acquired_hidden_low_(acquired_hidden_low),
        acquired_hidden_high_(acquired_hidden_high),
        metadata_(
            solver_tt_detail::reveal_metadata(core, reserved0, reserved1)) {}
#else
      : core_(core), unseen_low_(unseen_low), unseen_high_(unseen_high),
        acquired_hidden_low_(acquired_hidden_low),
        acquired_hidden_high_(acquired_hidden_high), reserved0_(reserved0),
        reserved1_(reserved1) {
  }
#endif

  bool operator==(const RevealStateKey &other) const noexcept {
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return board_hash_ == other.board_hash_ &&
           unseen_low_ == other.unseen_low_ &&
           unseen_high_ == other.unseen_high_ &&
           acquired_hidden_low_ == other.acquired_hidden_low_ &&
           acquired_hidden_high_ == other.acquired_hidden_high_ &&
           metadata_ == other.metadata_;
#else
    return core_ == other.core_ && unseen_low_ == other.unseen_low_ &&
           unseen_high_ == other.unseen_high_ &&
           acquired_hidden_low_ == other.acquired_hidden_low_ &&
           acquired_hidden_high_ == other.acquired_hidden_high_ &&
           reserved0_ == other.reserved0_ && reserved1_ == other.reserved1_;
#endif
  }

  bool operator!=(const RevealStateKey &other) const noexcept {
    return !(*this == other);
  }

  uint64_t board_hash() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return board_hash_;
#else
    return core_.board_hash;
#endif
  }

  uint64_t unseen_low() const noexcept { return unseen_low_; }
  uint64_t unseen_high() const noexcept { return unseen_high_; }
  uint64_t acquired_hidden_low() const noexcept { return acquired_hidden_low_; }
  uint64_t acquired_hidden_high() const noexcept {
    return acquired_hidden_high_;
  }

  uint64_t metadata_bits() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return metadata_;
#else
    return solver_tt_detail::reveal_metadata(core_, reserved0_, reserved1_);
#endif
  }

private:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t board_hash_ = 0;
#else
  StateKeyCore core_;
#endif
  uint64_t unseen_low_ = 0;
  uint64_t unseen_high_ = 0;
  uint64_t acquired_hidden_low_ = 0;
  uint64_t acquired_hidden_high_ = 0;
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t metadata_ = solver_tt_detail::reveal_metadata(StateKeyCore{}, 0, 0);
#else
  uint8_t reserved0_ = 0;
  uint8_t reserved1_ = 0;
#endif
};

struct RevealStateKeyHash {
  size_t operator()(const RevealStateKey &key) const
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      noexcept
#endif
  {
    const uint64_t mixed =
        key.board_hash() ^ (key.unseen_low() * 0x9e3779b97f4a7c15ULL) ^
        (key.unseen_high() * 0xc2b2ae3d27d4eb4fULL) ^
        (key.acquired_hidden_low() * 0x27d4eb2f165667c5ULL) ^
        (key.acquired_hidden_high() * 0x94d049bb133111ebULL) ^
        (key.metadata_bits() * 0x165667b19e3779f9ULL);
    return std::hash<uint64_t>{}(mixed);
  }
};

struct RevealDepthStateKey {
  RevealStateKey state;
  int depth = 0;

  RevealDepthStateKey() = default;
  RevealDepthStateKey(const RevealStateKey &key_state, int key_depth) noexcept
      : state(key_state), depth(key_depth) {}

  bool operator==(const RevealDepthStateKey &other) const noexcept {
    return state == other.state && depth == other.depth;
  }
};

struct RevealDepthStateKeyHash {
  size_t operator()(const RevealDepthStateKey &key) const
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      noexcept
#endif
  {
    return RevealStateKeyHash{}(key.state) ^
           (std::hash<int>{}(key.depth) * 0x9e3779b9U);
  }
};

#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
class RevealExactStateKey {
public:
  RevealExactStateKey() = default;
  explicit RevealExactStateKey(const RevealStateKey &key)
      : board_hash_(key.board_hash()), unseen_low_(key.unseen_low()),
        unseen_high_(key.unseen_high()), metadata_(key.metadata_bits()) {
    // Exact/root-independent searches deliberately exclude cards acquired
    // before the root.  Reject accidental use with the general reveal key in
    // every build because omitting these bits would otherwise cause false
    // transposition hits.
    if (key.acquired_hidden_low() != 0 || key.acquired_hidden_high() != 0)
      throw std::invalid_argument(
          "exact reveal TT key requires a root-independent state");
  }

  bool operator==(const RevealExactStateKey &other) const noexcept {
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    return board_hash_ == other.board_hash_ &&
           unseen_low_ == other.unseen_low_ &&
           unseen_high_ == other.unseen_high_ && metadata_ == other.metadata_;
  }

  uint64_t board_hash() const noexcept { return board_hash_; }
  uint64_t unseen_low() const noexcept { return unseen_low_; }
  uint64_t unseen_high() const noexcept { return unseen_high_; }
  uint64_t metadata_bits() const noexcept { return metadata_; }

private:
  uint64_t board_hash_ = 0;
  uint64_t unseen_low_ = 0;
  uint64_t unseen_high_ = 0;
  uint64_t metadata_ = solver_tt_detail::reveal_metadata(StateKeyCore{}, 0, 0);
};

struct RevealExactStateKeyHash {
  size_t operator()(const RevealExactStateKey &key) const noexcept {
    const uint64_t mixed = key.board_hash() ^
                           (key.unseen_low() * 0x9e3779b97f4a7c15ULL) ^
                           (key.unseen_high() * 0xc2b2ae3d27d4eb4fULL) ^
                           (key.metadata_bits() * 0x165667b19e3779f9ULL);
    return std::hash<uint64_t>{}(mixed);
  }
};

struct RevealExactDepthStateKey {
  RevealExactStateKey state;
  int depth = 0;

  RevealExactDepthStateKey() = default;
  RevealExactDepthStateKey(const RevealStateKey &full_state, int key_depth)
      : state(full_state), depth(key_depth) {}

  bool operator==(const RevealExactDepthStateKey &other) const noexcept {
    return state == other.state && depth == other.depth;
  }
};

struct RevealExactDepthStateKeyHash {
  size_t operator()(const RevealExactDepthStateKey &key) const noexcept {
    return RevealExactStateKeyHash{}(key.state) ^
           (std::hash<int>{}(key.depth) * 0x9e3779b9U);
  }
};
#else
using RevealExactStateKey = RevealStateKey;
using RevealExactStateKeyHash = RevealStateKeyHash;
using RevealExactDepthStateKey = RevealDepthStateKey;
using RevealExactDepthStateKeyHash = RevealDepthStateKeyHash;
#endif

class RevealMemoEntry {
public:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  static constexpr bool tracks_persistence = false;
#else
  static constexpr bool tracks_persistence = true;
#endif

  RevealMemoEntry() = default;
  RevealMemoEntry(ForceStatus status, uint64_t action_code, int reveal_card,
                  bool has_action, size_t action_count, bool replayable = true)
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      : action_code_(action_code), action_count_(action_count),
        reveal_card_(solver_tt_detail::checked_reveal_card(reveal_card)),
        flags_(solver_tt_detail::entry_flags(status, has_action, replayable)) {}
#else
      : status_(status), action_code_(action_code), reveal_card_(reveal_card),
        has_action_(has_action), action_count_(action_count),
        replayable_(replayable) {
  }
#endif

  ForceStatus status() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return static_cast<ForceStatus>(flags_ & 0x3U);
#else
    return status_;
#endif
  }

  void set_status(ForceStatus value) noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    flags_ = static_cast<uint8_t>((flags_ & ~uint8_t{0x3U}) |
                                  static_cast<uint8_t>(value));
#else
    status_ = value;
#endif
  }

  uint64_t action_code() const noexcept { return action_code_; }
  int reveal_card() const noexcept { return reveal_card_; }
  bool has_action() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return (flags_ & (1U << 2)) != 0;
#else
    return has_action_;
#endif
  }
  size_t action_count() const noexcept {
    return static_cast<size_t>(action_count_);
  }
  void set_action_count(size_t value) noexcept { action_count_ = value; }
  bool replayable() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return (flags_ & (1U << 3)) != 0;
#else
    return replayable_;
#endif
  }

#ifndef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t generation() const noexcept { return generation_; }
  uint64_t last_touched() const noexcept { return last_touched_; }
  void set_generation(uint64_t value) noexcept { generation_ = value; }
  void set_last_touched(uint64_t value) noexcept { last_touched_ = value; }
#endif

private:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t action_code_ = 0;
  size_t action_count_ = 0;
  int8_t reveal_card_ = -1;
  uint8_t flags_ =
      solver_tt_detail::entry_flags(ForceStatus::UNKNOWN, false, true);
#else
  ForceStatus status_ = ForceStatus::UNKNOWN;
  uint64_t action_code_ = 0;
  int reveal_card_ = -1;
  bool has_action_ = false;
  size_t action_count_ = 0;
  bool replayable_ = true;
  uint64_t generation_ = 0;
  uint64_t last_touched_ = 0;
#endif
};

class RevealPersistentEntry {
public:
  static constexpr bool tracks_persistence = true;

  RevealPersistentEntry() = default;
  RevealPersistentEntry(ForceStatus status, uint64_t action_code,
                        int reveal_card, bool has_action, size_t action_count,
                        bool replayable = true)
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      : action_code_(action_code),
        action_count_(checked_action_count(action_count)),
        reveal_card_(solver_tt_detail::checked_reveal_card(reveal_card)),
        flags_(solver_tt_detail::entry_flags(status, has_action, replayable)) {}
#else
      : status_(status), action_code_(action_code), reveal_card_(reveal_card),
        has_action_(has_action), action_count_(action_count),
        replayable_(replayable) {
  }
#endif

  ForceStatus status() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return static_cast<ForceStatus>(flags_ & 0x3U);
#else
    return status_;
#endif
  }
  void set_status(ForceStatus value) noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    flags_ = static_cast<uint8_t>((flags_ & ~uint8_t{0x3U}) |
                                  static_cast<uint8_t>(value));
#else
    status_ = value;
#endif
  }
  uint64_t action_code() const noexcept { return action_code_; }
  int reveal_card() const noexcept { return reveal_card_; }
  bool has_action() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return (flags_ & (1U << 2)) != 0;
#else
    return has_action_;
#endif
  }
  size_t action_count() const noexcept {
    return static_cast<size_t>(action_count_);
  }
  void set_action_count(size_t value) {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    action_count_ = checked_action_count(value);
#else
    action_count_ = value;
#endif
  }
  bool replayable() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return (flags_ & (1U << 3)) != 0;
#else
    return replayable_;
#endif
  }

  uint64_t generation() const noexcept { return generation_; }
  uint64_t last_touched() const noexcept { return last_touched_; }
  void set_generation(uint64_t value) noexcept { generation_ = value; }
  void set_last_touched(uint64_t value) noexcept { last_touched_ = value; }

private:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  static uint32_t checked_action_count(size_t value) {
    if (value > std::numeric_limits<uint32_t>::max())
      throw std::overflow_error(
          "persistent solver TT action count exceeds uint32_t");
    return static_cast<uint32_t>(value);
  }

  uint64_t action_code_ = 0;
  uint64_t generation_ = 0;
  uint64_t last_touched_ = 0;
  uint32_t action_count_ = 0;
  int8_t reveal_card_ = -1;
  uint8_t flags_ =
      solver_tt_detail::entry_flags(ForceStatus::UNKNOWN, false, true);
#else
  ForceStatus status_ = ForceStatus::UNKNOWN;
  uint64_t action_code_ = 0;
  int reveal_card_ = -1;
  bool has_action_ = false;
  size_t action_count_ = 0;
  bool replayable_ = true;
  uint64_t generation_ = 0;
  uint64_t last_touched_ = 0;
#endif
};

struct VisibleStateKey {
  StateKeyCore core;

  bool operator==(const VisibleStateKey &other) const noexcept {
    CSPLENDOR_PERF_TT_KEY_COMPARISON();
    return core == other.core;
  }
};

struct VisibleStateKeyHash {
  size_t operator()(const VisibleStateKey &key) const
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      noexcept
#endif
  {
    return std::hash<uint64_t>{}(
        key.core.board_hash ^
        (key.core.metadata_bits() * 0x9e3779b97f4a7c15ULL));
  }
};

struct VisibleDepthStateKey {
  VisibleStateKey state;
  int depth = 0;

  bool operator==(const VisibleDepthStateKey &other) const noexcept {
    return state == other.state && depth == other.depth;
  }
};

struct VisibleDepthStateKeyHash {
  size_t operator()(const VisibleDepthStateKey &key) const
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      noexcept
#endif
  {
    return VisibleStateKeyHash{}(key.state) ^
           (std::hash<int>{}(key.depth) * 0x9e3779b9U);
  }
};

enum class VisibleEntryReason : uint8_t {
  NoVisibleAction,
  CurrentPlayerWin,
  CurrentPlayerDraw,
  AllResponsesLose,
};

#ifdef CSPLENDOR_COMPACT_SOLVER_REASONS
using VisibleEntryReasonStorage = VisibleEntryReason;
#else
using VisibleEntryReasonStorage = std::string;
#endif

enum class VisibleBound : uint8_t { EXACT, LOWER, UPPER };

class VisibleMemoEntry {
public:
  using Bound = VisibleBound;

  VisibleMemoEntry() = default;
  VisibleMemoEntry(int score, Bound bound, VisibleEntryReasonStorage reason,
                   uint64_t action_code, bool has_action, size_t action_count)
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      : action_code_(action_code),
        action_count_(solver_tt_detail::checked_u16_action_count(action_count)),
        score_(solver_tt_detail::checked_visible_score(score)), bound_(bound),
        reason_(std::move(reason)), has_action_(has_action) {}
#else
      : score_(score), bound_(bound), reason_(std::move(reason)),
        action_code_(action_code), has_action_(has_action),
        action_count_(action_count) {
  }
#endif

  int score() const noexcept {
    return static_cast<int>(score_);
  }
  Bound bound() const noexcept { return bound_; }
  const VisibleEntryReasonStorage &reason() const noexcept { return reason_; }
  uint64_t action_code() const noexcept { return action_code_; }
  bool has_action() const noexcept { return has_action_; }
  size_t action_count() const noexcept {
    return static_cast<size_t>(action_count_);
  }

private:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t action_code_ = 0;
  uint16_t action_count_ = 0;
  int8_t score_ = 0;
  Bound bound_ = Bound::EXACT;
  VisibleEntryReasonStorage reason_{};
  bool has_action_ = false;
#else
  int score_ = 0;
  Bound bound_ = Bound::EXACT;
  VisibleEntryReasonStorage reason_{};
  uint64_t action_code_ = 0;
  bool has_action_ = false;
  size_t action_count_ = 0;
#endif
};

class VisibleForceEntry {
public:
  VisibleForceEntry() = default;
  VisibleForceEntry(ForceStatus status, uint64_t action_code, bool has_action,
                    size_t action_count)
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
      : action_code_(action_code),
        action_count_(solver_tt_detail::checked_u16_action_count(action_count)),
        flags_(solver_tt_detail::entry_flags(status, has_action, true)) {}
#else
      : status_(status), action_code_(action_code), has_action_(has_action),
        action_count_(action_count) {
  }
#endif

  ForceStatus status() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return static_cast<ForceStatus>(flags_ & 0x3U);
#else
    return status_;
#endif
  }
  uint64_t action_code() const noexcept { return action_code_; }
  bool has_action() const noexcept {
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
    return (flags_ & (1U << 2)) != 0;
#else
    return has_action_;
#endif
  }
  size_t action_count() const noexcept {
    return static_cast<size_t>(action_count_);
  }

private:
#ifdef CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES
  uint64_t action_code_ = 0;
  uint16_t action_count_ = 0;
  uint8_t flags_ =
      solver_tt_detail::entry_flags(ForceStatus::UNKNOWN, false, true);
#else
  ForceStatus status_ = ForceStatus::UNKNOWN;
  uint64_t action_code_ = 0;
  bool has_action_ = false;
  size_t action_count_ = 0;
#endif
};

struct VisibleForceBounds {
  int min_proven_depth = std::numeric_limits<int>::max();
  int max_refuted_depth = -1;
  VisibleForceEntry proven;
  VisibleForceEntry refuted;
};

struct SolverTtLayoutMetrics {
  size_t reveal_state_key = 0;
  size_t reveal_depth_key = 0;
  size_t reveal_exact_state_key = 0;
  size_t reveal_exact_depth_key = 0;
  size_t reveal_memo_entry = 0;
  size_t reveal_persistent_entry = 0;
  size_t reveal_memo_value = 0;
  size_t reveal_persistent_value = 0;
  size_t reveal_proof_node_value = 0;
  size_t visible_state_key = 0;
  size_t visible_depth_key = 0;
  size_t visible_memo_entry = 0;
  size_t visible_force_entry = 0;
  size_t visible_force_bounds = 0;
  size_t visible_memo_value = 0;
  size_t visible_force_value = 0;
  size_t visible_bounds_value = 0;
};

inline SolverTtLayoutMetrics solver_tt_layout_metrics() noexcept {
  return {
      sizeof(RevealStateKey),
      sizeof(RevealDepthStateKey),
      sizeof(RevealExactStateKey),
      sizeof(RevealExactDepthStateKey),
      sizeof(RevealMemoEntry),
      sizeof(RevealPersistentEntry),
      sizeof(std::pair<const RevealDepthStateKey, RevealMemoEntry>),
      sizeof(std::pair<const RevealExactDepthStateKey, RevealPersistentEntry>),
      sizeof(std::pair<const RevealDepthStateKey, size_t>),
      sizeof(VisibleStateKey),
      sizeof(VisibleDepthStateKey),
      sizeof(VisibleMemoEntry),
      sizeof(VisibleForceEntry),
      sizeof(VisibleForceBounds),
      sizeof(std::pair<const VisibleStateKey, VisibleMemoEntry>),
      sizeof(std::pair<const VisibleDepthStateKey, VisibleForceEntry>),
      sizeof(std::pair<const VisibleStateKey, VisibleForceBounds>)};
}

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_SOLVER_TT_TYPES_H
