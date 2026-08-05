#ifndef CSPLENDOR_STATE_INVARIANTS_H
#define CSPLENDOR_STATE_INVARIANTS_H

#include "board.h"
#include <cstdint>
#include <string>

namespace csplendor::state {

// The profiles mirror doc/refactoring_contracts.md.  Reachable and Search
// states obey the physical game invariants, while Editor and Serialized
// states intentionally permit incomplete or duplicated game material.
enum class Profile : uint8_t { Reachable, Editor, Search, Serialized };

enum class InvariantViolation : uint64_t {
  InvalidCurrentPlayer = 1ULL << 0U,
  InvalidWinner = 1ULL << 1U,
  FixedCapacityOverflow = 1ULL << 2U,
  InvalidCardId = 1ULL << 3U,
  InvalidCardLevel = 1ULL << 4U,
  InvalidNobleId = 1ULL << 5U,
  InvalidProvenanceCapacity = 1ULL << 6U,
  PackedGemsMismatch = 1ULL << 7U,
  PackedBonusesMismatch = 1ULL << 8U,
  NobleEligibilityMismatch = 1ULL << 9U,
  ReservedCountMismatch = 1ULL << 10U,
  ReservedLayoutMismatch = 1ULL << 11U,
  HiddenReservationMismatch = 1ULL << 12U,
  PurchasedCountMismatch = 1ULL << 13U,
  BonusProvenanceMismatch = 1ULL << 14U,
  PointProvenanceMismatch = 1ULL << 15U,
  TokenConservationMismatch = 1ULL << 16U,
  PlayerTokenLimitExceeded = 1ULL << 17U,
  DuplicateCard = 1ULL << 18U,
  DuplicateNoble = 1ULL << 19U,
  NoblePartitionMismatch = 1ULL << 20U,
  StaleHashCache = 1ULL << 21U,
};

struct InvariantReport {
  uint64_t violations = 0;

  constexpr bool ok() const noexcept { return violations == 0; }

  constexpr bool has(InvariantViolation violation) const noexcept {
    return (violations & static_cast<uint64_t>(violation)) != 0;
  }

  constexpr void add(InvariantViolation violation) noexcept {
    violations |= static_cast<uint64_t>(violation);
  }
};

// Pure diagnostic: this function never repairs the state and never populates
// or invalidates Board's lazy hash cache.
InvariantReport validate_invariants(const Board &board,
                                    Profile profile) noexcept;

const char *invariant_violation_name(InvariantViolation violation) noexcept;
std::string describe_invariant_violations(const InvariantReport &report);

} // namespace csplendor::state

#endif // CSPLENDOR_STATE_INVARIANTS_H
