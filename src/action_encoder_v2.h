#ifndef CSPLENDOR_ACTION_ENCODER_V2_H
#define CSPLENDOR_ACTION_ENCODER_V2_H

#include "action.h"
#include "card_data.h"
#include "game.h"
#include "types.h"
#include <algorithm>
#include <array>
#include <cstdint>

/**
 * ActionEncoderV2 - Full action space encoder (4869 actions)
 *
 * Uses multiset composition ranking for both return and payment encoding.
 * Every distinct legal action maps to a unique action ID (injective).
 * See doc/action_space_v2.md for the complete specification.
 *
 * Action ID layout:
 *   TAKE_DIFFERENT:   10 combos  x  84 return patterns =  840 [0..839]
 *   TAKE_SAME:         5 colors  x  28 return patterns =  140 [840..979]
 *   RESERVE_VISIBLE:  12 slots   x   7 return patterns =   84 [980..1063]
 *   RESERVE_DECK:      3 levels  x   7 return patterns =   21 [1064..1084]
 *   PURCHASE_VISIBLE: 12 slots   x 252 payment patterns = 3024 [1085..4108]
 *   PURCHASE_RESERVED: 3 slots   x 252 payment patterns =  756 [4109..4864]
 *   VISIT_NOBLE:       3                                      [4865..4867]
 *   PASS:              1                                      [4868]
 *   Total: 4869
 */
class ActionEncoderV2 {
public:
  // ─── Multiset coefficient table H(n,k) = C(n+k-1, k) ───
  // H[n][k] for n=0..6, k=0..5
  static constexpr int H[7][6] = {
      {1, 0, 0, 0, 0, 0},     // n=0
      {1, 1, 1, 1, 1, 1},     // n=1
      {1, 2, 3, 4, 5, 6},     // n=2
      {1, 3, 6, 10, 15, 21},  // n=3
      {1, 4, 10, 20, 35, 56}, // n=4
      {1, 5, 15, 35, 70, 126},// n=5
      {1, 6, 21, 56, 126, 252},// n=6
  };

  // Cumulative offset for return sum s (6 colors): patterns with sum < s
  static constexpr int RETURN_OFFSET[4] = {0, 1, 7, 28};

  // Cumulative offset for payment sum s (5 colors): patterns with sum < s
  static constexpr int PAYMENT_OFFSET[6] = {0, 1, 6, 21, 56, 126};

  // ─── Pattern counts per action type ───
  static constexpr int TAKE_DIFF_RETURN_PATTERNS = 84;  // ret 0-3, 6 colors
  static constexpr int TAKE_SAME_RETURN_PATTERNS = 28;  // ret 0-2, 6 colors
  static constexpr int RESERVE_RETURN_PATTERNS = 7;     // ret 0-1, 6 colors
  static constexpr int PURCHASE_PAYMENT_PATTERNS = 252;  // gold 0-5, 5 colors

  // Base action counts
  static constexpr int NUM_TAKE_DIFFERENT = 10;
  static constexpr int NUM_TAKE_SAME = 5;
  static constexpr int NUM_RESERVE_VISIBLE = 12;
  static constexpr int NUM_RESERVE_DECK = 3;
  static constexpr int NUM_PURCHASE_VISIBLE = 12;
  static constexpr int NUM_PURCHASE_RESERVED = 3;
  static constexpr int NUM_VISIT_NOBLE = 3;

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
  static constexpr int OFFSET_PURCHASE_VISIBLE =
      OFFSET_RESERVE_DECK +
      NUM_RESERVE_DECK * RESERVE_RETURN_PATTERNS; // 1085
  static constexpr int OFFSET_PURCHASE_RESERVED =
      OFFSET_PURCHASE_VISIBLE +
      NUM_PURCHASE_VISIBLE * PURCHASE_PAYMENT_PATTERNS; // 4109
  static constexpr int OFFSET_VISIT_NOBLE =
      OFFSET_PURCHASE_RESERVED +
      NUM_PURCHASE_RESERVED * PURCHASE_PAYMENT_PATTERNS; // 4865
  static constexpr int OFFSET_PASS =
      OFFSET_VISIT_NOBLE + NUM_VISIT_NOBLE; // 4868

  static constexpr int ACTION_SIZE = OFFSET_PASS + 1; // 4869

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

  // ─── Return encoding (6 colors, max sum 3) ───

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

  // ─── Payment encoding (5 colors, max sum 5) ───

