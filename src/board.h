#ifndef CSPLENDOR_BOARD_H
#define CSPLENDOR_BOARD_H

#include "card_data.h"
#include "fixed_stack.h"
#include "noble_data.h"
#include "perf_counters.h"
#include "player.h"
#include "portable_rng.h"
#include "types.h"
#include "zobrist.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace csplendor::solver_internal {
class RevealSearchState;
}

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

  // A successful trusted rule transition can preserve an already-computed
  // exact hash without changing Board's public field layout.  The mutator
  // keeps the candidate hash on the stack and publishes it only on commit;
  // an early return or exception therefore leaves the cache invalid.
  template <bool MaintainExactHash> class RuleMutator {
  public:
    explicit RuleMutator(Board &board) noexcept : board_(&board) {
      if constexpr (MaintainExactHash) {
#ifdef CSPLENDOR_VERIFY_INCREMENTAL_HASH
        if (!board.hash_valid)
          std::abort();
#endif
        hash_ = board.cached_hash;
        zobrist_ = &Zobrist::get_instance();
      }
      board.invalidate_hash();
    }

    RuleMutator(const RuleMutator &) = delete;
    RuleMutator &operator=(const RuleMutator &) = delete;

    RuleMutator(RuleMutator &&other) noexcept
        : board_(other.board_), hash_(other.hash_), zobrist_(other.zobrist_),
          committed_(other.committed_) {
      other.board_ = nullptr;
    }

    RuleMutator &operator=(RuleMutator &&) = delete;

    ~RuleMutator() {
      if (board_ != nullptr && !committed_)
        board_->invalidate_hash();
    }

    void set_bank(int color, uint8_t value) noexcept {
      const uint8_t old = board_->bank[color];
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_bank_salt(*zobrist_, color, old) ^
                 Board::exact_bank_salt(*zobrist_, color, value);
      }
      board_->bank[color] = value;
    }

    void set_visible(int level, int slot, int8_t card_id) noexcept {
      const int8_t old = board_->visible[level][slot];
      if (old == card_id)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_visible_salt(*zobrist_, level, slot, old) ^
                 Board::exact_visible_salt(*zobrist_, level, slot, card_id);
      }
      board_->visible[level][slot] = card_id;
    }

    void set_player_gem(int player, int color, uint8_t value) noexcept {
      const uint8_t old = board_->players[player].gems[color];
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_player_gem_salt(*zobrist_, player, color, old) ^
                 Board::exact_player_gem_salt(*zobrist_, player, color, value);
      }
      board_->players[player].gems[color] = value;
    }

    void set_player_bonus(int player, int color, uint8_t value) noexcept {
      const uint8_t old = board_->players[player].bonuses[color];
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_player_bonus_salt(*zobrist_, player, color, old) ^
            Board::exact_player_bonus_salt(*zobrist_, player, color, value);
      }
      board_->players[player].bonuses[color] = value;
    }

    void set_player_points(int player, uint8_t value) noexcept {
      const uint8_t old = board_->players[player].points;
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_player_points_salt(*zobrist_, player, old) ^
                 Board::exact_player_points_salt(*zobrist_, player, value);
      }
      board_->players[player].points = value;
    }

    void set_player_reserved_count(int player, uint8_t value) noexcept {
      const uint8_t old = board_->players[player].reserved_count;
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_player_reserved_count_salt(*zobrist_, player, old) ^
            Board::exact_player_reserved_count_salt(*zobrist_, player, value);
      }
      board_->players[player].reserved_count = value;
    }

    void set_player_purchased_count(int player, uint8_t value) noexcept {
      const uint8_t old = board_->players[player].purchased_count;
      if (old == value)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_player_purchased_count_salt(*zobrist_, player, old) ^
            Board::exact_player_purchased_count_salt(*zobrist_, player, value);
      }
      board_->players[player].purchased_count = value;
    }

    void set_player_reserved(int player, int slot, int8_t card_id) noexcept {
      const int8_t old = board_->players[player].reserved[slot];
      if (old == card_id)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_player_reserved_salt(*zobrist_, player, slot, old) ^
            Board::exact_player_reserved_salt(*zobrist_, player, slot, card_id);
      }
      board_->players[player].reserved[slot] = card_id;
    }

    void set_player_reserved_hidden(int player, int slot,
                                    bool hidden) noexcept {
      const bool old = board_->players[player].reserved_is_hidden[slot];
      if (old == hidden)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_player_reserved_hidden_salt(*zobrist_, player,
                                                          slot, old) ^
                 Board::exact_player_reserved_hidden_salt(*zobrist_, player,
                                                          slot, hidden);
      }
      board_->players[player].reserved_is_hidden[slot] = hidden;
    }

    uint8_t pop_deck(int level) noexcept {
      const size_t position = board_->decks[level].size() - 1;
      const uint8_t card_id = board_->decks[level].back();
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_deck_card_salt(*zobrist_, level, position, card_id);
      }
      board_->decks[level].pop_back();
      return card_id;
    }

    void push_deck_unchecked(int level, uint8_t card_id) noexcept {
      const size_t position = board_->decks[level].size();
      if constexpr (MaintainExactHash) {
        hash_ ^=
            Board::exact_deck_card_salt(*zobrist_, level, position, card_id);
      }
      board_->decks[level].push_back_unchecked(card_id);
    }

    void remove_noble(uint8_t noble_id) noexcept {
      if constexpr (MaintainExactHash) {
        for (size_t slot = 0; slot < board_->nobles.size(); ++slot)
          hash_ ^=
              Board::exact_noble_salt(*zobrist_, slot, board_->nobles[slot]);
      }
      board_->nobles.remove(noble_id);
      if constexpr (MaintainExactHash) {
        for (size_t slot = 0; slot < board_->nobles.size(); ++slot)
          hash_ ^=
              Board::exact_noble_salt(*zobrist_, slot, board_->nobles[slot]);
      }
    }

    void set_current_player(uint8_t player) noexcept {
      const uint8_t old = board_->current_player;
      if (old == player)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_current_player_salt(*zobrist_, old) ^
                 Board::exact_waiting_noble_salt(*zobrist_,
                                                 board_->waiting_noble, old) ^
                 Board::exact_current_player_salt(*zobrist_, player) ^
                 Board::exact_waiting_noble_salt(*zobrist_,
                                                 board_->waiting_noble, player);
      }
      board_->current_player = player;
    }

    void set_turn(uint16_t turn) noexcept {
      const uint16_t old = board_->turn;
      if (old == turn)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_turn_salt(*zobrist_, old) ^
                 Board::exact_turn_salt(*zobrist_, turn);
      }
      board_->turn = turn;
    }

    void set_final_round(bool final_round) noexcept {
      const bool old = board_->final_round;
      if (old == final_round)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_final_round_salt(*zobrist_, old) ^
                 Board::exact_final_round_salt(*zobrist_, final_round);
      }
      board_->final_round = final_round;
    }

    void set_waiting_noble(bool waiting) noexcept {
      const bool old = board_->waiting_noble;
      if (old == waiting)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_waiting_noble_salt(*zobrist_, old,
                                                 board_->current_player) ^
                 Board::exact_waiting_noble_salt(*zobrist_, waiting,
                                                 board_->current_player);
      }
      board_->waiting_noble = waiting;
    }

    void set_winner(int8_t winner) noexcept {
      const int8_t old = board_->winner;
      if (old == winner)
        return;
      if constexpr (MaintainExactHash) {
        hash_ ^= Board::exact_winner_salt(*zobrist_, old) ^
                 Board::exact_winner_salt(*zobrist_, winner);
      }
      board_->winner = winner;
    }

    void commit() noexcept {
      if constexpr (MaintainExactHash) {
        board_->cached_hash = hash_;
        board_->hash_valid = true;
      } else {
        board_->invalidate_hash();
      }

#ifdef CSPLENDOR_VERIFY_INCREMENTAL_HASH
      const uint64_t oracle = board_->compute_hash_uncached();
      const uint64_t cached_before_fallback = board_->cached_hash;
      const bool valid_before_fallback = board_->hash_valid;
      const uint64_t maintained =
          valid_before_fallback ? board_->cached_hash : board_->hash();
      if (maintained != oracle) {
        CSPLENDOR_PERF_INC(HashOracleFailures);
        std::abort();
      }
      if (!valid_before_fallback) {
        board_->cached_hash = cached_before_fallback;
        board_->hash_valid = false;
      }
#endif
      committed_ = true;
    }

  private:
    Board *board_ = nullptr;
    uint64_t hash_ = 0;
    const Zobrist *zobrist_ = nullptr;
    bool committed_ = false;
  };

  void init(uint64_t seed) {
    reset();
    std::mt19937 rng(static_cast<std::mt19937::result_type>(seed));

    // Gems
    for (int i = 0; i < 5; ++i)
      bank[i] = GEMS_PER_COLOR;
    bank[GOLD] = NUM_GOLD;

    // Decks
    for (int i = 0; i < CARD_COUNT; ++i) {
      decks[CARDS[i].level - 1].push_back_unchecked(CARDS[i].id);
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
      nobles.push_back_unchecked(all_nobles[i]);
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
  void invalidate_hash() noexcept { hash_valid = false; }

  // Named mutation gateways make the caller's trust boundary explicit. They
  // intentionally share the same zero-cost invalidation operation; editor
  // callers validate a complete payload before entering, while trusted rule
  // transitions validate at a higher boundary or deliberately allow partial
  // mutation for rollback-based search.
  Board &begin_editor_mutation() noexcept {
    invalidate_hash();
    return *this;
  }

  Board &begin_unchecked_mutation() noexcept {
    invalidate_hash();
    return *this;
  }

  bool is_game_over() const { return winner != -1; }

  std::string to_string() const {
    std::stringstream ss;
    ss << "Turn " << turn << ", Player " << (int)current_player << "'s turn"
       << std::endl;
    ss << "Bank: [D:" << (int)bank[0] << " S:" << (int)bank[1]
       << " E:" << (int)bank[2] << " R:" << (int)bank[3]
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
      ss << "Gems: [D:" << (int)p.gems[0] << " S:" << (int)p.gems[1]
         << " E:" << (int)p.gems[2] << " R:" << (int)p.gems[3]
         << " O:" << (int)p.gems[4] << " G:" << (int)p.gems[5] << "] ("
         << p.total_gems() << ")" << std::endl;
      ss << "Bonuses: [D:" << (int)p.bonuses[0] << " S:" << (int)p.bonuses[1]
         << " E:" << (int)p.bonuses[2] << " R:" << (int)p.bonuses[3]
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
    CSPLENDOR_PERF_INC(ExactHashCalls);
    if (hash_valid) {
      CSPLENDOR_PERF_INC(ExactHashCacheHits);
      return cached_hash;
    }

    CSPLENDOR_PERF_INC(ExactHashCacheMisses);
    cached_hash = compute_hash_uncached();
    hash_valid = true;
    return cached_hash;
  }

  // Pure full-information position-hash computation.  Caching belongs in
  // hash(), so callers can use this as a validation oracle.
  uint64_t compute_hash_uncached() const {
    return compute_hash_impl<true, true>();
  }

  // Canonical rule-state hash for searches that model hidden decks as sets.
  // The caller must key the remaining card set separately.  Excluding both
  // deck order and the absolute turn lets equivalent reveal histories share
  // a transposition while retaining every field that affects legal play.
  uint64_t compute_set_deck_search_hash() const {
    CSPLENDOR_PERF_INC(SolverSetDeckHashCalls);
    return compute_hash_impl<false, false>();
  }

private:
  friend class csplendor::solver_internal::RevealSearchState;

  static uint64_t exact_bank_salt(const Zobrist &z, int color,
                                  uint8_t value) noexcept {
    if (value < 13)
      return z.bank_gems[color][value];
    return hash_out_of_range_value(0x100 + color, value);
  }

  static uint64_t exact_visible_salt(const Zobrist &z, int level, int slot,
                                     int8_t card_id) noexcept {
    const int card_index = static_cast<int>(card_id) + 1;
    if (card_index >= 0 && card_index <= CARD_COUNT)
      return z.cards_board[level][slot][card_index];
    return 0;
  }

  static uint64_t exact_noble_salt(const Zobrist &z, size_t slot,
                                   uint8_t noble_id) noexcept {
    if (is_valid_noble_id(noble_id))
      return z.nobles_on_board[slot][noble_id];
    return 0;
  }

  static uint64_t exact_deck_card_salt(const Zobrist &z, int level,
                                       size_t position,
                                       uint8_t card_id) noexcept {
    if (is_valid_card_id(card_id))
      return z.deck_cards[level][position][card_id];
    return 0;
  }

  static uint64_t exact_player_points_salt(const Zobrist &z, int player,
                                           uint8_t points) noexcept {
    return z.player_points[player][points];
  }

  static uint64_t exact_player_reserved_count_salt(const Zobrist &z, int player,
                                                   uint8_t count) noexcept {
    if (count <= MAX_RESERVED)
      return z.player_reserved_count[player][count];
    return hash_out_of_range_value(0x200 + player, count);
  }

  static uint64_t exact_player_purchased_count_salt(const Zobrist &z,
                                                    int player,
                                                    uint8_t count) noexcept {
    if (count <= CARD_COUNT)
      return z.player_purchased_count[player][count];
    return hash_out_of_range_value(0x210 + player, count);
  }

  static uint64_t exact_player_gem_salt(const Zobrist &z, int player, int color,
                                        uint8_t value) noexcept {
    if (value < 13)
      return z.player_gems[player][color][value];
    return hash_out_of_range_value(0x300 + player * 8 + color, value);
  }

  static uint64_t exact_player_bonus_salt(const Zobrist &z, int player,
                                          int color, uint8_t value) noexcept {
    if (value < 16)
      return z.player_bonuses[player][color][value];
    return hash_out_of_range_value(0x400 + player * 8 + color, value);
  }

  static uint64_t exact_player_reserved_salt(const Zobrist &z, int player,
                                             int slot,
                                             int8_t card_id) noexcept {
    const int card_index = static_cast<int>(card_id) + 1;
    if (card_index >= 0 && card_index <= CARD_COUNT)
      return z.cards_reserved[player][slot][card_index];
    return hash_out_of_range_value(0x500 + player * 4 + slot,
                                   static_cast<uint8_t>(card_id));
  }

  static uint64_t exact_player_reserved_hidden_salt(const Zobrist &z,
                                                    int player, int slot,
                                                    bool hidden) noexcept {
    return z.reserved_is_hidden[player][slot][hidden ? 1 : 0];
  }

  static uint64_t exact_current_player_salt(const Zobrist &z,
                                            uint8_t player) noexcept {
    if (player < NUM_PLAYERS)
      return z.current_player[player];
    return 0;
  }

  static uint64_t exact_waiting_noble_salt(const Zobrist &z, bool waiting,
                                           uint8_t player) noexcept {
    if (waiting && player < NUM_PLAYERS)
      return z.waiting_noble[player];
    return 0;
  }

  static uint64_t exact_final_round_salt(const Zobrist &z,
                                         bool final_round) noexcept {
    return z.final_round[final_round ? 1 : 0];
  }

  static uint64_t exact_winner_salt(const Zobrist &z, int8_t winner) noexcept {
    if (winner >= -2 && winner <= 1)
      return z.winner[winner + 2];
    return 0;
  }

  static uint64_t exact_turn_salt(const Zobrist &z, uint16_t turn) noexcept {
    return z.turn[turn];
  }

  template <bool IncludeDeckOrder, bool IncludeTurn>
  uint64_t compute_hash_impl() const {
    const auto &z = Zobrist::get_instance();
    uint64_t h = 0;

    // Bank
    for (int i = 0; i < 6; ++i) {
      CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 1);
      h ^= exact_bank_salt(z, i, bank[i]);
    }

    // Visible cards
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 1);
        h ^= exact_visible_salt(z, l, s, visible[l][s]);
      }
    }

    // Nobles
    for (size_t slot = 0; slot < nobles.size(); ++slot) {
      CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 1);
      h ^= exact_noble_salt(z, slot, nobles[slot]);
    }

    if constexpr (IncludeDeckOrder) {
      // Deck order determines which cards can be revealed next.  Hash every
      // occupied stack position, rather than only the deck size.
      for (int l = 0; l < 3; ++l) {
        for (size_t s = 0; s < decks[l].size(); ++s) {
          CSPLENDOR_PERF_INC(ExactDeckCardSaltsVisited);
          h ^= exact_deck_card_salt(z, l, s, decks[l][s]);
        }
      }
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 3);
      h ^= exact_player_points_salt(z, i, p.points);
      h ^= exact_player_reserved_count_salt(z, i, p.reserved_count);
      h ^= exact_player_purchased_count_salt(z, i, p.purchased_count);
      for (int g = 0; g < 6; ++g) {
        CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 1);
        h ^= exact_player_gem_salt(z, i, g, p.gems[g]);
      }
      for (int b = 0; b < 5; ++b) {
        CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 1);
        h ^= exact_player_bonus_salt(z, i, b, p.bonuses[b]);
      }
      for (int r = 0; r < 3; ++r) {
        CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, 2);
        h ^= exact_player_reserved_salt(z, i, r, p.reserved[r]);
        h ^=
            exact_player_reserved_hidden_salt(z, i, r, p.reserved_is_hidden[r]);
      }
    }

    // Current player & states
    CSPLENDOR_PERF_HASH_FIELDS(IncludeDeckOrder, IncludeTurn ? 5 : 4);
    h ^= exact_current_player_salt(z, current_player);
    h ^= exact_waiting_noble_salt(z, waiting_noble, current_player);
    h ^= exact_final_round_salt(z, final_round);
    h ^= exact_winner_salt(z, winner);
    if constexpr (IncludeTurn)
      h ^= exact_turn_salt(z, turn);

    return h;
  }

