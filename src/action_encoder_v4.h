#ifndef CSPLENDOR_ACTION_ENCODER_V4_H
#define CSPLENDOR_ACTION_ENCODER_V4_H

#include "action.h"
#include "action_encoder_v3.h"
#include "encoding_schema.h"
#include "game.h"
#include "types.h"
#include <array>
#include <cstdint>
#include <vector>

/**
 * ActionEncoderV4 - V3 with the deck-reservation return split off (3121 ids).
 *
 * Under the current rules a deck reservation never carries a token return:
 * when it takes the player above ten tokens, the player sees the drawn card
 * and then returns one token with a separate RETURN_GEM action.
 *
 *   TAKE_DIFFERENT   840  [0..839]      same as V3
 *   TAKE_SAME        140  [840..979]    same as V3
 *   RESERVE_VISIBLE   84  [980..1063]   same as V3
 *   RESERVE_DECK       3  [1064..1066]  one id per level (no return)
 *   PURCHASE        2035  [1067..3101]  V3 id - 18
 *   VISIT_NOBLE       12  [3102..3113]  V3 id - 18
 *   RETURN_GEM         6  [3114..3119]  white, blue, green, red, black, gold
 *   PASS               1  [3120]
 *
 * v3_to_v4_id() maps every V3 id deterministically (a V3 deck reservation
 * with any return pattern maps to the V4 reservation of the same level), so
 * V3 policy targets migrate by summing probabilities over each image.
 */
class ActionEncoderV4 {
public:
  using Schema = csplendor::encoding::ActionSpaceV4;
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

  // Ids from OFFSET_PURCHASE on are V3 ids shifted down by this amount.
  static constexpr int V3_SHIFT =
      V3::OFFSET_PURCHASE - Schema::OFFSET_PURCHASE; // 18

  static_assert(V3::OFFSET_RESERVE_DECK == OFFSET_RESERVE_DECK,
                "V3/V4 share the ids below the deck reservations");
  static_assert(V3::OFFSET_VISIT_NOBLE - V3_SHIFT == OFFSET_VISIT_NOBLE,
                "purchase and noble ids shift together");

  // Deterministic V3 -> V4 id map; -1 for ids outside the V3 space.
  static constexpr int v3_to_v4_id(int v3_id) noexcept {
    if (v3_id < 0 || v3_id >= V3::ACTION_SIZE)
      return -1;
    if (v3_id < V3::OFFSET_RESERVE_DECK)
      return v3_id;
    if (v3_id < V3::OFFSET_PURCHASE)
      return OFFSET_RESERVE_DECK +
             (v3_id - V3::OFFSET_RESERVE_DECK) / V3::RESERVE_RETURN_PATTERNS;
    if (v3_id < V3::OFFSET_PASS)
      return v3_id - V3_SHIFT;
    return OFFSET_PASS;
  }

  // Inverse for ids that exist in V3 (a deck reservation maps to its
  // no-return V3 id); -1 for RETURN_GEM and out-of-range ids.
  static constexpr int v4_to_v3_id(int v4_id) noexcept {
    if (v4_id < 0 || v4_id >= ACTION_SIZE)
      return -1;
    if (v4_id < OFFSET_RESERVE_DECK)
      return v4_id;
    if (v4_id < OFFSET_PURCHASE)
      return V3::OFFSET_RESERVE_DECK +
             (v4_id - OFFSET_RESERVE_DECK) * V3::RESERVE_RETURN_PATTERNS;
    if (v4_id < OFFSET_RETURN_GEM)
      return v4_id + V3_SHIFT;
    if (v4_id < OFFSET_PASS)
      return -1;
    return V3::OFFSET_PASS;
  }

  static int encode(const Action &action, const Game &game) {
    switch (action.type) {
    case RETURN_GEM: {
      const int color = action.returned_color();
      int total = 0;
      for (uint8_t amount : action.return_gems)
        total += amount;
      return color < 6 && total == 1 ? OFFSET_RETURN_GEM + color : -1;
    }
    case RESERVE_DECK:
      for (uint8_t amount : action.return_gems)
        if (amount != 0)
          return -1;
      if (action.deck_level < 0 || action.deck_level > 2)
        return -1;
      return OFFSET_RESERVE_DECK + action.deck_level;
    default: {
      const int v3_id = V3::encode(action, game);
      return v3_id < 0 ? -1 : v3_to_v4_id(v3_id);
    }
    }
  }

  static Action decode(int action_id, const Game &game) {
    if (action_id >= OFFSET_RETURN_GEM && action_id < OFFSET_PASS) {
      Action action;
      action.type = RETURN_GEM;
      action.return_gems[action_id - OFFSET_RETURN_GEM] = 1;
      return action;
    }
    if (action_id >= OFFSET_RESERVE_DECK && action_id < OFFSET_PURCHASE) {
      Action action;
      action.type = RESERVE_DECK;
      action.deck_level = static_cast<int8_t>(action_id - OFFSET_RESERVE_DECK);
      return action;
    }
    const int v3_id = v4_to_v3_id(action_id);
    if (v3_id < 0) {
      Action invalid;
      invalid.type = ACTION_TYPE_COUNT;
      return invalid;
    }
    return V3::decode(v3_id, game);
  }

  // Legal actions in MoveGenerator order with their V4 ids (forced PASS when
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

#endif // CSPLENDOR_ACTION_ENCODER_V4_H
