#ifndef CSPLENDOR_ACTION_ENCODER_V5_H
#define CSPLENDOR_ACTION_ENCODER_V5_H

#include "action.h"
#include "action_encoder_v3.h"
#include "encoding_schema.h"
#include "game.h"
#include "types.h"
#include <array>
#include <cstdint>
#include <vector>

/**
 * ActionEncoderV5 - the all-action policy space (3049 ids).
 *
 * No reservation carries a token return: when it takes the player above ten
 * tokens, the player sees the refilled board card or the drawn card and then
 * returns one token with a separate RETURN_GEM action.
 *
 *   TAKE_DIFFERENT   840  [0..839]      same as V3
 *   TAKE_SAME        140  [840..979]    same as V3
 *   RESERVE_VISIBLE   12  [980..991]    980 + level * 4 + slot
 *   RESERVE_DECK       3  [992..994]    992 + level
 *   PURCHASE        2035  [995..3029]   V3 id - 90
 *   VISIT_NOBLE       12  [3030..3041]  V3 id - 90
 *   RETURN_GEM         6  [3042..3047]  white, blue, green, red, black, gold
 *   PASS               1  [3048]
 */
class ActionEncoderV5 {
public:
  using Schema = csplendor::encoding::ActionSpaceV5;
  using V3 = ActionEncoderV3;

  static constexpr int OFFSET_TAKE_DIFFERENT = Schema::OFFSET_TAKE_DIFFERENT;
  static constexpr int OFFSET_TAKE_SAME = Schema::OFFSET_TAKE_SAME;
  static constexpr int OFFSET_RESERVE_VISIBLE = Schema::OFFSET_RESERVE_VISIBLE;
  static constexpr int OFFSET_RESERVE_DECK = Schema::OFFSET_RESERVE_DECK;
  static constexpr int OFFSET_PURCHASE = Schema::OFFSET_PURCHASE;
  static constexpr int TOTAL_PURCHASE = Schema::TOTAL_PURCHASE;
  static constexpr int OFFSET_VISIT_NOBLE = Schema::OFFSET_VISIT_NOBLE;
  static constexpr int OFFSET_RETURN_GEM = Schema::OFFSET_RETURN_GEM;
  static constexpr int OFFSET_PASS = Schema::OFFSET_PASS;
  static constexpr int ACTION_SIZE = Schema::SIZE;

  // Purchase and noble ids are V3 ids shifted down by this amount.
  static constexpr int V3_SHIFT = V3::OFFSET_PURCHASE - OFFSET_PURCHASE; // 90

  static_assert(V3::OFFSET_TAKE_SAME == OFFSET_TAKE_SAME &&
                    V3::OFFSET_RESERVE_VISIBLE == OFFSET_RESERVE_VISIBLE,
                "V5 keeps the V3 take ids");
  static_assert(V3::OFFSET_VISIT_NOBLE - V3_SHIFT == OFFSET_VISIT_NOBLE,
                "V5 keeps the V3 purchase and noble layout");

  static int encode(const Action &action, const Game &game) {
    switch (action.type) {
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      return V3::encode(action, game);
    case RESERVE_VISIBLE: {
      if (!csplendor::rules::has_no_token_return(action))
        return -1;
      for (int level = 0; level < 3; ++level)
        for (int slot = 0; slot < 4; ++slot)
          if (game.board.visible[level][slot] == action.card_id)
            return OFFSET_RESERVE_VISIBLE + level * 4 + slot;
      return -1;
    }
    case RESERVE_DECK:
      if (!csplendor::rules::has_no_token_return(action) ||
          action.deck_level < 0 || action.deck_level > 2)
        return -1;
      return OFFSET_RESERVE_DECK + action.deck_level;
    case PURCHASE:
    case VISIT_NOBLE: {
      const int v3_id = V3::encode(action, game);
      return v3_id < 0 ? -1 : v3_id - V3_SHIFT;
    }
    case RETURN_GEM: {
      const int color = action.returned_color();
      int total = 0;
      for (uint8_t amount : action.return_gems)
        total += amount;
      return color < 6 && total == 1 ? OFFSET_RETURN_GEM + color : -1;
    }
    case PASS:
      return OFFSET_PASS;
    default:
      return -1;
    }
  }

  static Action decode(int action_id, const Game &game) {
    Action action;
    if (action_id < 0 || action_id >= ACTION_SIZE) {
      action.type = ACTION_TYPE_COUNT;
    } else if (action_id < OFFSET_RESERVE_VISIBLE) {
      return V3::decode(action_id, game);
    } else if (action_id < OFFSET_RESERVE_DECK) {
      const int local = action_id - OFFSET_RESERVE_VISIBLE;
      action.type = RESERVE_VISIBLE;
      action.card_id = game.board.visible[local / 4][local % 4];
    } else if (action_id < OFFSET_PURCHASE) {
      action.type = RESERVE_DECK;
      action.deck_level = static_cast<int8_t>(action_id - OFFSET_RESERVE_DECK);
    } else if (action_id < OFFSET_RETURN_GEM) {
      return V3::decode(action_id + V3_SHIFT, game);
    } else if (action_id < OFFSET_PASS) {
      action.type = RETURN_GEM;
      action.return_gems[action_id - OFFSET_RETURN_GEM] = 1;
    } else {
      action.type = PASS;
    }
    return action;
  }

  // Legal actions in MoveGenerator order with their V5 ids (forced PASS when
  // no ordinary action exists, as in get_action_mask()).
  template <typename Sink>
  static void for_each_legal_with_id(const Game &game, Sink &sink) {
    int emitted = 0;
    auto enumerate = [&game, &sink, &emitted](const Action &action) {
      const int id = encode(action, game);
      if (id >= 0 && id < ACTION_SIZE) {
        ++emitted;
        sink(id, action);
      }
      return true;
    };
    MoveGenerator::consume_all_capped(game.board, game.simple_payment_mode,
                                      enumerate);
    if (emitted == 0 && game.requires_forced_pass()) {
      Action pass;
      pass.type = PASS;
      sink(OFFSET_PASS, pass);
    }
  }

  static std::array<uint8_t, ACTION_SIZE> get_action_mask(const Game &game) {
    std::array<uint8_t, ACTION_SIZE> mask = {};
    auto sink = [&mask](int id, const Action &) { mask[id] = 1; };
    for_each_legal_with_id(game, sink);
    return mask;
  }

  // encode(a, game) for every entry of game.legal_actions(), in that order.
  static std::vector<int32_t> legal_action_ids(const Game &game) {
    std::vector<int32_t> ids;
    auto sink = [&ids, &game](const Action &action) {
      ids.push_back(encode(action, game));
      return true;
    };
    MoveGenerator::consume_all_capped(game.board, game.simple_payment_mode,
                                      sink);
    return ids;
  }

  static Action decode_and_match(int action_id, const Game &game) {
    Action matched;
    bool found = false;
    auto sink = [action_id, &game, &matched, &found](const Action &legal) {
      if (encode(legal, game) != action_id)
        return true;
      matched = legal;
      found = true;
      return false;
    };
    MoveGenerator::consume_all_capped(game.board, game.simple_payment_mode,
                                      sink);
    return found ? matched : decode(action_id, game);
  }
};

#endif // CSPLENDOR_ACTION_ENCODER_V5_H
