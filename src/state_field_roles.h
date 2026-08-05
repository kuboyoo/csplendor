#ifndef CSPLENDOR_STATE_FIELD_ROLES_H
#define CSPLENDOR_STATE_FIELD_ROLES_H

#include <array>
#include <cstdint>
#include <string_view>

namespace csplendor::state {

enum class FieldRole : uint8_t {
  Canonical,
  Derived,
  Provenance,
  Cache,
};

struct FieldRoleDescriptor {
  std::string_view owner;
  std::string_view name;
  FieldRole role;
};

// This inventory is the machine-readable counterpart of the R2-C ownership
// table. Keep one entry per public data member so representation changes must
// make an explicit canonical/derived/provenance/cache decision.
inline constexpr std::array<FieldRoleDescriptor, 12> BOARD_FIELD_ROLES = {{
    {"Board", "bank", FieldRole::Canonical},
    {"Board", "visible", FieldRole::Canonical},
    {"Board", "decks", FieldRole::Canonical},
    {"Board", "nobles", FieldRole::Canonical},
    {"Board", "players", FieldRole::Canonical},
    {"Board", "current_player", FieldRole::Canonical},
    {"Board", "turn", FieldRole::Canonical},
    {"Board", "final_round", FieldRole::Canonical},
    {"Board", "waiting_noble", FieldRole::Canonical},
    {"Board", "winner", FieldRole::Canonical},
    {"Board", "cached_hash", FieldRole::Cache},
    {"Board", "hash_valid", FieldRole::Cache},
}};

inline constexpr std::array<FieldRoleDescriptor, 12> PLAYER_FIELD_ROLES = {{
    {"PlayerState", "gems", FieldRole::Canonical},
    {"PlayerState", "packed_gems", FieldRole::Derived},
    {"PlayerState", "bonuses", FieldRole::Canonical},
    {"PlayerState", "packed_bonuses", FieldRole::Derived},
    {"PlayerState", "points", FieldRole::Canonical},
    {"PlayerState", "reserved", FieldRole::Canonical},
    {"PlayerState", "reserved_is_hidden", FieldRole::Canonical},
    {"PlayerState", "reserved_count", FieldRole::Canonical},
    {"PlayerState", "purchased_count", FieldRole::Canonical},
    {"PlayerState", "purchased_cards", FieldRole::Provenance},
    {"PlayerState", "acquired_nobles", FieldRole::Provenance},
    {"PlayerState", "noble_eligibility_mask", FieldRole::Derived},
}};

inline constexpr std::array<FieldRoleDescriptor, 5> GAME_FIELD_ROLES = {{
    {"Game", "board", FieldRole::Canonical},
    {"Game", "history", FieldRole::Provenance},
    {"Game", "board_history", FieldRole::Provenance},
    {"Game", "simple_payment_mode", FieldRole::Canonical},
    {"Game", "blank_refill_mode", FieldRole::Canonical},
}};

constexpr const char *field_role_name(FieldRole role) noexcept {
  switch (role) {
  case FieldRole::Canonical:
    return "canonical";
  case FieldRole::Derived:
    return "derived";
  case FieldRole::Provenance:
    return "provenance";
  case FieldRole::Cache:
    return "cache";
  }
  return "unknown";
}

} // namespace csplendor::state

#endif // CSPLENDOR_STATE_FIELD_ROLES_H
