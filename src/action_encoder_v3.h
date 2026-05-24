#ifndef CSPLENDOR_ACTION_ENCODER_V3_H
#define CSPLENDOR_ACTION_ENCODER_V3_H

#include "action.h"
#include "card_data.h"
#include "game.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <cstdint>

/**
 * ActionEncoderV3 - ID-based action space encoder (3133 actions)
 *
 * Key differences from V2:
 * - PURCHASE actions are indexed by card ID (0-89) instead of slot position
 * - VISIT_NOBLE actions are indexed by noble ID (0-11) instead of slot position
 * This enables NN to learn card/noble-specific strategies without
 * redundant position-dependent representations.
 *
 * Uses multiset composition ranking for return encoding (same as V2).
 * Uses constrained graded lexicographic ranking for payment encoding.
 * See doc/action_space_v3.md for the complete specification.
 *
 * Action ID layout:
 *   TAKE_DIFFERENT:   10 combos  x  84 return patterns =  840 [0..839]
 *   TAKE_SAME:         5 colors  x  28 return patterns =  140 [840..979]
 *   RESERVE_VISIBLE:  12 slots   x   7 return patterns =   84 [980..1063]
 *   RESERVE_DECK:      3 levels  x   7 return patterns =   21 [1064..1084]
 *   PURCHASE:          90 cards  x  card-specific       = 2035 [1085..3119]
 *   VISIT_NOBLE:      12 noble IDs                      =   12 [3120..3131]
 *   PASS:              1                                       [3132]
 *   Total: 3133
 */
class ActionEncoderV3 {
public:
  // ─── Multiset coefficient table H(n,k) = C(n+k-1, k) ───
  // Used for return encoding (same as V2)
  static constexpr int H[7][6] = {
      {1, 0, 0, 0, 0, 0},      // n=0
      {1, 1, 1, 1, 1, 1},      // n=1
      {1, 2, 3, 4, 5, 6},      // n=2
      {1, 3, 6, 10, 15, 21},   // n=3
      {1, 4, 10, 20, 35, 56},  // n=4
      {1, 5, 15, 35, 70, 126}, // n=5
      {1, 6, 21, 56, 126, 252},// n=6
  };

  // Cumulative offset for return sum s (6 colors)
  static constexpr int RETURN_OFFSET[4] = {0, 1, 7, 28};

  // ─── Pattern counts per action type ───
  static constexpr int TAKE_DIFF_RETURN_PATTERNS = 84;  // ret 0-3, 6 colors
  static constexpr int TAKE_SAME_RETURN_PATTERNS = 28;  // ret 0-2, 6 colors
  static constexpr int RESERVE_RETURN_PATTERNS = 7;     // ret 0-1, 6 colors

  // Base action counts
  static constexpr int NUM_TAKE_DIFFERENT = 10;
  static constexpr int NUM_TAKE_SAME = 5;
  static constexpr int NUM_RESERVE_VISIBLE = 12;
  static constexpr int NUM_RESERVE_DECK = 3;
  static constexpr int NUM_CARDS = 90;
  static constexpr int NUM_NOBLES = 12; // Total noble types in the game
  static constexpr int MAX_GOLD = 5; // 2-player game

  // Offsets
  static constexpr int OFFSET_TAKE_DIFFERENT = 0;
  static constexpr int OFFSET_TAKE_SAME =
      OFFSET_TAKE_DIFFERENT +
      NUM_TAKE_DIFFERENT * TAKE_DIFF_RETURN_PATTERNS; // 840
  static constexpr int OFFSET_RESERVE_VISIBLE =
      OFFSET_TAKE_SAME +
      NUM_TAKE_SAME * TAKE_SAME_RETURN_PATTERNS; // 980
  static constexpr int OFFSET_RESERVE_DECK =
      OFFSET_RESERVE_VISIBLE +
      NUM_RESERVE_VISIBLE * RESERVE_RETURN_PATTERNS; // 1064
  static constexpr int OFFSET_PURCHASE =
      OFFSET_RESERVE_DECK +
      NUM_RESERVE_DECK * RESERVE_RETURN_PATTERNS; // 1085

  static constexpr int TOTAL_PURCHASE = 2035;

