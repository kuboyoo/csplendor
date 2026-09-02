#include "game.h"
#include "game_snapshot.h"
#include "rule_query.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

namespace {

constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr uint64_t REACHABLE_ACTION_DIGEST = 0x5048f8689f1dcee7ULL;

void check(bool condition) {
  if (!condition)
    std::abort();
}

template <typename Integer>
void digest_little_endian(uint64_t &digest, Integer value, size_t bytes) {
  const uint64_t widened = static_cast<uint64_t>(value);
  for (size_t index = 0; index < bytes; ++index) {
    digest ^= static_cast<uint8_t>(widened >> (index * 8));
    digest *= FNV_PRIME;
  }
}

csplendor::rules::VisibleCardSource legacy_visible_source(const Board &board,
                                                          int card_id) {
  for (int level = 0; level < 3; ++level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      if (board.visible[level][slot] == card_id)
        return {static_cast<int8_t>(level), static_cast<int8_t>(slot)};
    }
  }
  return {};
}

int legacy_reserved_source(const PlayerState &player, int card_id) {
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
    if (player.reserved[slot] == card_id)
      return slot;
  }
  return -1;
}

std::array<int, 5> legacy_effective_cost(const PlayerState &player,
                                         const Card &card) {
  std::array<int, 5> result{};
  for (int color = 0; color < 5; ++color) {
    result[color] = std::max(0, static_cast<int>(card.cost[color]) -
                                    static_cast<int>(player.bonuses[color]));
  }
  return result;
}

std::array<uint8_t, 6> legacy_gems_after_action(const Board &board,
                                                const Action &action) {
  std::array<uint8_t, 6> result = board.players[board.current_player].gems;
  if (action.type == TAKE_DIFFERENT || action.type == TAKE_SAME) {
    for (int color = 0; color < 5; ++color)
      result[color] += action.take[color];
  } else if (action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK) {
    if (board.bank[GOLD] > 0)
      ++result[GOLD];
  }
  return result;
}

bool legacy_validate_return(const std::array<uint8_t, 6> &available,
                            const std::array<uint8_t, 6> &returned) {
  int available_total = 0;
  int returned_total = 0;
  for (int color = 0; color < 6; ++color) {
    if (returned[color] > available[color])
      return false;
    available_total += available[color];
    returned_total += returned[color];
  }
  return returned_total == std::max(0, available_total - Board::MAX_TOKENS);
}

bool legacy_validate_payment(const PlayerState &player, const Card &card,
                             const std::array<uint8_t, 5> &gold_as) {
  int gold_used = 0;
  for (int color = 0; color < 5; ++color) {
    const int cost = std::max(0, static_cast<int>(card.cost[color]) -
                                     static_cast<int>(player.bonuses[color]));
    const int from_gold = gold_as[color];
    if (from_gold > cost || cost - from_gold > player.gems[color])
      return false;
    gold_used += from_gold;
  }
  return gold_used <= player.gems[GOLD];
}

uint16_t brute_return_count(const std::array<uint8_t, 6> &available,
                            int remaining, int color_idx) {
  if (remaining == 0)
    return 1;
  if (color_idx == 6)
    return 0;

  uint16_t count = 0;
  const int maximum =
      std::min(remaining, static_cast<int>(available[color_idx]));
  for (int amount = 0; amount <= maximum; ++amount) {
    count = static_cast<uint16_t>(count + brute_return_count(available,
                                                             remaining - amount,
                                                             color_idx + 1));
  }
  return count;
}

void test_packed_return_code_helper() {
  constexpr std::array<ActionType, 4> RETURN_TYPES = {
      TAKE_DIFFERENT, TAKE_SAME, RESERVE_VISIBLE, RESERVE_DECK};
  constexpr uint32_t CASES = 4 * 4 * 4 * 4 * 4 * 4;
  for (ActionType type : RETURN_TYPES) {
    Action base;
    base.type = type;
    base.take = {1, 0, 2, 0, 1};
    base.card_id = 37;
    base.deck_level = 2;
    const uint64_t base_code = base.pack();
    for (uint32_t encoded = 0; encoded < CASES; ++encoded) {
      Action expected = base;
      uint32_t remainder = encoded;
      for (uint8_t &amount : expected.return_gems) {
        amount = static_cast<uint8_t>(remainder % 4);
        remainder /= 4;
      }
      check(Action::pack_return_gems(base_code, type, expected.return_gems) ==
            expected.pack());
    }
  }
}

