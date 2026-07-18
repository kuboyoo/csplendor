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
  // Noble order is part of the action-encoding contract: the 48/legacy
  // encoders map a noble visit to its Board slot.  Use a (slot, noble_id)
  // salt so reordered and duplicate editor states cannot collapse by XOR.
  uint64_t nobles_on_board[12][12]; // slot, noble_id
  uint64_t current_player[2];
  uint64_t waiting_noble[2];
  // Full-information state fields.  These are deliberately separate from
  // the observable-state hash: the regular board hash is used as a game
  // position key and must distinguish future-relevant state.
  uint64_t deck_cards[3][40][90]; // level, stack position, card_id
  uint64_t deck_sizes[3][41];     // level, number of remaining cards
  uint64_t player_points[2][256];
  uint64_t player_reserved_count[2][4];
  uint64_t player_purchased_count[2][91];
  uint64_t reserved_is_hidden[2][3][2];
  // Public signature for an opponent hidden reserve.  The card ID remains
  // hidden, but its tier is public and is encoded by StateEncoder.  Use a
  // separate table so adding this contract does not shift legacy salts.
  uint64_t hidden_reserved_level[2][3][3]; // player, slot, level - 1
  uint64_t final_round[2];
  uint64_t winner[4]; // winner + 2: draw, ongoing, player 0, player 1
  uint64_t turn[65536];

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

    // Preserve the original main RNG consumption and the old slot-zero salts
    // so every non-noble Zobrist table keeps its existing values.  Additional
    // slot salts use an isolated deterministic stream.
    for (int n = 0; n < 12; ++n)
      nobles_on_board[0][n] = gen();
    std::mt19937_64 noble_slot_rng(0x4e4f424c45534c4fULL);
    auto gen_noble_slot = [&]() {
      return std::uniform_int_distribution<uint64_t>()(noble_slot_rng);
    };
    for (int slot = 1; slot < 12; ++slot)
      for (int n = 0; n < 12; ++n)
        nobles_on_board[slot][n] = gen_noble_slot();

    current_player[0] = gen();
    current_player[1] = gen();
    waiting_noble[0] = gen();
    waiting_noble[1] = gen();

    for (int l = 0; l < 3; ++l)
      for (int s = 0; s < 40; ++s)
        for (int c = 0; c < 90; ++c)
          deck_cards[l][s][c] = gen();

    for (int l = 0; l < 3; ++l)
      for (int size = 0; size < 41; ++size)
        deck_sizes[l][size] = gen();

    for (int p = 0; p < 2; ++p) {
      for (int points = 0; points < 256; ++points)
        player_points[p][points] = gen();
      for (int count = 0; count < 4; ++count)
        player_reserved_count[p][count] = gen();
      for (int count = 0; count < 91; ++count)
        player_purchased_count[p][count] = gen();
      for (int slot = 0; slot < 3; ++slot)
        for (int hidden = 0; hidden < 2; ++hidden)
          reserved_is_hidden[p][slot][hidden] = gen();
    }

    std::mt19937_64 hidden_level_rng(0x48494444454e4c56ULL);
    auto gen_hidden_level = [&]() {
      return std::uniform_int_distribution<uint64_t>()(hidden_level_rng);
    };
    for (int player = 0; player < 2; ++player)
      for (int slot = 0; slot < 3; ++slot)
        for (int level = 0; level < 3; ++level)
          hidden_reserved_level[player][slot][level] = gen_hidden_level();

    final_round[0] = gen();
    final_round[1] = gen();
    for (int state = 0; state < 4; ++state)
      winner[state] = gen();
    for (int value = 0; value < 65536; ++value)
      turn[value] = gen();
  }
};

#endif // CSPLENDOR_ZOBRIST_H