  static constexpr int OFFSET_VISIT_NOBLE =
      OFFSET_PURCHASE + TOTAL_PURCHASE; // 3120
  static constexpr int OFFSET_PASS =
      OFFSET_VISIT_NOBLE + NUM_NOBLES; // 3132
  static constexpr int ACTION_SIZE = OFFSET_PASS + 1; // 3133

  // ─── Card payment offset and pattern count tables ───
  // CARD_PAYMENT_OFFSET[card_id] = cumulative offset into PURCHASE range
  // CARD_PATTERN_COUNT[card_id] = number of valid payment patterns for card
  static constexpr uint16_t CARD_PAYMENT_OFFSET[90] = {
      0,    4,   10,   19,   37,   53,   69,   93,   // Cards 0-7
     98,  102,  108,  117,  135,  151,  167,  191,   // Cards 8-15
    196,  200,  206,  215,  233,  249,  265,  289,   // Cards 16-23
    294,  298,  304,  313,  331,  347,  363,  387,   // Cards 24-31
    392,  396,  402,  411,  429,  445,  461,  485,   // Cards 32-39
    490,  522,  560,  566,  584,  610,  616,  648,   // Cards 40-47
    686,  692,  710,  736,  742,  774,  812,  818,   // Cards 48-55
    836,  862,  868,  900,  938,  944,  962,  988,   // Cards 56-63
    994, 1026, 1064, 1070, 1088, 1114, 1120, 1231,   // Cards 64-71
   1237, 1285, 1303, 1414, 1420, 1468, 1486, 1597,   // Cards 72-79
   1603, 1651, 1669, 1780, 1786, 1834, 1852, 1963,   // Cards 80-87
   1969, 2017                                         // Cards 88-89
  };

  static constexpr uint8_t CARD_PATTERN_COUNT[90] = {
      4,  6,  9, 18, 16, 16, 24,  5,   // Cards 0-7
      4,  6,  9, 18, 16, 16, 24,  5,   // Cards 8-15
      4,  6,  9, 18, 16, 16, 24,  5,   // Cards 16-23
      4,  6,  9, 18, 16, 16, 24,  5,   // Cards 24-31
      4,  6,  9, 18, 16, 16, 24,  5,   // Cards 32-39
     32, 38,  6, 18, 26,  6, 32, 38,   // Cards 40-47
      6, 18, 26,  6, 32, 38,  6, 18,   // Cards 48-55
     26,  6, 32, 38,  6, 18, 26,  6,   // Cards 56-63
     32, 38,  6, 18, 26,  6,111,  6,   // Cards 64-71
     48, 18,111,  6, 48, 18,111,  6,   // Cards 72-79
     48, 18,111,  6, 48, 18,111,  6,   // Cards 80-87
     48, 18                             // Cards 88-89
  };

  // ─── Take Different Combinations (C(5,3) = 10) ───
  static constexpr std::array<std::array<uint8_t, 3>, 10> TAKE_DIFF_COMBOS = {{
      {0, 1, 2},
      {0, 1, 3},
      {0, 1, 4},
      {0, 2, 3},
      {0, 2, 4},
      {0, 3, 4},
      {1, 2, 3},
      {1, 2, 4},
      {1, 3, 4},
      {2, 3, 4},
  }};

  // ─── Return encoding (6 colors, max sum 3) — same as V2 ───

  static int encode_return(const std::array<uint8_t, 6> &ret) {
    int s = 0;
    for (int i = 0; i < 6; ++i)
      s += ret[i];
    if (s == 0)
      return 0;
    if (s < 0 || s > 3)
      return -1;

    int rank = 0;
    int remaining = s;
    for (int i = 0; i < 5; ++i) {
      for (int v = remaining; v > static_cast<int>(ret[i]); --v) {
        rank += H[5 - i][remaining - v];
      }
      remaining -= ret[i];
    }
    return RETURN_OFFSET[s] + rank;
  }