void test_small_return_pattern_table() {
  using Pattern = std::array<uint8_t, 6>;
  constexpr uint32_t CASES = 4 * 4 * 4 * 4 * 4 * 4;
  const auto &table = csplendor::move_generation_detail::SMALL_RETURN_PATTERNS;

  for (uint32_t encoded = 0; encoded < CASES; ++encoded) {
    std::array<uint8_t, 6> available{};
    uint32_t remainder = encoded;
    for (uint8_t &amount : available) {
      amount = static_cast<uint8_t>(remainder % 4);
      remainder /= 4;
    }

    for (int excess = 1; excess <= 3; ++excess) {
      std::array<Pattern, 56> oracle{};
      uint8_t oracle_count = 0;
      for (int a0 = 0; a0 <= std::min<int>(excess, available[0]); ++a0)
        for (int a1 = 0; a1 <= std::min<int>(excess, available[1]); ++a1)
          for (int a2 = 0; a2 <= std::min<int>(excess, available[2]); ++a2)
            for (int a3 = 0; a3 <= std::min<int>(excess, available[3]); ++a3)
              for (int a4 = 0; a4 <= std::min<int>(excess, available[4]); ++a4)
                for (int a5 = 0; a5 <= std::min<int>(excess, available[5]);
                     ++a5) {
                  if (a0 + a1 + a2 + a3 + a4 + a5 == excess) {
                    oracle[oracle_count++] = {
                        static_cast<uint8_t>(a0), static_cast<uint8_t>(a1),
                        static_cast<uint8_t>(a2), static_cast<uint8_t>(a3),
                        static_cast<uint8_t>(a4), static_cast<uint8_t>(a5)};
                  }
                }

      uint8_t actual_count = 0;
      for (uint8_t index = 0; index < table.counts[excess]; ++index) {
        const Pattern &pattern = table.patterns[excess][index];
        bool valid = true;
        for (int color = 0; color < 6; ++color)
          valid = valid && pattern[color] <= available[color];
        if (valid)
          check(pattern == oracle[actual_count++]);
      }
      check(actual_count == oracle_count);
    }
  }
}

void test_small_return_count_closed_form() {
  constexpr std::array<uint16_t, 9> limits = {0,  1,  2,  3,        7,
                                              17, 31, 56, MAX_MOVES};
  constexpr uint32_t RADIX = 5;
  constexpr uint32_t CASES = RADIX * RADIX * RADIX * RADIX * RADIX * RADIX;

  for (uint32_t encoded = 0; encoded < CASES; ++encoded) {
    std::array<uint8_t, 6> available{};
    uint32_t remainder = encoded;
    for (uint8_t &amount : available) {
      amount = static_cast<uint8_t>(remainder % RADIX);
      remainder /= RADIX;
    }
    for (int excess = 0; excess <= 3; ++excess) {
      const uint16_t oracle = brute_return_count(available, excess, 0);
      for (uint16_t limit : limits) {
        const uint16_t expected = std::min(oracle, limit);
        check(csplendor::move_generation_detail::count_small_token_returns(
                  available, excess, limit) == expected);
      }
    }
  }
}

csplendor::rules::EligibleNobles legacy_eligible_nobles(const Board &board,
                                                        int player_index) {
  csplendor::rules::EligibleNobles result;
  if (player_index < 0 || player_index >= Board::NUM_PLAYERS)
    return result;
  const uint16_t mask = board.players[player_index].noble_eligibility_mask;
  for (uint8_t noble_id : board.nobles) {
    if (is_valid_noble_id(noble_id) &&
        (mask & (uint16_t{1} << noble_id)) != 0) {
      result.push_back_unchecked(noble_id);
    }
  }
  return result;
}

