#include "information_state.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <stdexcept>

namespace {

Action first_deck_reservation(const Game &game) {
  for (const Action &action : game.legal_actions()) {
    if (action.type == RESERVE_DECK)
      return action;
  }
  throw std::runtime_error("fixture has no deck reservation");
}

void test_hidden_worlds_share_observer_identity() {
  Game root(42);
  const std::string root_key =
      csplendor::information_state::serialize(root, 0);
  for (uint64_t seed = 1; seed <= 16; ++seed) {
    const Game world = root.shuffled_clone_portable(0, seed);
    assert(csplendor::information_state::serialize(world, 0) == root_key);
  }

  Game reserved(7);
  assert(reserved.apply(first_deck_reservation(reserved), false));
  const std::string opponent_key =
      csplendor::information_state::serialize(reserved, 1);
  bool private_identity_changed = false;
  const std::string owner_key =
      csplendor::information_state::serialize(reserved, 0);
  for (uint64_t seed = 20; seed <= 60; ++seed) {
    const Game world = reserved.shuffled_clone_portable(1, seed);
    assert(csplendor::information_state::serialize(world, 1) == opponent_key);
    private_identity_changed |=
        csplendor::information_state::serialize(world, 0) != owner_key;
  }
  assert(private_identity_changed);
}

void test_unordered_public_slots_are_canonical() {
  Game root(11);
  Game reordered = root.clone_light();
  for (auto &level : reordered.board.visible)
    std::reverse(level.begin(), level.end());
  std::reverse(reordered.board.nobles.begin(), reordered.board.nobles.end());

  for (uint8_t observer = 0; observer < Board::NUM_PLAYERS; ++observer) {
    assert(csplendor::information_state::serialize(root, observer) ==
           csplendor::information_state::serialize(reordered, observer));
    assert(csplendor::information_state::stable_hash(root, observer) ==
           csplendor::information_state::stable_hash(reordered, observer));
  }
}

void test_public_changes_and_observer_are_distinct() {
  Game root(3);
  assert(csplendor::information_state::serialize(root, 0) !=
         csplendor::information_state::serialize(root, 1));

  Game changed = root.clone_light();
  ++changed.board.turn;
  assert(csplendor::information_state::serialize(root, 0) !=
         csplendor::information_state::serialize(changed, 0));

  changed = root.clone_light();
  changed.simple_payment_mode = true;
  assert(csplendor::information_state::serialize(root, 0) !=
         csplendor::information_state::serialize(changed, 0));
}

} // namespace

int main() {
  try {
    test_hidden_worlds_share_observer_identity();
    test_unordered_public_slots_are_canonical();
    test_public_changes_and_observer_are_distinct();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
  return 0;
}