  static std::array<uint8_t, 6> decode_return(int pattern) {
    std::array<uint8_t, 6> ret = {0, 0, 0, 0, 0, 0};
    if (pattern == 0)
      return ret;

    int s;
    if (pattern < RETURN_OFFSET[1])
      s = 0;
    else if (pattern < RETURN_OFFSET[2])
      s = 1;
    else if (pattern < RETURN_OFFSET[3])
      s = 2;
    else
      s = 3;

    int local_rank = pattern - RETURN_OFFSET[s];
    int remaining_s = s;

    for (int i = 0; i < 5; ++i) {
      for (int v = remaining_s; v >= 0; --v) {
        int count = H[5 - i][remaining_s - v];
        if (local_rank < count) {
          ret[i] = static_cast<uint8_t>(v);
          remaining_s -= v;
          break;
        }
        local_rank -= count;
      }
    }
    ret[5] = static_cast<uint8_t>(remaining_s);
    return ret;
  }

  // ─── Card-specific payment encoding (constrained, graded lex) ───

  // Count valid compositions of exactly sum s into parts[pos..4]
  // where part[i] <= upper[i]
  static int count_compositions(int s, int pos,
                                const std::array<uint8_t, 5> &upper) {
    if (pos == 5)
      return (s == 0) ? 1 : 0;
    int count = 0;
    int max_v = std::min(s, static_cast<int>(upper[pos]));
    for (int v = 0; v <= max_v; ++v) {
      count += count_compositions(s - v, pos + 1, upper);
    }
    return count;
  }

  // Encode gold_as pattern for a specific card (graded lexicographic order)
  static int encode_payment_for_card(const std::array<uint8_t, 5> &gold_as,
                                     int card_id) {
    if (!is_valid_card_id(card_id))
      return -1;
    const auto &cost = CARDS[card_id].cost;
    int s = 0;
    for (int i = 0; i < 5; ++i)
      s += gold_as[i];
    if (s < 0 || s > MAX_GOLD)
      return -1;

    // Offset: count patterns with sum < s
    int offset = 0;
    for (int k = 0; k < s; ++k) {
      offset += count_compositions(k, 0, cost);
    }

    // Rank within sum s (lexicographic: lower values first)
    int rank = 0;
    int remaining = s;
    for (int i = 0; i < 4; ++i) {
      for (int v = 0; v < static_cast<int>(gold_as[i]); ++v) {
        rank += count_compositions(remaining - v, i + 1, cost);
      }
      remaining -= gold_as[i];
    }

    return offset + rank;
  }

  // Decode payment pattern index for a specific card
  static std::array<uint8_t, 5> decode_payment_for_card(int pattern,
                                                        int card_id) {
    std::array<uint8_t, 5> ga = {0, 0, 0, 0, 0};
    if (!is_valid_card_id(card_id) || pattern < 0 ||
        pattern >= CARD_PATTERN_COUNT[card_id])
      return ga;

    const auto &cost = CARDS[card_id].cost;
    if (pattern == 0)
      return ga;

    // Determine sum s
    int s = 0;
    int cumulative = 0;
    for (s = 0; s <= MAX_GOLD; ++s) {
      int cnt = count_compositions(s, 0, cost);
      if (cumulative + cnt > pattern)
        break;
      cumulative += cnt;
    }

    int local_rank = pattern - cumulative;
    int remaining = s;

    // Unrank in lexicographic order
    for (int i = 0; i < 4; ++i) {
      int max_v = std::min(remaining, static_cast<int>(cost[i]));
      for (int v = 0; v <= max_v; ++v) {
        int cnt = count_compositions(remaining - v, i + 1, cost);
        if (local_rank < cnt) {
          ga[i] = static_cast<uint8_t>(v);
          remaining -= v;
          break;
        }
        local_rank -= cnt;
      }
    }
    ga[4] = static_cast<uint8_t>(remaining);

    return ga;
  }

  // ─── Helper functions ───

  static int find_take_diff_index(const std::array<uint8_t, 5> &take) {
    uint8_t colors[3];
    int idx = 0;
    for (int i = 0; i < 5 && idx < 3; ++i) {
      if (take[i] > 0)
        colors[idx++] = i;
    }
    if (idx != 3)
      return -1;

    for (int i = 0; i < 10; ++i) {
      if (TAKE_DIFF_COMBOS[i][0] == colors[0] &&
          TAKE_DIFF_COMBOS[i][1] == colors[1] &&
          TAKE_DIFF_COMBOS[i][2] == colors[2]) {
        return i;
      }
    }
    return -1;
  }

