#ifndef CSPLENDOR_ACTION_ENCODER_V3_H
#define CSPLENDOR_ACTION_ENCODER_V3_H

#include "action.h"
#include "action_encoder_common.h"
#include "card_data.h"
#include "encoding_schema.h"
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
  using Schema = csplendor::encoding::ActionSpaceV3;
  // ─── Multiset coefficient table H(n,k) = C(n+k-1, k) ───
  // Used for return encoding (same as V2)
  static constexpr const int (&H)[7][6] = action_encoder_detail::ReturnCodec::H;

  // Cumulative offset for return sum s (6 colors)
  static constexpr const int (&RETURN_OFFSET)[4] =
      action_encoder_detail::ReturnCodec::OFFSET;

  // ─── Pattern counts per action type ───
  static constexpr int TAKE_DIFF_RETURN_PATTERNS =
      Schema::TAKE_DIFF_RETURN_PATTERNS;
  static constexpr int TAKE_SAME_RETURN_PATTERNS =
      Schema::TAKE_SAME_RETURN_PATTERNS;
  static constexpr int RESERVE_RETURN_PATTERNS =
      Schema::RESERVE_RETURN_PATTERNS;

  // Base action counts
  static constexpr int NUM_TAKE_DIFFERENT = Schema::NUM_TAKE_DIFFERENT;
  static constexpr int NUM_TAKE_SAME = Schema::NUM_TAKE_SAME;
  static constexpr int NUM_RESERVE_VISIBLE = Schema::NUM_RESERVE_VISIBLE;
  static constexpr int NUM_RESERVE_DECK = Schema::NUM_RESERVE_DECK;
  static constexpr int NUM_CARDS = Schema::NUM_CARDS;
  static constexpr int NUM_NOBLES = Schema::NUM_NOBLES;
  static constexpr int MAX_GOLD = Schema::MAX_GOLD;

  // Offsets
  static constexpr int OFFSET_TAKE_DIFFERENT = Schema::OFFSET_TAKE_DIFFERENT;
  static constexpr int OFFSET_TAKE_SAME = Schema::OFFSET_TAKE_SAME;
  static constexpr int OFFSET_RESERVE_VISIBLE = Schema::OFFSET_RESERVE_VISIBLE;
  static constexpr int OFFSET_RESERVE_DECK = Schema::OFFSET_RESERVE_DECK;
  static constexpr int OFFSET_PURCHASE = Schema::OFFSET_PURCHASE;
  static constexpr int TOTAL_PURCHASE = Schema::TOTAL_PURCHASE;
  static constexpr int OFFSET_VISIT_NOBLE = Schema::OFFSET_VISIT_NOBLE;
  static constexpr int OFFSET_PASS = Schema::OFFSET_PASS;
  static constexpr int ACTION_SIZE = Schema::SIZE;

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
  static constexpr const std::array<std::array<uint8_t, 3>, 10>
      &TAKE_DIFF_COMBOS = action_encoder_detail::TakeDifferentCodec::COMBINATIONS;

  // ─── Return encoding (6 colors, max sum 3) — same as V2 ───

  static int encode_return(const std::array<uint8_t, 6> &ret) {
    return action_encoder_detail::ReturnCodec::encode(ret);
  }

  static std::array<uint8_t, 6> decode_return(int pattern) {
    return action_encoder_detail::ReturnCodec::decode(pattern);
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
    for (int i = 0; i < 5; ++i) {
      if (gold_as[i] > cost[i])
        return -1;
      s += gold_as[i];
    }
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
    return action_encoder_detail::TakeDifferentCodec::find_index(take);
  }

  static int find_take_same_color(const std::array<uint8_t, 5> &take) {
    for (int i = 0; i < 5; ++i) {
      if (take[i] == 2)
        return i;
    }
    return -1;
  }

  static int find_visible_slot(int8_t card_id, const Board &board) {
    return action_encoder_detail::find_visible_slot(card_id, board);
  }

  static int find_reserved_slot(int8_t card_id, const PlayerState &player) {
    return action_encoder_detail::find_reserved_slot(card_id, player);
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
    case PASS:
      return OFFSET_PASS;
    default:
      return -1;
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
      action.deck_level = static_cast<int8_t>(level);
      action.return_gems = decode_return(ret_pat);

    } else if (action_id < OFFSET_PURCHASE) {
      // RESERVE_DECK
      int local = action_id - OFFSET_RESERVE_DECK;
      int level = local / RESERVE_RETURN_PATTERNS;
      int ret_pat = local % RESERVE_RETURN_PATTERNS;

      action.type = RESERVE_DECK;
      action.deck_level = static_cast<int8_t>(level);
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
      action.type = PASS;
    }

    return action;
  }

  static std::array<uint8_t, ACTION_SIZE> get_action_mask(const Game &game) {
    std::array<uint8_t, ACTION_SIZE> mask = {};
    auto sink = [&game, &mask](const Action &action) {
      int id = encode(action, game);
      if (id >= 0 && id < ACTION_SIZE)
        mask[id] = 1;
      return true;
    };
    MoveGenerator::consume_all_capped(game.board, game.simple_payment_mode,
                                      sink);

    bool has_action = false;
    for (int i = 0; i < OFFSET_PASS; ++i) {
      if (mask[i]) {
        has_action = true;
        break;
      }
    }
    if (!has_action && game.requires_forced_pass()) {
      mask[OFFSET_PASS] = 1;
    }

    return mask;
  }

  static Action decode_and_match(int action_id, const Game &game) {
    return action_encoder_detail::decode_and_match_first<ActionEncoderV3>(
        action_id, game, [&game](auto &sink) {
          MoveGenerator::consume_all_capped(game.board,
                                            game.simple_payment_mode, sink);
        });
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
