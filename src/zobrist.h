#ifndef CSPLENDOR_ZOBRIST_H
#define CSPLENDOR_ZOBRIST_H

#include "types.h"
#include <array>
#include <cstdint>
#include <random>

class Zobrist {
public:
  static Zobrist &get_instance() {
    static Zobrist instance;
    return instance;
  }

  uint64_t cards_board[3][4][91];    // level, slot, card_id + 1 (0 for empty)
  uint64_t cards_reserved[2][3][92]; // player, slot, card_id + 1, 91 for hidden
  uint64_t player_gems[2][6][13];    // player, gem_type, count
  uint64_t player_bonuses[2][5][16]; // player, color, count
  uint64_t bank_gems[6][13];         // gem_type, count
  uint64_t nobles_on_board[12];      // noble_id
  uint64_t current_player[2];
  uint64_t waiting_noble[2];

  Zobrist() {
    std::mt19937_64 rng(42); // Fixed seed for consistency across runs
    auto gen = [&]() { return std::uniform_int_distribution<uint64_t>()(rng); };

    for (int l = 0; l < 3; ++l)
      for (int s = 0; s < 4; ++s)
        for (int c = 0; c < 91; ++c)
          cards_board[l][s][c] = gen();

    for (int p = 0; p < 2; ++p)
      for (int s = 0; s < 3; ++s)
        for (int c = 0; c < 92; ++c)
          cards_reserved[p][s][c] = gen();

    for (int p = 0; p < 2; ++p)
      for (int g = 0; g < 6; ++g)
        for (int c = 0; c < 13; ++c)
          player_gems[p][g][c] = gen();

    for (int p = 0; p < 2; ++p)
      for (int b = 0; b < 5; ++b)
        for (int c = 0; c < 16; ++c)
          player_bonuses[p][b][c] = gen();

    for (int g = 0; g < 6; ++g)
      for (int c = 0; c < 13; ++c)
        bank_gems[g][c] = gen();

    for (int n = 0; n < 12; ++n)
      nobles_on_board[n] = gen();

    current_player[0] = gen();
    current_player[1] = gen();
    waiting_noble[0] = gen();
    waiting_noble[1] = gen();
  }
};

#endif // CSPLENDOR_ZOBRIST_H
