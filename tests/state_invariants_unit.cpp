#include "game.h"
#include "state_invariants.h"
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using csplendor::state::InvariantViolation;
using csplendor::state::Profile;
using csplendor::state::validate_invariants;

void check(bool condition) {
  if (!condition)
    std::abort();
}

void require_valid(const Board &board, Profile profile) {
  const auto report = validate_invariants(board, profile);
  if (!report.ok()) {
    std::cerr << csplendor::state::describe_invariant_violations(report)
              << '\n';
    check(false);
  }
}

void test_reachable_corpus() {
  uint64_t checked = 0;
  for (uint64_t seed = 0; seed < 64; ++seed) {
    Game game(seed);
    require_valid(game.board, Profile::Reachable);
    for (uint64_t ply = 0; ply < 160 && !game.is_game_over(); ++ply) {
      if (!game.apply_random_action(seed * 0x9e3779b97f4a7c15ULL + ply, false))
        break;
      require_valid(game.board, Profile::Reachable);
      ++checked;
    }
  }
  check(checked > 4000);
}

void test_profile_specific_physical_rules() {
  Game duplicate(11);
  duplicate.board.visible[0][0] = duplicate.board.visible[0][1];
  duplicate.board.invalidate_hash();
  require_valid(duplicate.board, Profile::Editor);
  const auto duplicate_report =
      validate_invariants(duplicate.board, Profile::Reachable);
  check(duplicate_report.has(InvariantViolation::DuplicateCard));

  Game tokens(12);
  ++tokens.board.bank[0];
  tokens.board.invalidate_hash();
  require_valid(tokens.board, Profile::Serialized);
  const auto token_report = validate_invariants(tokens.board, Profile::Search);
  check(token_report.has(InvariantViolation::TokenConservationMismatch));

  Game provenance(13);
  provenance.board.players[0].purchased_count = 1;
  provenance.board.invalidate_hash();
  require_valid(provenance.board, Profile::Editor);
  require_valid(provenance.board, Profile::Serialized);
  check(validate_invariants(provenance.board, Profile::Reachable)
            .has(InvariantViolation::PurchasedCountMismatch));
}

void test_local_and_derived_diagnostics() {
  Game packed(21);
  packed.board.players[0].packed_gems ^= 1;
  auto report = validate_invariants(packed.board, Profile::Editor);
  check(report.has(InvariantViolation::PackedGemsMismatch));

  Game reserved(22);
  reserved.board.players[0].reserved[1] = reserved.board.visible[0][0];
  reserved.board.players[0].reserved_count = 0;
  reserved.board.invalidate_hash();
  report = validate_invariants(reserved.board, Profile::Serialized);
  check(report.has(InvariantViolation::ReservedCountMismatch));
  report = validate_invariants(reserved.board, Profile::Reachable);
  check(report.has(InvariantViolation::ReservedLayoutMismatch));

  Game invalid_id(23);
  invalid_id.board.visible[0][0] = 127;
  invalid_id.board.invalidate_hash();
  check(validate_invariants(invalid_id.board, Profile::Editor)
            .has(InvariantViolation::InvalidCardId));

  Game overflow(24);
  overflow.board.decks[0].count = Board::MAX_DECK_SIZE + 1;
  overflow.board.invalidate_hash();
  check(validate_invariants(overflow.board, Profile::Editor)
            .has(InvariantViolation::FixedCapacityOverflow));
}

void test_stale_hash_is_visible_without_mutation() {
  Game game(31);
  const uint64_t cached = game.board.hash();
  check(game.board.hash_valid);
  ++game.board.turn; // Deliberately bypass the public mutation gateway.

  const auto report = validate_invariants(game.board, Profile::Reachable);
  check(report.has(InvariantViolation::StaleHashCache));
  check(game.board.hash_valid);
  check(game.board.cached_hash == cached);

  game.board.invalidate_hash();
  require_valid(game.board, Profile::Reachable);
  check(!game.board.hash_valid);
  const uint64_t refreshed = game.board.hash();
  check(refreshed != cached);
  require_valid(game.board, Profile::Reachable);
}

void test_search_determinization() {
  for (uint64_t seed = 0; seed < 32; ++seed) {
    Game game(seed);
    for (uint64_t ply = 0; ply < 12; ++ply) {
      if (!game.apply_random_action(seed * 37 + ply, false))
        break;
    }
    game.board.randomize_hidden_information(
        static_cast<uint8_t>(seed % Board::NUM_PLAYERS), seed + 1000);
    require_valid(game.board, Profile::Search);
  }
}

void test_deterministic_description() {
  csplendor::state::InvariantReport report;
  report.add(InvariantViolation::StaleHashCache);
  report.add(InvariantViolation::InvalidWinner);
  check(csplendor::state::describe_invariant_violations(report) ==
        "invalid_winner,stale_hash_cache");
  check(csplendor::state::describe_invariant_violations({}) == "ok");
}

} // namespace

int main() {
  test_reachable_corpus();
  test_profile_specific_physical_rules();
  test_local_and_derived_diagnostics();
  test_stale_hash_is_visible_without_mutation();
  test_search_determinization();
  test_deterministic_description();
  return 0;
}
