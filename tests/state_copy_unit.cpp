#include "game.h"
#include "game_snapshot.h"
#include "state_field_roles.h"
#include "undo_record.h"
#include <cstdlib>
#include <string_view>

namespace {

void check(bool condition) {
  if (!condition)
    std::abort();
}

template <typename Fields>
bool has_field(const Fields &fields, std::string_view name,
               csplendor::state::FieldRole role) {
  for (const auto &field : fields) {
    if (field.name == name && field.role == role)
      return true;
  }
  return false;
}

void build_history(Game &game) {
  for (int ply = 0; ply < 12 && !game.is_game_over(); ++ply) {
    const auto codes = game.legal_action_codes();
    check(!codes.empty());
    check(game.apply_action_code(codes[static_cast<size_t>(ply) % codes.size()],
                                 true));
  }
  check(!game.history.empty());
  check(game.history.size() == game.board_history.size());
}

void test_copy_policies() {
  Game source(17);
  source.simple_payment_mode = true;
  source.blank_refill_mode = true;
  build_history(source);

  const std::string snapshot = csplendor::snapshot::serialize(source);
  Game full = source.clone();
  Game current = source.clone_light();
  Game shuffled = source.shuffled_clone_portable(0, 99);
  Game shuffled_again = source.shuffled_clone_portable(0, 99);

  check(csplendor::snapshot::serialize(full) == snapshot);
  check(full.history.size() == source.history.size());
  check(full.board_history.size() == source.board_history.size());
  check(csplendor::snapshot::serialize(current) == snapshot);
  check(current.history.empty() && current.board_history.empty());
  check(current.simple_payment_mode && current.blank_refill_mode);
  check(shuffled.history.empty() && shuffled.board_history.empty());
  check(shuffled.simple_payment_mode && shuffled.blank_refill_mode);
  check(csplendor::snapshot::serialize(shuffled) ==
        csplendor::snapshot::serialize(shuffled_again));
  check(csplendor::snapshot::serialize(source) == snapshot);

  // Every copy owns its Board and provenance vectors independently.
  const size_t source_count = source.board.players[0].purchased_cards.size();
  current.board.players[0].purchased_cards.push_back(89);
  check(source.board.players[0].purchased_cards.size() == source_count);
}

void test_serialized_snapshot_excludes_journals() {
  Game source(23);
  build_history(source);
  const std::string bytes = csplendor::snapshot::serialize(source);
  Game restored = csplendor::snapshot::deserialize(bytes);

  check(csplendor::snapshot::serialize(restored) == bytes);
  check(restored.history.empty() && restored.board_history.empty());
}

void test_undo_snapshot_preserves_editor_compatibility() {
  Game game(31);
  const std::string before = csplendor::snapshot::serialize(game);
  const auto codes = game.legal_action_codes();
  check(!codes.empty() && game.apply_action_code(codes.front(), true));

  // Public C++ and Python editors may mutate after apply(). Production undo
  // deliberately restores the complete pre-action Board snapshot.
  game.board.begin_editor_mutation().turn = 60000;
  game.board.players[0].purchased_cards.push_back(89);
  check(game.undo());
  check(csplendor::snapshot::serialize(game) == before);
}

void test_delta_record_remains_a_verification_type() {
  Game game(41);
  const Board before = game.board;
  const auto record = csplendor::detail::UndoRecord::capture(game.board);
  const auto codes = game.legal_action_codes();
  check(!codes.empty() && game.apply_action_code_trusted(codes.front(), false));
  check(record.restores_snapshot(game.board, before));
  check(sizeof(csplendor::detail::UndoRecord) < sizeof(Board));
}

void test_field_role_inventory() {
  using csplendor::state::BOARD_FIELD_ROLES;
  using csplendor::state::FieldRole;
  using csplendor::state::GAME_FIELD_ROLES;
  using csplendor::state::PLAYER_FIELD_ROLES;

  check(has_field(BOARD_FIELD_ROLES, "bank", FieldRole::Canonical));
  check(has_field(BOARD_FIELD_ROLES, "cached_hash", FieldRole::Cache));
  check(has_field(PLAYER_FIELD_ROLES, "packed_gems", FieldRole::Derived));
  check(
      has_field(PLAYER_FIELD_ROLES, "purchased_cards", FieldRole::Provenance));
  check(has_field(GAME_FIELD_ROLES, "history", FieldRole::Provenance));
  check(std::string_view(csplendor::state::field_role_name(FieldRole::Cache)) ==
        "cache");
}

} // namespace

int main() {
  test_copy_policies();
  test_serialized_snapshot_excludes_journals();
  test_undo_snapshot_preserves_editor_compatibility();
  test_delta_record_remains_a_verification_type();
  test_field_role_inventory();
  return 0;
}