public:

  // Compute hash from scratch (for debugging/validation)
  uint64_t recompute_hash() const {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    const bool had_cached_hash = hash_valid;
    const uint64_t previous_hash = cached_hash;
#endif
    const uint64_t recomputed = compute_hash_uncached();
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    if (had_cached_hash && previous_hash != recomputed)
      CSPLENDOR_PERF_INC(HashOracleFailures);
#endif
    cached_hash = recomputed;
    hash_valid = true;
    return recomputed;
  }

  // Observable hash - only includes information visible to the observer
  // Used for MCTS determinization to avoid different hashes for same observable state
  uint64_t observable_hash(uint8_t observer) const {
    CSPLENDOR_PERF_INC(ObservableHashCalls);
    const auto &z = Zobrist::get_instance();
    uint64_t h = 0;

    // Bank - always visible
    for (int i = 0; i < 6; ++i) {
      CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
      if (bank[i] < 13)
        h ^= z.bank_gems[i][bank[i]];
      else
        h ^= hash_out_of_range_value(0x100 + i, bank[i]);
    }

    // Visible cards - always visible
    for (int l = 0; l < 3; ++l) {
      for (int s = 0; s < 4; ++s) {
        CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
        int card_idx = static_cast<int>(visible[l][s]) + 1;
        if (card_idx >= 0 && card_idx <= CARD_COUNT)
          h ^= z.cards_board[l][s][card_idx];
      }
    }

    // Deck sizes only (not contents) - visible information
    for (int l = 0; l < 3; ++l) {
      CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
      h ^= z.deck_sizes[l][decks[l].size()];
    }

    // Nobles - always visible
    for (size_t slot = 0; slot < nobles.size(); ++slot) {
      CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
      const uint8_t n_id = nobles[slot];
      if (is_valid_noble_id(n_id))
        h ^= z.nobles_on_board[slot][n_id];
    }

    // Players
    for (int i = 0; i < 2; ++i) {
      const auto &p = players[i];
      CSPLENDOR_PERF_ADD(ObservableHashFieldsVisited, 3);
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
        CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
        if (p.gems[g] < 13)
          h ^= z.player_gems[i][g][p.gems[g]];
        else
          h ^= hash_out_of_range_value(0x300 + i * 8 + g, p.gems[g]);
      }
      for (int b = 0; b < 5; ++b) {
        CSPLENDOR_PERF_INC(ObservableHashFieldsVisited);
        if (p.bonuses[b] < 16)
          h ^= z.player_bonuses[i][b][p.bonuses[b]];
        else
          h ^= hash_out_of_range_value(0x400 + i * 8 + b, p.bonuses[b]);
      }
      // Reserved cards - only include if visible to observer
      for (int r = 0; r < 3; ++r) {
        CSPLENDOR_PERF_ADD(ObservableHashFieldsVisited, 2);
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
    CSPLENDOR_PERF_ADD(ObservableHashFieldsVisited, 5);
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
    for (const auto &deck : decks) {
      if (deck.count > deck.capacity())
        return;
    }

    begin_unchecked_mutation();

    uint8_t opponent = 1 - observer_player;
    auto &p_opp = players[opponent];

    for (int l = 0; l < 3; ++l) {
      // Copy deck to pool using fixed-size array
      FixedStack<uint8_t, MAX_DECK_SIZE + 3> pool; // +3 for possible reserved
      for (size_t i = 0; i < decks[l].size(); ++i) {
        pool.push_back_unchecked(decks[l][i]);
      }
      std::array<int, 3> reserved_indices;
      int reserved_count = 0;

      // Collect hidden reserved cards of this level
      for (int i = 0; i < 3; ++i) {
        if (is_valid_card_id(p_opp.reserved[i]) && p_opp.reserved_is_hidden[i]) {
          if (get_card(p_opp.reserved[i]).level == l + 1) {
            pool.push_back_unchecked(p_opp.reserved[i]);
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
        decks[l].push_back_unchecked(pool[i]);
      }
    }
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