void check_noble_equivalence(const Board &board) {
  for (int player = -1; player <= Board::NUM_PLAYERS; ++player) {
    const auto legacy = legacy_eligible_nobles(board, player);
    const auto extracted = csplendor::rules::eligible_nobles(board, player);
    check(legacy.size() == extracted.size());
    for (size_t index = 0; index < legacy.size(); ++index)
      check(legacy[index] == extracted[index]);
  }
}

void check_turn_end_equivalence(const Board &board) {
  const bool legacy_final_round =
      !board.final_round && board.current_player < Board::NUM_PLAYERS &&
      board.players[board.current_player].points >= 15;
  check(legacy_final_round ==
        csplendor::rules::should_start_final_round(board));

  const int points0 = board.players[0].points;
  const int points1 = board.players[1].points;
  const int purchased0 = board.players[0].purchased_count;
  const int purchased1 = board.players[1].purchased_count;
  const int8_t legacy_winner =
      static_cast<int8_t>(points0 != points1 ? (points0 > points1 ? 0 : 1)
                          : purchased0 == purchased1 ? -2
                          : purchased0 < purchased1  ? 0
                                                     : 1);
  check(legacy_winner == csplendor::rules::winner_after_completed_round(board));
}

void check_action_queries(const Game &game, const Action &action) {
  const Board &board = game.board;
  const PlayerState &player = board.players[board.current_player];

  if (action.type == TAKE_DIFFERENT || action.type == TAKE_SAME ||
      action.type == RESERVE_VISIBLE || action.type == RESERVE_DECK) {
    const auto legacy_available = legacy_gems_after_action(board, action);
    const auto extracted_available =
        csplendor::rules::gems_after_token_action(board, action);
    check(legacy_available == extracted_available);
    check(legacy_validate_return(legacy_available, action.return_gems) ==
          csplendor::rules::validate_token_return(extracted_available,
                                                  action.return_gems));
  }

  if (action.type == RESERVE_VISIBLE ||
      (action.type == PURCHASE && !action.from_reserved)) {
    const auto legacy = legacy_visible_source(board, action.card_id);
    const auto extracted =
        csplendor::rules::find_visible_card_source(board, action.card_id);
    check(legacy.level == extracted.level && legacy.slot == extracted.slot);
  }

  if (action.type == PURCHASE) {
    if (action.from_reserved) {
      check(
          legacy_reserved_source(player, action.card_id) ==
          csplendor::rules::find_reserved_card_source(player, action.card_id));
    }
    const Card &card = get_card(action.card_id);
    check(legacy_effective_cost(player, card) ==
          csplendor::rules::effective_card_cost(player, card));
    check(legacy_validate_payment(player, card, action.gold_as) ==
          csplendor::rules::validate_purchase_payment(player, card,
                                                      action.gold_as));
  }
}

void check_transition_equivalence(const Game &game, uint64_t code) {
  Game validated = game.clone_light();
  Game trusted = game.clone_light();
  check(validated.apply_action_code(code, false));
  check(trusted.apply_action_code_trusted(code, false));
  check(csplendor::snapshot::serialize(validated) ==
        csplendor::snapshot::serialize(trusted));
  check(validated.board.hash() == trusted.board.hash());
}