  static int find_take_same_color(const std::array<uint8_t, 5> &take) {
    for (int i = 0; i < 5; ++i) {
      if (take[i] == 2)
        return i;
    }
    return -1;
  }

  static int find_visible_slot(int8_t card_id, const Board &board) {
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < 4; ++slot) {
        if (board.visible[level][slot] == card_id) {
          return level * 4 + slot;
        }
      }
    }
    return -1;
  }

  static int find_reserved_slot(int8_t card_id, const PlayerState &player) {
    for (int i = 0; i < 3; ++i) {
      if (player.reserved[i] == card_id) {
        return i;
      }
    }
    return -1;
  }

  static bool is_in_reserved(int8_t card_id, const PlayerState &player) {
    for (int i = 0; i < 3; ++i) {
      if (player.reserved[i] == card_id)
        return true;
    }
    return false;
  }

  // Binary search for card_id from local purchase index
  static int find_card_id(int local_idx) {
    int lo = 0, hi = NUM_CARDS - 1;
    while (lo < hi) {
      int mid = (lo + hi + 1) / 2;
      if (CARD_PAYMENT_OFFSET[mid] <= local_idx) {
        lo = mid;
      } else {
        hi = mid - 1;
      }
    }
    return lo;
  }

  // ─── Main encode function ───
  static int encode(const Action &action, const Game &game) {
    const Board &board = game.board;

    switch (action.type) {
    case TAKE_DIFFERENT: {
      int combo_idx = find_take_diff_index(action.take);
      if (combo_idx < 0)
        return -1;
      int ret_pattern = encode_return(action.return_gems);
      if (ret_pattern < 0 || ret_pattern >= TAKE_DIFF_RETURN_PATTERNS)
        return -1;
      return OFFSET_TAKE_DIFFERENT + combo_idx * TAKE_DIFF_RETURN_PATTERNS +
             ret_pattern;
    }

    case TAKE_SAME: {
      int color = find_take_same_color(action.take);
      if (color < 0)
        return -1;
      int ret_pattern = encode_return(action.return_gems);
      if (ret_pattern < 0 || ret_pattern >= TAKE_SAME_RETURN_PATTERNS)
        return -1;
      return OFFSET_TAKE_SAME + color * TAKE_SAME_RETURN_PATTERNS + ret_pattern;
    }

    case RESERVE_VISIBLE: {
      int slot = find_visible_slot(action.card_id, board);
      if (slot < 0)
        return -1;
      int ret_pattern = encode_return(action.return_gems);
      if (ret_pattern < 0 || ret_pattern >= RESERVE_RETURN_PATTERNS)
        return -1;
      return OFFSET_RESERVE_VISIBLE + slot * RESERVE_RETURN_PATTERNS +
             ret_pattern;
    }

    case RESERVE_DECK: {
      if (action.deck_level < 0 || action.deck_level > 2)
        return -1;
      int ret_pattern = encode_return(action.return_gems);
      if (ret_pattern < 0 || ret_pattern >= RESERVE_RETURN_PATTERNS)
        return -1;
      return OFFSET_RESERVE_DECK + action.deck_level * RESERVE_RETURN_PATTERNS +
             ret_pattern;
    }

    case PURCHASE: {
      if (action.card_id < 0 || action.card_id >= NUM_CARDS)
        return -1;
      int pay_pattern =
          encode_payment_for_card(action.gold_as, action.card_id);
      if (pay_pattern < 0 || pay_pattern >= CARD_PATTERN_COUNT[action.card_id])
        return -1;
      return OFFSET_PURCHASE + CARD_PAYMENT_OFFSET[action.card_id] +
             pay_pattern;
    }

    case VISIT_NOBLE: {
      if (action.noble_choice < 0 || action.noble_choice >= NUM_NOBLES)
        return -1;
      return OFFSET_VISIT_NOBLE + action.noble_choice;
    }

    default:
      return OFFSET_PASS;
    }
  }

  // ─── Decode function ───
  static Action decode(int action_id, const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    Action action;

    if (action_id < 0 || action_id >= ACTION_SIZE) {
      action.type = ACTION_TYPE_COUNT;
      return action;
    }

    if (action_id < OFFSET_TAKE_SAME) {
      // TAKE_DIFFERENT
      int local = action_id - OFFSET_TAKE_DIFFERENT;
      int combo = local / TAKE_DIFF_RETURN_PATTERNS;
      int ret_pat = local % TAKE_DIFF_RETURN_PATTERNS;

      action.type = TAKE_DIFFERENT;
      action.take = {0, 0, 0, 0, 0};
      for (int i = 0; i < 3; ++i) {
        action.take[TAKE_DIFF_COMBOS[combo][i]] = 1;
      }
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_RESERVE_VISIBLE) {
      // TAKE_SAME
      int local = action_id - OFFSET_TAKE_SAME;
      int color = local / TAKE_SAME_RETURN_PATTERNS;
      int ret_pat = local % TAKE_SAME_RETURN_PATTERNS;

      action.type = TAKE_SAME;
      action.take = {0, 0, 0, 0, 0};
      action.take[color] = 2;
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_RESERVE_DECK) {
      // RESERVE_VISIBLE
      int local = action_id - OFFSET_RESERVE_VISIBLE;
      int slot = local / RESERVE_RETURN_PATTERNS;
      int ret_pat = local % RESERVE_RETURN_PATTERNS;

      int level = slot / 4;
      int s = slot % 4;

      action.type = RESERVE_VISIBLE;
      action.card_id = board.visible[level][s];
      action.deck_level = level;
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_PURCHASE) {
      // RESERVE_DECK
      int local = action_id - OFFSET_RESERVE_DECK;
      int level = local / RESERVE_RETURN_PATTERNS;
      int ret_pat = local % RESERVE_RETURN_PATTERNS;

      action.type = RESERVE_DECK;
      action.deck_level = level;
      action.card_id = -1;
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_VISIT_NOBLE) {
      // PURCHASE (card ID-based)
      int local_idx = action_id - OFFSET_PURCHASE;
      int card_id = find_card_id(local_idx);
      int pat = local_idx - CARD_PAYMENT_OFFSET[card_id];

      action.type = PURCHASE;
      action.card_id = card_id;
      action.gold_as = decode_payment_for_card(pat, card_id);
      action.from_reserved = is_in_reserved(card_id, player);

    } else if (action_id < OFFSET_PASS) {
      // VISIT_NOBLE (noble ID-based)
      int noble_id = action_id - OFFSET_VISIT_NOBLE;
      action.type = VISIT_NOBLE;
      action.noble_choice = noble_id;

    } else {
      action.type = ACTION_TYPE_COUNT;
    }

    return action;
  }

  static std::array<uint8_t, ACTION_SIZE> get_action_mask(const Game &game) {
    std::array<uint8_t, ACTION_SIZE> mask = {};
    MoveList legal_actions =
        MoveGenerator::generate_all_fixed(game.board, game.simple_payment_mode);

    for (const auto &action : legal_actions) {
      int id = encode(action, game);
      if (id >= 0 && id < ACTION_SIZE) {
        mask[id] = 1;
      }
    }

    bool has_action = false;
    for (int i = 0; i < OFFSET_PASS; ++i) {
      if (mask[i]) {
        has_action = true;
        break;
      }
    }
    if (!has_action) {
      mask[OFFSET_PASS] = 1;
    }

    return mask;
  }

  static Action decode_and_match(int action_id, const Game &game) {
    MoveList legal_actions =
        MoveGenerator::generate_all_fixed(game.board, game.simple_payment_mode);

    for (const auto &legal : legal_actions) {
      if (encode(legal, game) == action_id) {
        return legal;
      }
    }

    return decode(action_id, game);
  }

  // Compute the number of valid payment patterns for a card (for verification)
  static int compute_pattern_count(int card_id) {
    if (!is_valid_card_id(card_id))
      return -1;
    const auto &cost = CARDS[card_id].cost;
    int total = 0;
    for (int s = 0; s <= MAX_GOLD; ++s) {
      total += count_compositions(s, 0, cost);
    }
    return total;
  }
};

#endif // CSPLENDOR_ACTION_ENCODER_V3_H
