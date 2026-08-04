#ifndef CSPLENDOR_BOARD_H
#define CSPLENDOR_BOARD_H

#include "card_data.h"
#include "noble_data.h"
#include "player.h"
#include "portable_rng.h"
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
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));

    // Gems
    for (int i = 0; i < 5; ++i)
      bank[i] = GEMS_PER_COLOR;
    bank[GOLD] = NUM_GOLD;

    // Decks
    for (int i = 0; i < CARD_COUNT; ++i) {
      decks[CARDS[i].level - 1].push_back(CARDS[i].id);
    }
    for (int i = 0; i < 3; ++i) {
      portable_mt19937_shuffle(decks[i].begin(), decks[i].end(), rng);
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
      all_nobles[i] = static_cast<uint8_t>(i);
    portable_mt19937_shuffle(all_nobles.begin(), all_nobles.end(), rng);
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

    cached_hash = compute_hash_uncached();
    hash_valid = true;
    return cached_hash;
  }

  // Pure full-information position-hash computation.  Caching belongs in
  // hash(), so callers can use this as a validation oracle.
  uint64_t compute_hash_uncached() const {
    const auto &z = Zobrist::get_instance();
    uint64_t h = 0;

    // Bank
    for (int i = 0; i < 6; ++i) {
      if (bank[i] < 13)
        h ^= z.bank_gems[i][bank[i]];
      else
        h ^= hash_out_of_range_value(0x100 + i, bank[i]);
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
    for (size_t slot = 0; slot < nobles.size(); ++slot) {
      const uint8_t n_id = nobles[slot];
      if (is_valid_noble_id(n_id))
        h ^= z.nobles_on_board[slot][n_id];
    }

    // Deck order determines which cards can be revealed next.  Hash every
    // occupied stack position, rather than only the deck size.
    for (int l = 0; l < 3; ++l) {
      for (size_t s = 0; s < decks[l].size(); ++s) {
        const uint8_t card_id = decks[l][s];
        if (is_valid_card_id(card_id))
          h ^= z.deck_cards[l][s][card_id];
      }
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      h ^= z.player_points[i][p.points];
      if (p.reserved_count <= MAX_RESERVED)
        h ^= z.player_reserved_count[i][p.reserved_count];
      else
        h ^= hash_out_of_range_value(0x200 + i, p.reserved_count);
      if (p.purchased_count <= CARD_COUNT)
        h ^= z.player_purchased_count[i][p.purchased_count];
      else
        h ^= hash_out_of_range_value(0x210 + i, p.purchased_count);
      for (int g = 0; g < 6; ++g) {
        if (p.gems[g] < 13)
          h ^= z.player_gems[i][g][p.gems[g]];
        else
          h ^= hash_out_of_range_value(0x300 + i * 8 + g, p.gems[g]);
      }
      for (int b = 0; b < 5; ++b) {
        if (p.bonuses[b] < 16)
          h ^= z.player_bonuses[i][b][p.bonuses[b]];
        else
          h ^= hash_out_of_range_value(0x400 + i * 8 + b, p.bonuses[b]);
      }
      for (int r = 0; r < 3; ++r) {
        int card_idx = static_cast<int>(p.reserved[r]) + 1;
        if (card_idx >= 0 && card_idx <= CARD_COUNT)
          h ^= z.cards_reserved[i][r][card_idx];
        else
          h ^= hash_out_of_range_value(
              0x500 + i * 4 + r, static_cast<uint8_t>(p.reserved[r]));
        h ^= z.reserved_is_hidden[i][r][p.reserved_is_hidden[r] ? 1 : 0];
      }
    }

    // Current player & states
    if (current_player < NUM_PLAYERS)
      h ^= z.current_player[current_player];
    if (waiting_noble && current_player < NUM_PLAYERS)
      h ^= z.waiting_noble[current_player];
    h ^= z.final_round[final_round ? 1 : 0];
    if (winner >= -2 && winner <= 1)
      h ^= z.winner[winner + 2];
    h ^= z.turn[turn];

    return h;
  }

  // Compute hash from scratch (for debugging/validation)
  uint64_t recompute_hash() const {
    const uint64_t recomputed = compute_hash_uncached();
    cached_hash = recomputed;
    hash_valid = true;
    return recomputed;
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
      else
        h ^= hash_out_of_range_value(0x100 + i, bank[i]);
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
      h ^= z.deck_sizes[l][decks[l].size()];
    }

    // Nobles - always visible
    for (size_t slot = 0; slot < nobles.size(); ++slot) {
      const uint8_t n_id = nobles[slot];
      if (is_valid_noble_id(n_id))
        h ^= z.nobles_on_board[slot][n_id];
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      h ^= z.player_points[i][p.points];
      if (p.reserved_count <= MAX_RESERVED)
        h ^= z.player_reserved_count[i][p.reserved_count];
      else
        h ^= hash_out_of_range_value(0x200 + i, p.reserved_count);
      if (p.purchased_count <= CARD_COUNT)
        h ^= z.player_purchased_count[i][p.purchased_count];
      else
        h ^= hash_out_of_range_value(0x210 + i, p.purchased_count);
      for (int g = 0; g < 6; ++g) {
        if (p.gems[g] < 13)
          h ^= z.player_gems[i][g][p.gems[g]];
        else
          h ^= hash_out_of_range_value(0x300 + i * 8 + g, p.gems[g]);
      }
      for (int b = 0; b < 5; ++b) {
        if (p.bonuses[b] < 16)
          h ^= z.player_bonuses[i][b][p.bonuses[b]];
        else
          h ^= hash_out_of_range_value(0x400 + i * 8 + b, p.bonuses[b]);
      }
      // Reserved cards - only include if visible to observer
      for (int r = 0; r < 3; ++r) {
        if (i == observer || !p.reserved_is_hidden[r]) {
          // Observer can see their own reserved cards
          int card_idx = static_cast<int>(p.reserved[r]) + 1;
          if (card_idx >= 0 && card_idx <= CARD_COUNT)
            h ^= z.cards_reserved[i][r][card_idx];
          else
            h ^= hash_out_of_range_value(
                0x500 + i * 4 + r, static_cast<uint8_t>(p.reserved[r]));
        } else {
          // The hidden card ID is private, while the tier is public and is
          // also present in StateEncoder.  Hash the slot/tier signature so two
          // different public information sets cannot alias in the MCTS tree.
          if (p.reserved[r] != -1) {
            h ^= z.cards_reserved[i][r][CARD_COUNT + 1];
            if (is_valid_card_id(p.reserved[r])) {
              const int level = get_card(p.reserved[r]).level;
              if (level >= 1 && level <= 3)
                h ^= z.hidden_reserved_level[i][r][level - 1];
            }
          }
        }
      }
    }

    // Current player & states
    if (current_player < NUM_PLAYERS)
      h ^= z.current_player[current_player];
    if (waiting_noble && current_player < NUM_PLAYERS)
      h ^= z.waiting_noble[current_player];
    h ^= z.final_round[final_round ? 1 : 0];
    if (winner >= -2 && winner <= 1)
      h ^= z.winner[winner + 2];
    h ^= z.turn[turn];

    return h;
  }

  // Observable rule-position identity for repetition detection.  The regular
  // observable hash intentionally includes the monotonic turn counter for
  // MCTS tree identity; repetition logic must not treat that metadata as a
  // material position change.
  uint64_t observable_repetition_hash(uint8_t observer) const {
    const auto &z = Zobrist::get_instance();
    return observable_hash(observer) ^ z.turn[turn];
  }

  std::vector<uint8_t> observable_card_pool(uint8_t observer,
                                            int level) const {
    if (observer >= NUM_PLAYERS)
      throw std::invalid_argument("observer must identify a player");
    if (level < 1 || level > 3)
      throw std::invalid_argument("card level must be in [1, 3]");

    const auto &deck = decks[level - 1];
    std::vector<uint8_t> result(deck.begin(), deck.end());
    const PlayerState &opponent = players[1 - observer];
    for (int slot = 0; slot < MAX_RESERVED; ++slot) {
      const int card_id = opponent.reserved[slot];
      if (opponent.reserved_is_hidden[slot] && is_valid_card_id(card_id) &&
          get_card(card_id).level == level)
        result.push_back(static_cast<uint8_t>(card_id));
    }
    std::sort(result.begin(), result.end());
    return result;
  }

  void randomize_hidden_information(uint8_t observer_player, uint64_t seed) {
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));
    randomize_hidden_information_impl(
        observer_player, [&rng](auto first, auto last) {
          portable_mt19937_shuffle(first, last, rng);
        });
  }

  void randomize_hidden_information_portable(uint8_t observer_player,
                                              uint64_t seed) {
    PortableRng rng(seed);
    randomize_hidden_information_impl(
        observer_player, [&rng](auto first, auto last) {
          portable_shuffle(first, last, rng);
        });
  }

private:
  template <typename Shuffle>
  void randomize_hidden_information_impl(uint8_t observer_player,
                                         Shuffle shuffle) {
    if (observer_player >= NUM_PLAYERS)
      return;

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
      shuffle(pool.begin(), pool.end());

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
  // Canonical reachable values keep their existing Zobrist tables.  Python's
  // public editor accepts the wider uint8_t domain, so values outside those
  // tables need a field-tagged fallback instead of silently contributing no
  // salt.  SplitMix64's avalanche keeps the fallback deterministic and avoids
  // consuming/changing the canonical Zobrist RNG stream.
  static uint64_t hash_out_of_range_value(uint64_t tag, uint64_t value) {
    uint64_t x = 0x9e3779b97f4a7c15ULL ^
                 (tag * 0xbf58476d1ce4e5b9ULL) ^ value;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
  }
};

#endif // CSPLENDOR_BOARD_H