void test_reachable_differential_and_golden() {
  uint64_t digest = FNV_OFFSET;
  uint64_t state_count = 0;
  uint64_t action_count = 0;

  for (uint32_t seed = 0; seed < 32; ++seed) {
    Game game(seed);
    for (uint32_t ply = 0; ply < 96; ++ply) {
      const auto actions = game.legal_actions();
      const auto codes = game.legal_action_codes();
      check(actions.size() == codes.size());
      check(codes.size() == game.legal_action_count());

      digest_little_endian(digest, seed, 4);
      digest_little_endian(digest, ply, 4);
      digest_little_endian(digest, static_cast<uint16_t>(codes.size()), 2);
      for (size_t index = 0; index < actions.size(); ++index) {
        check(actions[index].pack() == codes[index]);
        check(game.is_legal(actions[index]));
        check_action_queries(game, actions[index]);
        digest_little_endian(digest, codes[index], 8);
      }

      check_noble_equivalence(game.board);
      check_turn_end_equivalence(game.board);
      if (!codes.empty()) {
        check_transition_equivalence(game, codes.front());
        check_transition_equivalence(game, codes[codes.size() / 2]);
        check_transition_equivalence(game, codes.back());
      }

      ++state_count;
      action_count += codes.size();
      if (codes.empty())
        break;
      const size_t selected =
          (static_cast<size_t>(seed) * 131 + static_cast<size_t>(ply) * 17) %
          codes.size();
      check(game.apply_action_code_trusted(codes[selected], false));
    }
  }

  check(state_count == 2770);
  check(action_count == 97354);
  check(digest == REACHABLE_ACTION_DIGEST);
}

void test_large_reachable_move_generation_corpus() {
  uint64_t state_count = 0;
  uint64_t action_count = 0;

  for (uint32_t seed = 0; seed < 144; ++seed) {
    Game game(10'000 + seed);
    game.simple_payment_mode = (seed & 1U) != 0;
    for (uint32_t ply = 0; ply < 96; ++ply) {
      const auto actions = game.legal_actions();
      const auto codes = game.legal_action_codes();
      check(actions.size() == codes.size());
      check(codes.size() == game.legal_action_count());

      for (size_t index = 0; index < actions.size(); ++index) {
        check(actions[index].pack() == codes[index]);
        check(game.is_legal(actions[index]));

        Game by_action = game.clone_light();
        Game by_code = game.clone_light();
        check(by_action.apply(actions[index], false));
        check(by_code.apply_action_code(codes[index], false));
        check(csplendor::snapshot::serialize(by_action) ==
              csplendor::snapshot::serialize(by_code));
      }

      ++state_count;
      action_count += codes.size();
      if (codes.empty())
        break;
      const size_t selected =
          (static_cast<size_t>(seed) * 257 + static_cast<size_t>(ply) * 29) %
          codes.size();
      check(game.apply_action_code_trusted(codes[selected], false));
    }
  }

  check(state_count >= 10'000);
  check(action_count >= 100'000);
}

void test_editor_edges_and_rejections() {
  Game game(7);
  Board &board = game.board.begin_editor_mutation();
  const int8_t visible = board.visible[0][0];
  board.visible[2][3] = visible;
  const auto visible_source =
      csplendor::rules::find_visible_card_source(board, visible);
  check(visible_source.level == 0 && visible_source.slot == 0);

  PlayerState &player = board.players[board.current_player];
  player.reserved[0] = visible;
  player.reserved[2] = visible;
  check(csplendor::rules::find_reserved_card_source(player, visible) == 0);
  check(!csplendor::rules::find_visible_card_source(board, -1));
  check(csplendor::rules::find_reserved_card_source(player, -2) == -1);

  const std::array<uint8_t, 6> available = {2, 2, 2, 2, 2, 1};
  std::array<uint8_t, 6> returned = {1, 0, 0, 0, 0, 0};
  check(csplendor::rules::validate_token_return(available, returned));
  returned[0] = 2;
  check(!csplendor::rules::validate_token_return(available, returned));
  returned = {3, 0, 0, 0, 0, 0};
  check(!csplendor::rules::validate_token_return(available, returned));

  PlayerState payment;
  payment.gems = {1, 1, 1, 1, 1, 1};
  payment.bonuses = {0, 0, 0, 0, 0};
  std::array<uint8_t, 5> excessive_gold = {7, 0, 0, 0, 0};
  check(!csplendor::rules::validate_purchase_payment(payment, get_card(0),
                                                     excessive_gold));
}

} // namespace

int main() {
  test_packed_return_code_helper();
  test_small_return_pattern_table();
  test_small_return_count_closed_form();
  test_reachable_differential_and_golden();
  test_large_reachable_move_generation_corpus();
  test_editor_edges_and_rejections();
  return 0;
}
