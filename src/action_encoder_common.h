#ifndef CSPLENDOR_ACTION_ENCODER_COMMON_H
#define CSPLENDOR_ACTION_ENCODER_COMMON_H

#include "action.h"
#include "game.h"
#include "rule_query.h"
#include <array>
#include <cstdint>

namespace action_encoder_detail {

struct ReturnCodec {
  static constexpr int H[7][6] = {
      {1, 0, 0, 0, 0, 0},     {1, 1, 1, 1, 1, 1},
      {1, 2, 3, 4, 5, 6},     {1, 3, 6, 10, 15, 21},
      {1, 4, 10, 20, 35, 56}, {1, 5, 15, 35, 70, 126},
      {1, 6, 21, 56, 126, 252},
  };
  static constexpr int OFFSET[4] = {0, 1, 7, 28};

  static constexpr int encode(const std::array<uint8_t, 6> &ret) {
    int sum = 0;
    for (int i = 0; i < 6; ++i)
      sum += ret[i];
    if (sum == 0)
      return 0;
    if (sum > 3)
      return -1;

    int rank = 0;
    int remaining = sum;
    for (int i = 0; i < 5; ++i) {
      for (int value = remaining; value > static_cast<int>(ret[i]); --value)
        rank += H[5 - i][remaining - value];
      remaining -= ret[i];
    }
    return OFFSET[sum] + rank;
  }

  static constexpr std::array<uint8_t, 6> decode(int pattern) {
    std::array<uint8_t, 6> ret = {0, 0, 0, 0, 0, 0};
    if (pattern < 0 || pattern >= 84)
      return ret;

    int sum = 0;
    while (sum < 3 && pattern >= OFFSET[sum + 1])
      ++sum;

    int rank = pattern - OFFSET[sum];
    int remaining = sum;
    for (int i = 0; i < 5; ++i) {
      for (int value = remaining; value >= 0; --value) {
        const int count = H[5 - i][remaining - value];
        if (rank < count) {
          ret[i] = static_cast<uint8_t>(value);
          remaining -= value;
          break;
        }
        rank -= count;
      }
    }
    ret[5] = static_cast<uint8_t>(remaining);
    return ret;
  }
};

struct TakeDifferentCodec {
  static constexpr std::array<std::array<uint8_t, 3>, 10> COMBINATIONS = {{
      {0, 1, 2}, {0, 1, 3}, {0, 1, 4}, {0, 2, 3}, {0, 2, 4},
      {0, 3, 4}, {1, 2, 3}, {1, 2, 4}, {1, 3, 4}, {2, 3, 4},
  }};

  static constexpr int find_index(const std::array<uint8_t, 5> &take) {
    std::array<uint8_t, 3> colors = {0, 0, 0};
    int count = 0;
    for (int color = 0; color < 5; ++color) {
      if (take[color] == 0)
        continue;
      if (take[color] != 1 || count == 3)
        return -1;
      colors[count++] = static_cast<uint8_t>(color);
    }
    if (count == 0)
      return -1;

    if (count == 3) {
      for (int index = 0; index < 10; ++index) {
        if (COMBINATIONS[index] == colors)
          return index;
      }
      return -1;
    }

    // When fewer than three colors remain in the bank, Splendor requires
    // taking every remaining color.  Reuse the first ordinary three-color
    // policy slot containing that subset.  The missing supersets are not
    // legal in the same state, so this remains injective over legal actions
    // and matches the legacy 48-action encoder's established mapping.
    for (int index = 0; index < 10; ++index) {
      bool contains_all = true;
      for (int i = 0; i < count && contains_all; ++i) {
        bool found = false;
        for (int j = 0; j < 3; ++j)
          found = found || COMBINATIONS[index][j] == colors[i];
        contains_all = contains_all && found;
      }
      if (contains_all)
        return index;
    }
    return -1;
  }
};

inline int find_visible_slot(int8_t card_id, const Board &board) {
  const auto source = csplendor::rules::find_visible_card_source(board, card_id);
  return source ? source.level * 4 + source.slot : -1;
}

inline int find_reserved_slot(int8_t card_id, const PlayerState &player) {
  return csplendor::rules::find_reserved_card_source(player, card_id);
}

template <typename Encoder, typename ScanLegalActions>
Action decode_and_match_first(int action_id, const Game &game,
                              ScanLegalActions &&scan_legal_actions) {
  Action matched;
  bool found = false;
  auto sink = [action_id, &game, &matched, &found](const Action &legal) {
    if (Encoder::encode(legal, game) != action_id)
      return true;
    matched = legal;
    found = true;
    return false;
  };
  scan_legal_actions(sink);
  return found ? matched : Encoder::decode(action_id, game);
}

constexpr bool return_codec_round_trips() {
  for (int pattern = 0; pattern < 84; ++pattern) {
    if (ReturnCodec::encode(ReturnCodec::decode(pattern)) != pattern)
      return false;
  }
  return true;
}

static_assert(return_codec_round_trips(),
              "return codec rank and unrank must be inverse operations");

} // namespace action_encoder_detail

#endif // CSPLENDOR_ACTION_ENCODER_COMMON_H
