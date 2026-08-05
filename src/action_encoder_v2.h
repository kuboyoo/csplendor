#ifndef CSPLENDOR_ACTION_ENCODER_V2_H
#define CSPLENDOR_ACTION_ENCODER_V2_H

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
  using Schema = csplendor::encoding::ActionSpaceV2;
  // ─── Multiset coefficient table H(n,k) = C(n+k-1, k) ───
  // H[n][k] for n=0..6, k=0..5
  static constexpr const int (&H)[7][6] = action_encoder_detail::ReturnCodec::H;

  // Cumulative offset for return sum s (6 colors): patterns with sum < s
  static constexpr const int (&RETURN_OFFSET)[4] =
      action_encoder_detail::ReturnCodec::OFFSET;

  // Cumulative offset for payment sum s (5 colors): patterns with sum < s
  static constexpr int PAYMENT_OFFSET[6] = {0, 1, 6, 21, 56, 126};

  // ─── Pattern counts per action type ───
  static constexpr int TAKE_DIFF_RETURN_PATTERNS =
      Schema::TAKE_DIFF_RETURN_PATTERNS;
  static constexpr int TAKE_SAME_RETURN_PATTERNS =
      Schema::TAKE_SAME_RETURN_PATTERNS;
  static constexpr int RESERVE_RETURN_PATTERNS =
      Schema::RESERVE_RETURN_PATTERNS;
  static constexpr int PURCHASE_PAYMENT_PATTERNS =
      Schema::PURCHASE_PAYMENT_PATTERNS;

  // Base action counts
  static constexpr int NUM_TAKE_DIFFERENT = Schema::NUM_TAKE_DIFFERENT;
  static constexpr int NUM_TAKE_SAME = Schema::NUM_TAKE_SAME;
  static constexpr int NUM_RESERVE_VISIBLE = Schema::NUM_RESERVE_VISIBLE;
  static constexpr int NUM_RESERVE_DECK = Schema::NUM_RESERVE_DECK;
  static constexpr int NUM_PURCHASE_VISIBLE = Schema::NUM_PURCHASE_VISIBLE;
  static constexpr int NUM_PURCHASE_RESERVED = Schema::NUM_PURCHASE_RESERVED;
  static constexpr int NUM_VISIT_NOBLE = Schema::NUM_VISIT_NOBLE;

  // Offsets
  static constexpr int OFFSET_TAKE_DIFFERENT = Schema::OFFSET_TAKE_DIFFERENT;
  static constexpr int OFFSET_TAKE_SAME = Schema::OFFSET_TAKE_SAME;
  static constexpr int OFFSET_RESERVE_VISIBLE = Schema::OFFSET_RESERVE_VISIBLE;
  static constexpr int OFFSET_RESERVE_DECK = Schema::OFFSET_RESERVE_DECK;
  static constexpr int OFFSET_PURCHASE_VISIBLE =
      Schema::OFFSET_PURCHASE_VISIBLE;
  static constexpr int OFFSET_PURCHASE_RESERVED =
      Schema::OFFSET_PURCHASE_RESERVED;
  static constexpr int OFFSET_VISIT_NOBLE = Schema::OFFSET_VISIT_NOBLE;
  static constexpr int OFFSET_PASS = Schema::OFFSET_PASS;
  static constexpr int ACTION_SIZE = Schema::SIZE;

  // ─── Take Different Combinations (C(5,3) = 10) ───
  static constexpr const std::array<std::array<uint8_t, 3>, 10>
      &TAKE_DIFF_COMBOS = action_encoder_detail::TakeDifferentCodec::COMBINATIONS;

  // ─── Return encoding (6 colors, max sum 3) ───

  static int encode_return(const std::array<uint8_t, 6> &ret) {
    return action_encoder_detail::ReturnCodec::encode(ret);
  }

  static std::array<uint8_t, 6> decode_return(int pattern) {
    return action_encoder_detail::ReturnCodec::decode(pattern);
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
          return OFFSET_VISIT_NOBLE + static_cast<int>(i);
        }
      }
      return -1;
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

    } else if (action_id < OFFSET_PURCHASE_VISIBLE) {
      // RESERVE_DECK
      int local = action_id - OFFSET_RESERVE_DECK;
      int level = local / RESERVE_RETURN_PATTERNS;
      int ret_pat = local % RESERVE_RETURN_PATTERNS;

      action.type = RESERVE_DECK;
      action.deck_level = static_cast<int8_t>(level);
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
    return action_encoder_detail::decode_and_match_first<ActionEncoderV2>(
        action_id, game, [&game](auto &sink) {
          MoveGenerator::consume_all_capped(game.board,
                                            game.simple_payment_mode, sink);
        });
  }
};

#endif // CSPLENDOR_ACTION_ENCODER_V2_H