  static int encode_payment(const std::array<uint8_t, 5> &gold_as) {
    int s = 0;
    for (int i = 0; i < 5; ++i)
      s += gold_as[i];
    if (s == 0)
      return 0;
    if (s < 0 || s > 5)
      return -1;

    int rank = 0;
    int remaining = s;
    for (int i = 0; i < 4; ++i) {
      for (int v = remaining; v > static_cast<int>(gold_as[i]); --v) {
        rank += H[4 - i][remaining - v];
      }
      remaining -= gold_as[i];
    }
    return PAYMENT_OFFSET[s] + rank;
  }

  static std::array<uint8_t, 5> decode_payment(int pattern) {
    std::array<uint8_t, 5> ga = {0, 0, 0, 0, 0};
    if (pattern == 0)
      return ga;

    int s;
    if (pattern < PAYMENT_OFFSET[1])
      s = 0;
    else if (pattern < PAYMENT_OFFSET[2])
      s = 1;
    else if (pattern < PAYMENT_OFFSET[3])
      s = 2;
    else if (pattern < PAYMENT_OFFSET[4])
      s = 3;
    else if (pattern < PAYMENT_OFFSET[5])
      s = 4;
    else
      s = 5;

    int local_rank = pattern - PAYMENT_OFFSET[s];
    int remaining_s = s;

    for (int i = 0; i < 4; ++i) {
      for (int v = remaining_s; v >= 0; --v) {
        int count = H[4 - i][remaining_s - v];
        if (local_rank < count) {
          ga[i] = static_cast<uint8_t>(v);
          remaining_s -= v;
          break;
        }
        local_rank -= count;
      }
    }
    ga[4] = static_cast<uint8_t>(remaining_s);
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

  // ─── Main encode function ───
  static int encode(const Action &action, const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];

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
      int pay_pattern = encode_payment(action.gold_as);
      if (pay_pattern < 0 || pay_pattern >= PURCHASE_PAYMENT_PATTERNS)
        return -1;
      if (action.from_reserved) {
        int slot = find_reserved_slot(action.card_id, player);
        if (slot < 0)
          return -1;
        return OFFSET_PURCHASE_RESERVED + slot * PURCHASE_PAYMENT_PATTERNS +
               pay_pattern;
      } else {
        int slot = find_visible_slot(action.card_id, board);
        if (slot < 0)
          return -1;
        return OFFSET_PURCHASE_VISIBLE + slot * PURCHASE_PAYMENT_PATTERNS +
               pay_pattern;
      }
    }

    case VISIT_NOBLE: {
      for (size_t i = 0; i < board.nobles.size() && i < 3; ++i) {
        if (board.nobles[i] == action.noble_choice) {
          return OFFSET_VISIT_NOBLE + i;
        }
      }
      return -1;
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

    } else if (action_id < OFFSET_PURCHASE_VISIBLE) {
      // RESERVE_DECK
      int local = action_id - OFFSET_RESERVE_DECK;
      int level = local / RESERVE_RETURN_PATTERNS;
      int ret_pat = local % RESERVE_RETURN_PATTERNS;

      action.type = RESERVE_DECK;
      action.deck_level = level;
      action.card_id = -1;
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_PURCHASE_RESERVED) {
      // PURCHASE_VISIBLE
      int local = action_id - OFFSET_PURCHASE_VISIBLE;
      int slot = local / PURCHASE_PAYMENT_PATTERNS;
      int pay_pat = local % PURCHASE_PAYMENT_PATTERNS;

      int level = slot / 4;
      int s = slot % 4;

      action.type = PURCHASE;
      action.card_id = board.visible[level][s];
      action.from_reserved = false;
      action.gold_as = decode_payment(pay_pat);

    } else if (action_id < OFFSET_VISIT_NOBLE) {
      // PURCHASE_RESERVED
      int local = action_id - OFFSET_PURCHASE_RESERVED;
      int slot = local / PURCHASE_PAYMENT_PATTERNS;
      int pay_pat = local % PURCHASE_PAYMENT_PATTERNS;

      action.type = PURCHASE;
      action.card_id = player.reserved[slot];
      action.from_reserved = true;
      action.gold_as = decode_payment(pay_pat);

    } else if (action_id < OFFSET_PASS) {
      // VISIT_NOBLE
      int idx = action_id - OFFSET_VISIT_NOBLE;
      action.type = VISIT_NOBLE;
      if (idx < (int)board.nobles.size()) {
        action.noble_choice = board.nobles[idx];
      }

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
};

#endif // CSPLENDOR_ACTION_ENCODER_V2_H
