#ifndef CSPLENDOR_BOARD_H
#define CSPLENDOR_BOARD_H

#include "card_data.h"
#include "noble_data.h"
#include "player.h"
#include "types.h"
#include "zobrist.h"
#include <algorithm>
#include <array>
#include <random>
#include <sstream>
#include <string>
#include <vector>

// Fixed-size stack for deck/noble storage - avoids heap allocations
template <typename T, size_t MaxSize> struct FixedStack {
  std::array<T, MaxSize> data;
  uint8_t count = 0;

  void clear() { count = 0; }
  bool empty() const { return count == 0; }
  size_t size() const { return count; }

  void push_back(T val) {
    if (count < MaxSize)
      data[count++] = val;
  }

  void pop_back() {
    if (count > 0)
      --count;
  }

  T back() const { return data[count - 1]; }
  T &back() { return data[count - 1]; }

  T *begin() { return data.data(); }
  T *end() { return data.data() + count; }
  const T *begin() const { return data.data(); }
  const T *end() const { return data.data() + count; }

  T &operator[](size_t i) { return data[i]; }
  const T &operator[](size_t i) const { return data[i]; }

  // For erase/remove compatibility
  void erase(T *it) {
    if (it >= begin() && it < end()) {
      std::move(it + 1, end(), it);
      --count;
    }
  }

  // Remove element by value
  void remove(T val) {
    auto it = std::find(begin(), end(), val);
    if (it != end()) {
      erase(it);
    }
  }
};

class Board {
public:
  static constexpr int NUM_PLAYERS = 2;
  static constexpr int GEMS_PER_COLOR = 4;
  static constexpr int NUM_GOLD = 5;
  static constexpr int NUM_NOBLES = 3;
  static constexpr int MAX_TOKENS = 10;
  static constexpr int MAX_RESERVED = 3;
  static constexpr int CARDS_PER_LEVEL = 4;
  static constexpr int MAX_DECK_SIZE = 40; // Max cards per level (Level 1 has 40)
  static constexpr int MAX_NOBLES_ON_BOARD = NOBLE_COUNT;

  std::array<uint8_t, 6> bank = {0};
  std::array<std::array<int8_t, CARDS_PER_LEVEL>, 3> visible = {
      {{{-1, -1, -1, -1}}, {{-1, -1, -1, -1}}, {{-1, -1, -1, -1}}}};
  FixedStack<uint8_t, MAX_DECK_SIZE> decks[3]; // [level] - fixed-size, no heap allocation
  FixedStack<uint8_t, MAX_NOBLES_ON_BOARD> nobles; // IDs of nobles on board
  PlayerState players[2];
  uint8_t current_player = 0;
  uint16_t turn = 0;
  bool final_round = false;
  bool waiting_noble = false;
  int8_t winner = -1; // -1: ongoing, 0, 1: player, -2: draw

  // Incremental Zobrist hash support
  mutable uint64_t cached_hash = 0;
  mutable bool hash_valid = false;

