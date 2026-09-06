#include "state_encoder_reference_5cb.h"
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool condition) {
  if (!condition) throw std::runtime_error("feature bitwise oracle mismatch");
}
void check(const Game &game) {
  for (int8_t observer : {-1, 0, 1}) for (int player : {0, 1}) {
    const auto expected = StateEncoderReference5CB::encode_canonical(game, player, observer);
    const auto actual = StateEncoder::encode_canonical(game, player, observer);
    require(std::memcmp(expected.data(), actual.data(), sizeof(actual)) == 0);
  }
}
int main() {
  try {
    Game game(42);
    for (int card = -1; card < CARD_COUNT; ++card) {
      for (auto &row : game.board.visible) row.fill(card);
      for (auto &player : game.board.players) {
        player.reserved.fill(card);
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
      game = Game(42);
      if (hidden) {
        game.board.players[1].reserved[0] = id;
        game.board.players[1].reserved_is_hidden[0] = true;
      } else game.board.visible[0][0] = id;
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
