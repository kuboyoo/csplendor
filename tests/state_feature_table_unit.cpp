#include "state_encoder_reference_5cb.h"
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition) {
  if (!condition) throw std::runtime_error("feature bitwise oracle mismatch");
}
// Representation only: IDs such as -128, 90 and 127 still reach the invalid-ID
// oracle below. This is not a check of game legality.
int8_t checked_int8(int value) {
  require(value >= std::numeric_limits<int8_t>::min() &&
          value <= std::numeric_limits<int8_t>::max());
  return static_cast<int8_t>(value);
}
void check(const Game &game) {
  for (int observer_value : {-1, 0, 1}) for (int player : {0, 1}) {
    const int8_t observer = checked_int8(observer_value);
    const auto expected = StateEncoderReference5CB::encode_canonical(game, player, observer);
    const auto actual = StateEncoder::encode_canonical(game, player, observer);
    require(std::memcmp(expected.data(), actual.data(), sizeof(actual)) == 0);
  }
}
int main() {
  try {
    Game game(42);
    for (int card = -1; card < CARD_COUNT; ++card) {
      const int8_t card_id = checked_int8(card);
      for (auto &row : game.board.visible) row.fill(card_id);
      for (auto &player : game.board.players) {
        player.reserved.fill(card_id);
        player.reserved_is_hidden = {true, false, true};
      }
      check(game);
    }
    for (int seed = 0; seed < 16; ++seed) {
      game = Game(seed);
      for (int ply = 0; ply < 100 && !game.is_game_over(); ++ply) {
        check(game);
        require(game.apply_random_action(static_cast<uint64_t>(seed * 7919 + ply * 97)));
      }
    }
    for (int id : {-128, -2, 90, 127}) for (bool hidden : {false, true}) {
      const int8_t invalid_id = checked_int8(id);
      game = Game(42);
      if (hidden) {
        game.board.players[1].reserved[0] = invalid_id;
        game.board.players[1].reserved_is_hidden[0] = true;
      } else game.board.visible[0][0] = invalid_id;
      bool reference_threw = false, actual_threw = false;
      try { (void)StateEncoderReference5CB::encode(game, 0); }
      catch (const std::out_of_range &) { reference_threw = true; }
      try { (void)StateEncoder::encode(game, 0); }
      catch (const std::out_of_range &) { actual_threw = true; }
      require(reference_threw && actual_threw);
    }
    std::cout << "all 196 floats: 90 cards/empty/hidden/observers/canonical/reachable/invalid PASS\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