  void init(uint64_t seed) {
    reset();
    std::mt19937 rng(seed);

    // Gems
    for (int i = 0; i < 5; ++i)
      bank[i] = GEMS_PER_COLOR;
    bank[GOLD] = NUM_GOLD;

    // Decks
    for (int i = 0; i < CARD_COUNT; ++i) {
      decks[CARDS[i].level - 1].push_back(CARDS[i].id);
    }
    for (int i = 0; i < 3; ++i) {
      std::shuffle(decks[i].begin(), decks[i].end(), rng);
    }

    // Visible cards
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        if (!decks[l].empty()) {
          visible[l][s] = decks[l].back();
          decks[l].pop_back();
        } else {
          visible[l][s] = -1;
        }
      }
    }

    // Nobles - use fixed-size array for shuffling
    std::array<uint8_t, NOBLE_COUNT> all_nobles;
    for (int i = 0; i < NOBLE_COUNT; ++i)
      all_nobles[i] = i;
    std::shuffle(all_nobles.begin(), all_nobles.end(), rng);
    for (int i = 0; i < NUM_NOBLES; ++i) {
      nobles.push_back(all_nobles[i]);
    }
  }

  void reset() {
    for (int i = 0; i < 6; ++i)
      bank[i] = 0;
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s)
        visible[l][s] = -1;
      decks[l].clear();
    }
    nobles.clear();
    players[0] = PlayerState();
    players[1] = PlayerState();
    current_player = 0;
    turn = 0;
    final_round = false;
    waiting_noble = false;
    winner = -1;
    hash_valid = false;
  }

  // Invalidate cached hash - must be called after any state modification
  void invalidate_hash() { hash_valid = false; }

  bool is_game_over() const { return winner != -1; }

  std::string to_string() const {
    std::stringstream ss;
    ss << "Turn " << turn << ", Player " << (int)current_player << "'s turn"
       << std::endl;
    ss << "Bank: [E:" << (int)bank[0] << " S:" << (int)bank[1]
       << " R:" << (int)bank[2] << " D:" << (int)bank[3]
       << " O:" << (int)bank[4] << " G:" << (int)bank[5] << "]" << std::endl;

    ss << "Nobles: [";
    for (size_t i = 0; i < nobles.size(); ++i) {
      ss << "N" << (int)nobles[i] << (i == nobles.size() - 1 ? "" : " ");
    }
    ss << "]" << std::endl;

    for (int l = 2; l >= 0; --l) {
      ss << "Level " << (l + 1) << ": [";
      for (int s = 0; s < 4; ++s) {
        if (visible[l][s] != -1)
          ss << "C" << (int)visible[l][s];
        else
          ss << "--";
        ss << (s == 3 ? "" : " ");
      }
      ss << "] (" << decks[l].size() << " left)" << std::endl;
    }

    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      ss << "--- Player " << i << " (" << (int)p.points << "pts) ---"
         << std::endl;
      ss << "Gems: [E:" << (int)p.gems[0] << " S:" << (int)p.gems[1]
         << " R:" << (int)p.gems[2] << " D:" << (int)p.gems[3]
         << " O:" << (int)p.gems[4] << " G:" << (int)p.gems[5] << "] ("
         << p.total_gems() << ")" << std::endl;
      ss << "Bonuses: [E:" << (int)p.bonuses[0] << " S:" << (int)p.bonuses[1]
         << " R:" << (int)p.bonuses[2] << " D:" << (int)p.bonuses[3]
         << " O:" << (int)p.bonuses[4] << "]" << std::endl;
      ss << "Reserved: [";
      for (int r = 0; r < 3; ++r) {
        if (p.reserved[r] != -1)
          ss << "C" << (int)p.reserved[r];
        else
          ss << "--";
        ss << (r == 2 ? "" : " ");
      }
      ss << "]" << std::endl;
    }

    return ss.str();
  }

  uint64_t hash() const {
    if (hash_valid) {
      return cached_hash;
    }

    const auto &z = Zobrist::get_instance();
    uint64_t h = 0;

    // Bank
    for (int i = 0; i < 6; ++i) {
      if (bank[i] < 13)
        h ^= z.bank_gems[i][bank[i]];
    }

    // Visible cards
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        int card_idx = static_cast<int>(visible[l][s]) + 1;
        if (card_idx >= 0 && card_idx <= CARD_COUNT)
          h ^= z.cards_board[l][s][card_idx];
      }
    }

    // Nobles
    for (uint8_t n_id : nobles) {
      if (is_valid_noble_id(n_id))
        h ^= z.nobles_on_board[n_id];
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      for (int g = 0; g < 6; ++g) {
        if (p.gems[g] < 13)
          h ^= z.player_gems[i][g][p.gems[g]];
      }
      for (int b = 0; b < 5; ++b) {
        if (p.bonuses[b] < 16)
          h ^= z.player_bonuses[i][b][p.bonuses[b]];
      }
      for (int r = 0; r < 3; ++r) {
        int card_idx = static_cast<int>(p.reserved[r]) + 1;
        if (card_idx >= 0 && card_idx <= CARD_COUNT)
          h ^= z.cards_reserved[i][r][card_idx];
      }
    }

    // Current player & states
    if (current_player < NUM_PLAYERS)
      h ^= z.current_player[current_player];
    if (waiting_noble && current_player < NUM_PLAYERS)
      h ^= z.waiting_noble[current_player];

    // Cache the result
    cached_hash = h;
    hash_valid = true;

    return h;
  }

  // Compute hash from scratch (for debugging/validation)
  uint64_t recompute_hash() const {
    hash_valid = false;
    return hash();
  }

  // Observable hash - only includes information visible to the observer
  // Used for MCTS determinization to avoid different hashes for same observable state
  uint64_t observable_hash(uint8_t observer) const {
    const auto &z = Zobrist::get_instance();
    uint64_t h = 0;

    // Bank - always visible
    for (int i = 0; i < 6; ++i) {
      if (bank[i] < 13)
        h ^= z.bank_gems[i][bank[i]];
    }

    // Visible cards - always visible
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        int card_idx = static_cast<int>(visible[l][s]) + 1;
        if (card_idx >= 0 && card_idx <= CARD_COUNT)
          h ^= z.cards_board[l][s][card_idx];
      }
    }

    // Deck sizes only (not contents) - visible information
    for (int l = 0; l < 3; ++l) {
      // Use deck size as a proxy (XOR with a unique value per size)
      h ^= z.bank_gems[l][std::min((int)decks[l].size(), 12)];
    }

    // Nobles - always visible
    for (uint8_t n_id : nobles) {
      if (is_valid_noble_id(n_id))
        h ^= z.nobles_on_board[n_id];
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      for (int g = 0; g < 6; ++g) {
        if (p.gems[g] < 13)
          h ^= z.player_gems[i][g][p.gems[g]];
      }
      for (int b = 0; b < 5; ++b) {
        if (p.bonuses[b] < 16)
          h ^= z.player_bonuses[i][b][p.bonuses[b]];
      }
      // Reserved cards - only include if visible to observer
      for (int r = 0; r < 3; ++r) {
        if (i == observer || !p.reserved_is_hidden[r]) {
          // Observer can see their own reserved cards
          int card_idx = static_cast<int>(p.reserved[r]) + 1;
          if (card_idx >= 0 && card_idx <= CARD_COUNT)
            h ^= z.cards_reserved[i][r][card_idx];
        } else {
          // Hidden reserved cards - just mark as "something reserved"
          if (p.reserved[r] != -1) {
            h ^= z.cards_reserved[i][r][CARD_COUNT + 1];
          }
        }
      }
    }

    // Current player & states
    if (current_player < NUM_PLAYERS)
      h ^= z.current_player[current_player];
    if (waiting_noble && current_player < NUM_PLAYERS)
      h ^= z.waiting_noble[current_player];

    return h;
  }

  void randomize_hidden_information(uint8_t observer_player, uint64_t seed) {
    if (observer_player >= NUM_PLAYERS)
      return;

    std::mt19937 rng(seed);
    uint8_t opponent = 1 - observer_player;
    auto &p_opp = players[opponent];

    for (int l = 0; l < 3; ++l) {
      // Copy deck to pool using fixed-size array
      FixedStack<uint8_t, MAX_DECK_SIZE + 3> pool; // +3 for possible reserved
      for (size_t i = 0; i < decks[l].size(); ++i) {
        pool.push_back(decks[l][i]);
      }
      std::array<int, 3> reserved_indices;
      int reserved_count = 0;

      // Collect hidden reserved cards of this level
      for (int i = 0; i < 3; ++i) {
        if (is_valid_card_id(p_opp.reserved[i]) && p_opp.reserved_is_hidden[i]) {
          if (get_card(p_opp.reserved[i]).level == l + 1) {
            pool.push_back(p_opp.reserved[i]);
            reserved_indices[reserved_count++] = i;
          }
        }
      }

      if (pool.empty())
        continue;

      // Shuffle the pool
      std::shuffle(pool.begin(), pool.end(), rng);

      // Redistribute
      for (int ri = 0; ri < reserved_count; ++ri) {
        int idx = reserved_indices[ri];
        p_opp.reserved[idx] = pool.back();
        pool.pop_back();
      }

      // Copy back to deck
      decks[l].clear();
      for (size_t i = 0; i < pool.size(); ++i) {
        decks[l].push_back(pool[i]);
      }
    }

    // Invalidate hash since state changed
    hash_valid = false;
  }
};

#endif // CSPLENDOR_BOARD_H
