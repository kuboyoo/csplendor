#include "board_editor.h"
#include "fixed_stack.h"
#include "game.h"
#include "state_invariants.h"
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace {

void check(bool condition) {
  if (!condition)
    std::abort();
}

template <typename Exception, typename Operation>
void check_throws(Operation operation) {
  try {
    operation();
  } catch (const Exception &) {
    return;
  }
  check(false);
}

void test_fixed_stack_overflow_policies() {
  FixedStack<int, 2> stack;
  check(stack.capacity() == 2);
  check(stack.empty());
  check(stack.try_push_back(10));
  check(stack.try_push_back(20));
  check(stack.full());
  check(!stack.try_push_back(30));
  check(stack.size() == 2 && stack[0] == 10 && stack[1] == 20);

  // The source-compatible wrapper keeps its historical silent-overflow
  // behavior, while internal callers can observe failure through try_*.
  stack.push_back(40);
  check(stack.size() == 2 && stack.back() == 20);
  check(stack.try_pop_back());
  check(stack.size() == 1 && stack.back() == 10);
  check(stack.try_pop_back());
  check(!stack.try_pop_back());
  stack.pop_back();
  check(stack.empty());
}

void test_editor_failure_is_atomic_for_state_and_cache() {
  Board board;
  board.init(42);
  const auto before_visible = board.visible;
  const uint64_t before_hash = board.hash();
  check(board.hash_valid);

  auto invalid_visible = std::vector<std::vector<int>>(
      3, std::vector<int>(Board::CARDS_PER_LEVEL, -1));
  invalid_visible[2][3] = CARD_COUNT;
  check_throws<std::invalid_argument>(
      [&] { csplendor::state::editor::set_visible(board, invalid_visible); });
  check(board.visible == before_visible);
  check(board.hash_valid && board.cached_hash == before_hash);

  auto invalid_decks = std::vector<std::vector<int>>(3);
  invalid_decks[0].assign(Board::MAX_DECK_SIZE + 1, 0);
  check_throws<std::invalid_argument>(
      [&] { csplendor::state::editor::set_decks(board, invalid_decks); });
  check(board.hash_valid && board.cached_hash == before_hash);

  check_throws<std::out_of_range>(
      [&] { csplendor::state::editor::set_current_player(board, -1); });
  check_throws<std::out_of_range>([&] {
    csplendor::state::editor::set_player(board, Board::NUM_PLAYERS,
                                         PlayerState{});
  });
  check(board.hash_valid && board.cached_hash == before_hash);
}

void test_valid_editor_commit_normalizes_and_invalidates() {
  Board board;
  board.init(7);
  static_cast<void>(board.hash());

  PlayerState player = board.players[0];
  player.gems = {1, 2, 3, 0, 1, 0};
  player.bonuses = {1, 0, 2, 0, 1};
  player.packed_gems = 0;
  player.packed_bonuses = 0;
  player.noble_eligibility_mask = 0xffffU;
  csplendor::state::editor::set_player(board, 0, player);
  check(!board.hash_valid);
  check(board.players[0].gems == player.gems);
  check(board.players[0].bonuses == player.bonuses);
  check(csplendor::state::validate_invariants(board,
                                              csplendor::state::Profile::Editor)
            .ok());

  static_cast<void>(board.hash());
  const std::array<uint8_t, 6> bank = {3, 4, 4, 4, 4, 5};
  csplendor::state::editor::set_bank(board, bank);
  check(board.bank == bank && !board.hash_valid);
}

void test_unchecked_gateway_invalidates_before_hot_path_mutation() {
  Board board;
  board.init(9);
  const uint64_t before = board.hash();
  Board &mutable_board = board.begin_unchecked_mutation();
  check(!board.hash_valid);
  ++mutable_board.turn;
  check(board.hash() != before);
}

void test_game_transitions_stay_reachable() {
  for (uint64_t seed = 0; seed < 32; ++seed) {
    Game game(seed);
    for (uint64_t ply = 0; ply < 128 && !game.is_game_over(); ++ply) {
      if (!game.apply_random_action(seed * 97 + ply, false))
        break;
      check(csplendor::state::validate_invariants(
                game.board, csplendor::state::Profile::Reachable)
                .ok());
    }
  }
}

} // namespace

int main() {
  test_fixed_stack_overflow_policies();
  test_editor_failure_is_atomic_for_state_and_cache();
  test_valid_editor_commit_normalizes_and_invalidates();
  test_unchecked_gateway_invalidates_before_hot_path_mutation();
  test_game_transitions_stay_reachable();
  return 0;
}
