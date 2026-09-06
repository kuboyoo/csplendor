#include "board_editor.h"
#include "game.h"
#include "game_snapshot.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Coverage {
  std::array<uint64_t, ACTION_TYPE_COUNT> action_types{};
  uint64_t terminal_games = 0;
  uint64_t trajectory_transitions = 0;
  uint64_t all_legal_states = 0;
  uint64_t all_legal_transitions = 0;
  uint64_t oracle_checks = 0;
  uint64_t token_returns = 0;
  uint64_t gold_payments = 0;
  uint64_t hidden_reserve_purchases = 0;
  uint64_t blank_refills = 0;
  uint64_t automatic_nobles = 0;
  uint64_t multiple_noble_choices = 0;
  uint64_t final_round_entries = 0;
  uint64_t draws = 0;
};

[[noreturn]] void fail(const char *message) {
  std::cerr << "incremental_hash_unit: " << message << '\n';
  std::abort();
}

void require(bool condition, const char *message) {
  if (!condition)
    fail(message);
}

template <typename T, size_t Capacity>
bool stack_equal(const FixedStack<T, Capacity> &left,
                 const FixedStack<T, Capacity> &right) {
  if (left.count != right.count)
    return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index] != right[index])
      return false;
  }
  return true;
}

bool player_equal(const PlayerState &left, const PlayerState &right) {
  return left.gems == right.gems && left.packed_gems == right.packed_gems &&
         left.bonuses == right.bonuses &&
         left.packed_bonuses == right.packed_bonuses &&
         left.points == right.points && left.reserved == right.reserved &&
         left.reserved_is_hidden == right.reserved_is_hidden &&
         left.reserved_count == right.reserved_count &&
         left.purchased_count == right.purchased_count &&
         left.purchased_cards == right.purchased_cards &&
         left.acquired_nobles == right.acquired_nobles &&
         left.noble_eligibility_mask == right.noble_eligibility_mask;
}

bool board_semantic_equal(const Board &left, const Board &right) {
  if (left.bank != right.bank || left.visible != right.visible ||
      !stack_equal(left.nobles, right.nobles))
    return false;
  for (int level = 0; level < 3; ++level) {
    if (!stack_equal(left.decks[level], right.decks[level]))
      return false;
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    if (!player_equal(left.players[player], right.players[player]))
      return false;
  }
  return left.current_player == right.current_player &&
         left.turn == right.turn && left.final_round == right.final_round &&
         left.waiting_noble == right.waiting_noble &&
         left.winner == right.winner;
}

bool board_with_cache_equal(const Board &left, const Board &right) {
  return board_semantic_equal(left, right) &&
         left.cached_hash == right.cached_hash &&
         left.hash_valid == right.hash_valid;
}

bool game_semantic_equal(const Game &left, const Game &right) {
  return board_semantic_equal(left.board, right.board) &&
         left.simple_payment_mode == right.simple_payment_mode &&
         left.blank_refill_mode == right.blank_refill_mode;
}

std::vector<Action> ordered_actions(const Game &game) {
  const std::vector<Action> actions = game.legal_actions();
  const std::vector<uint64_t> codes = game.legal_action_codes();
  require(actions.size() == codes.size(),
          "legal Action/code collection sizes disagree");
  require(actions.size() == game.legal_action_count(),
          "legal action count disagrees with materialized actions");
  for (size_t index = 0; index < actions.size(); ++index) {
    require(actions[index].pack() == codes[index],
            "ordered legal Action/code values disagree");
  }
  return actions;
}

void require_ordered_legal_equal(const Game &left, const Game &right) {
  const std::vector<Action> left_actions = ordered_actions(left);
  const std::vector<Action> right_actions = ordered_actions(right);
  require(left_actions.size() == right_actions.size(),
          "shadow legal action sizes disagree");
  for (size_t index = 0; index < left_actions.size(); ++index) {
    require(left_actions[index].pack() == right_actions[index].pack(),
            "shadow ordered legal actions disagree");
  }
}

uint64_t require_hash_oracle(Board &board, Coverage &coverage) {
  const uint64_t oracle = board.compute_hash_uncached();
  const uint64_t maintained = board.hash();
  ++coverage.oracle_checks;
  require(board.hash_valid, "hash() did not make the cache valid");
  require(board.cached_hash == oracle,
          "cached exact hash differs from uncached oracle");
  require(maintained == oracle, "hash() differs from uncached oracle");
  return oracle;
}

void require_valid_cache_after_success(Board &board, Coverage &coverage) {
#ifdef CSPLENDOR_INCREMENTAL_EXACT_HASH
  require(board.hash_valid,
          "valid-cache trusted apply did not preserve cache validity");
  const uint64_t oracle = board.compute_hash_uncached();
  ++coverage.oracle_checks;
  require(board.cached_hash == oracle,
          "incrementally maintained exact hash differs from oracle");
#else
  require(!board.hash_valid,
          "non-incremental build unexpectedly retained exact cache");
  static_cast<void>(require_hash_oracle(board, coverage));
#endif
}

bool action_uses_hidden_reserve(const Game &game, const Action &action) {
  if (action.type != PURCHASE || !action.from_reserved ||
      game.board.current_player >= Board::NUM_PLAYERS)
    return false;
  const PlayerState &player = game.board.players[game.board.current_player];
  for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
    if (player.reserved[slot] == action.card_id &&
        player.reserved_is_hidden[slot])
      return true;
  }
  return false;
}

void observe_transition(const Game &before, const Action &action,
                        const Game &after, Coverage &coverage) {
  require(action.type < ACTION_TYPE_COUNT, "invalid action type was applied");
  ++coverage.action_types[static_cast<size_t>(action.type)];
  if (std::any_of(action.return_gems.begin(), action.return_gems.end(),
                  [](uint8_t count) { return count != 0; }))
    ++coverage.token_returns;
  if (action.type == PURCHASE &&
      std::any_of(action.gold_as.begin(), action.gold_as.end(),
                  [](uint8_t count) { return count != 0; }))
    ++coverage.gold_payments;
  if (action_uses_hidden_reserve(before, action))
    ++coverage.hidden_reserve_purchases;
  if (before.blank_refill_mode &&
      (action.type == RESERVE_VISIBLE ||
       (action.type == PURCHASE && !action.from_reserved)))
    ++coverage.blank_refills;

  const int player = before.board.current_player;
  if (player < Board::NUM_PLAYERS &&
      after.board.players[player].acquired_nobles.size() >
          before.board.players[player].acquired_nobles.size() &&
      action.type != VISIT_NOBLE)
    ++coverage.automatic_nobles;
  if (!before.board.waiting_noble && after.board.waiting_noble &&
      after.legal_action_count() > 1)
    ++coverage.multiple_noble_choices;
  if (!before.board.final_round && after.board.final_round)
    ++coverage.final_round_entries;
  if (after.board.winner == -2)
    ++coverage.draws;
}

void apply_shadow_pair(Game &valid_cache, Game &invalid_cache,
                       const Action &action, bool record_history,
                       Coverage &coverage) {
  require(game_semantic_equal(valid_cache, invalid_cache),
          "pre-transition shadow games differ");
  require_ordered_legal_equal(valid_cache, invalid_cache);
  const Game before = valid_cache.clone_light();

  static_cast<void>(require_hash_oracle(valid_cache.board, coverage));
  invalid_cache.board.invalidate_hash();
  require(!invalid_cache.board.hash_valid,
          "invalid-cache shadow remained valid before apply");

  require(valid_cache.apply_trusted(action, record_history),
          "valid-cache trusted apply failed for a legal action");
  require(invalid_cache.apply_trusted(action, false),
          "invalid-cache trusted apply failed for a legal action");

  require_valid_cache_after_success(valid_cache.board, coverage);
  require(!invalid_cache.board.hash_valid,
          "invalid-cache trusted apply unexpectedly published a cache");
  static_cast<void>(require_hash_oracle(invalid_cache.board, coverage));
  require(game_semantic_equal(valid_cache, invalid_cache),
          "valid/invalid-cache transition results differ");
  require_ordered_legal_equal(valid_cache, invalid_cache);
  observe_transition(before, action, valid_cache, coverage);
}

uint64_t next_random(uint64_t &state) {
  state += 0x9e3779b97f4a7c15ULL;
  uint64_t value = state;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

Action choose_trajectory_action(const std::vector<Action> &actions,
                                uint64_t &random, uint64_t ply) {
  require(!actions.empty(), "non-terminal trajectory has no legal action");
  const uint64_t value = next_random(random);

  const auto choose_type = [&](ActionType type) -> const Action * {
    std::vector<const Action *> matching;
    for (const Action &action : actions) {
      if (action.type == type)
        matching.push_back(&action);
    }
    if (matching.empty())
      return nullptr;
    return matching[static_cast<size_t>(value % matching.size())];
  };

  if (ply % 17 == 0) {
    if (const Action *selected = choose_type(RESERVE_DECK))
      return *selected;
  }
  if (ply % 17 == 1) {
    if (const Action *selected = choose_type(RESERVE_VISIBLE))
      return *selected;
  }
  if ((value & 3ULL) != 0 || ply >= 96) {
    if (const Action *selected = choose_type(PURCHASE))
      return *selected;
  }
  return actions[static_cast<size_t>(value % actions.size())];
}

void verify_terminal_trajectories(Coverage &coverage) {
  constexpr uint64_t kGameCount = 1000;
  constexpr uint64_t kMaximumPlies = 512;
  for (uint64_t seed = 0; seed < kGameCount; ++seed) {
    Game valid(seed);
    valid.simple_payment_mode = (seed & 1ULL) != 0;
    valid.blank_refill_mode = (seed & 2ULL) != 0;
    Game invalid = valid.clone_light();
    std::vector<Board> snapshots;
    uint64_t random = seed ^ 0xc5a17e5eedULL;

    for (uint64_t ply = 0; !valid.is_game_over(); ++ply) {
      require(ply < kMaximumPlies,
              "deterministic trajectory did not reach a terminal state");
      const std::vector<Action> actions = ordered_actions(valid);
      require(!actions.empty(), "non-terminal game emitted no legal action");
      static_cast<void>(require_hash_oracle(valid.board, coverage));
      snapshots.push_back(valid.board);
      const Action selected = choose_trajectory_action(actions, random, ply);
      apply_shadow_pair(valid, invalid, selected, true, coverage);
      ++coverage.trajectory_transitions;
    }

    require(invalid.is_game_over(), "shadow trajectory did not terminate");
    require(ordered_actions(valid).empty(),
            "terminal state exposed legal actions");
    ++coverage.terminal_games;

    require(valid.history.size() == snapshots.size(),
            "recorded action history length is wrong");
    require(valid.board_history.size() == snapshots.size(),
            "recorded Board history length is wrong");
    for (size_t remaining = snapshots.size(); remaining > 0; --remaining) {
      require(valid.undo(), "trajectory undo failed");
      require(board_with_cache_equal(valid.board, snapshots[remaining - 1]),
              "trajectory undo did not restore the exact Board/cache");
      static_cast<void>(require_hash_oracle(valid.board, coverage));
    }
    require(!valid.undo(), "trajectory exposed excess undo history");
  }
}

void verify_all_legal_successors(Coverage &coverage) {
  constexpr uint64_t kSeedCount = 32;
  constexpr uint64_t kMaximumPlies = 256;
  for (uint64_t seed = 0; seed < kSeedCount; ++seed) {
    Game game(10000 + seed);
    game.simple_payment_mode = (seed & 1ULL) != 0;
    game.blank_refill_mode = (seed & 2ULL) != 0;
    uint64_t random = seed ^ 0xa11ce5eedULL;

    for (uint64_t ply = 0; !game.is_game_over(); ++ply) {
      require(ply < kMaximumPlies,
              "all-legal corpus did not reach a terminal state");
      const std::vector<Action> actions = ordered_actions(game);
      require(!actions.empty(), "all-legal corpus has no legal action");
      ++coverage.all_legal_states;
      for (const Action &action : actions) {
        Game valid = game.clone_light();
        Game invalid = game.clone_light();
        apply_shadow_pair(valid, invalid, action, false, coverage);
        ++coverage.all_legal_transitions;
      }

      const Action selected = choose_trajectory_action(actions, random, ply);
      static_cast<void>(require_hash_oracle(game.board, coverage));
      require(game.apply_trusted(selected, false),
              "all-legal corpus trajectory apply failed");
      require_valid_cache_after_success(game.board, coverage);
    }
  }
}

Action first_action_of_type(const Game &game, ActionType type) {
  const std::vector<Action> actions = ordered_actions(game);
  const auto found = std::find_if(
      actions.begin(), actions.end(),
      [type](const Action &action) { return action.type == type; });
  require(found != actions.end(), "required action type is absent");
  return *found;
}

template <typename Predicate>
Action first_matching_action(const Game &game, Predicate predicate,
                             const char *message) {
  const std::vector<Action> actions = ordered_actions(game);
  const auto found = std::find_if(actions.begin(), actions.end(), predicate);
  require(found != actions.end(), message);
  return *found;
}

Game verified_child(const Game &source, const Action &action,
                    Coverage &coverage, bool record_history = false) {
  Game valid = source.clone_light();
  Game invalid = source.clone_light();
  apply_shadow_pair(valid, invalid, action, record_history, coverage);
  return valid;
}

void apply_setup_code(Game &game, uint64_t code, Coverage &coverage) {
  static_cast<void>(require_hash_oracle(game.board, coverage));
  require(game.apply_action_code(code, false), "fixture replay action failed");
  require_valid_cache_after_success(game.board, coverage);
}

Game make_hidden_reserve_fixture(Coverage &coverage) {
  Game game(17);
  game =
      verified_child(game, first_action_of_type(game, RESERVE_DECK), coverage);
  const int owner = 0;
  require(game.board.players[owner].reserved_count == 1 &&
              game.board.players[owner].reserved_is_hidden[0],
          "deck reservation did not create a hidden reserve");

  Board &board = game.board.begin_editor_mutation();
  board.current_player = static_cast<uint8_t>(owner);
  board.players[owner].bonuses = {10, 10, 10, 10, 10};
  board.players[owner].sync_packed();
  board.nobles.clear();
  static_cast<void>(require_hash_oracle(game.board, coverage));
  return game;
}

void verify_reserve_payment_and_blank_paths(Coverage &coverage) {
  Game initial(42);
  static_cast<void>(verified_child(
      initial, first_action_of_type(initial, RESERVE_VISIBLE), coverage));
  static_cast<void>(verified_child(
      initial, first_action_of_type(initial, RESERVE_DECK), coverage));

  Game blank(42);
  blank.blank_refill_mode = true;
  static_cast<void>(verified_child(
      blank, first_action_of_type(blank, RESERVE_VISIBLE), coverage));

  Game hidden = make_hidden_reserve_fixture(coverage);
  const Action hidden_purchase = first_matching_action(
      hidden,
      [](const Action &action) {
        return action.type == PURCHASE && action.from_reserved;
      },
      "hidden-reserve purchase action is absent");
  static_cast<void>(verified_child(hidden, hidden_purchase, coverage));

  Game returns(123);
  Board &return_board = returns.board.begin_editor_mutation();
  return_board.bank = {2, 2, 2, 2, 2, 5};
  return_board.players[0].gems = {2, 2, 2, 2, 2, 0};
  return_board.players[0].sync_packed();
  static_cast<void>(require_hash_oracle(returns.board, coverage));
  const Action return_action = first_matching_action(
      returns,
      [](const Action &action) {
        return std::any_of(action.return_gems.begin(), action.return_gems.end(),
                           [](uint8_t count) { return count != 0; });
      },
      "token-return action is absent");
  static_cast<void>(verified_child(returns, return_action, coverage));

  Game gold(42);
  Board &gold_board = gold.board.begin_editor_mutation();
  gold_board.bank = {3, 3, 3, 3, 3, 2};
  gold_board.players[0].gems = {1, 1, 1, 1, 1, 3};
  gold_board.players[0].sync_packed();
  static_cast<void>(require_hash_oracle(gold.board, coverage));
  const Action gold_action = first_matching_action(
      gold,
      [](const Action &action) {
        return action.type == PURCHASE &&
               std::any_of(action.gold_as.begin(), action.gold_as.end(),
                           [](uint8_t count) { return count != 0; });
      },
      "gold-payment purchase action is absent");
  static_cast<void>(verified_child(gold, gold_action, coverage));
}

void make_player_eligible_for_all_nobles(Game &game) {
  Board &board = game.board.begin_editor_mutation();
  PlayerState &player = board.players[board.current_player];
  player.bonuses = {10, 10, 10, 10, 10};
  player.sync_packed();
}

void verify_noble_paths(Coverage &coverage) {
  Game automatic(5);
  const uint8_t only_noble = automatic.board.nobles[0];
  {
    Board &board = automatic.board.begin_editor_mutation();
    board.nobles.clear();
    board.nobles.push_back_unchecked(only_noble);
  }
  make_player_eligible_for_all_nobles(automatic);
  static_cast<void>(require_hash_oracle(automatic.board, coverage));
  const size_t acquired_before =
      automatic.board.players[automatic.board.current_player]
          .acquired_nobles.size();
  Game automatic_child = verified_child(
      automatic, first_action_of_type(automatic, TAKE_DIFFERENT), coverage);
  require(automatic_child.board.players[0].acquired_nobles.size() ==
              acquired_before + 1,
          "single eligible noble was not acquired automatically");
  require(!automatic_child.board.waiting_noble,
          "single noble incorrectly entered choice phase");

  Game multiple(5);
  make_player_eligible_for_all_nobles(multiple);
  static_cast<void>(require_hash_oracle(multiple.board, coverage));
  Game choice = verified_child(
      multiple, first_action_of_type(multiple, TAKE_DIFFERENT), coverage);
  require(choice.board.waiting_noble,
          "multiple eligible nobles did not enter choice phase");
  const std::vector<Action> visits = ordered_actions(choice);
  require(visits.size() >= 2,
          "multiple-noble fixture has fewer than two visits");
  for (const Action &visit : visits) {
    require(visit.type == VISIT_NOBLE,
            "noble choice phase emitted a non-visit action");
    static_cast<void>(verified_child(choice, visit, coverage));
  }
}

void verify_final_round(Coverage &coverage) {
  Game game(9);
  {
    Board &board = game.board.begin_editor_mutation();
    board.players[0].points = 15;
  }
  static_cast<void>(require_hash_oracle(game.board, coverage));
  Game first = verified_child(game, first_action_of_type(game, TAKE_DIFFERENT),
                              coverage);
  require(first.board.final_round && !first.is_game_over(),
          "first final-round transition has wrong status");
  const std::vector<Action> replies = ordered_actions(first);
  require(!replies.empty(), "final-round reply is absent");
  Game terminal = verified_child(first, replies.front(), coverage);
  require(terminal.is_game_over(),
          "completed final round did not produce a terminal state");
}

constexpr std::array<uint64_t, 25> kForcedPassActions = {
    19ULL,         386ULL,        498ULL,        442ULL,       2208ULL,
    610ULL,        8390772ULL,    2088ULL,       458ULL,       2592ULL,
    2592ULL,       33563296ULL,   17ULL,         67117704ULL,  1048844ULL,
    8623497864ULL, 648ULL,        8589943304ULL, 520ULL,       8589935104ULL,
    252ULL,        1075841184ULL, 2176ULL,       536872960ULL, 2048ULL};

Game forced_pass_fixture(Coverage &coverage) {
  Game game(10);
  for (uint64_t code : kForcedPassActions)
    apply_setup_code(game, code, coverage);
  require(!game.is_game_over() && game.requires_forced_pass(),
          "forced-pass replay did not reach its target state");
  const std::vector<Action> actions = ordered_actions(game);
  require(actions.size() == 1 && actions.front().type == PASS,
          "forced-pass fixture did not emit one PASS");
  return game;
}

void verify_forced_pass_and_draw(Coverage &coverage) {
  const Game forced = forced_pass_fixture(coverage);
  const Game passed =
      verified_child(forced, ordered_actions(forced).front(), coverage);
  require(!passed.is_game_over() && passed.board.current_player == 0,
          "one-sided forced pass resolved incorrectly");

  Game draw = forced.clone_light();
  Board &board = draw.board.begin_editor_mutation();
  size_t replacement = board.decks[0].size();
  for (size_t index = 0; index < board.decks[0].size(); ++index) {
    if (board.decks[0][index] == 22) {
      replacement = index;
      break;
    }
  }
  require(replacement < board.decks[0].size(),
          "draw fixture replacement card is absent");
  const int8_t previous_visible = board.visible[0][0];
  board.visible[0][0] = static_cast<int8_t>(board.decks[0][replacement]);
  board.decks[0][replacement] = static_cast<uint8_t>(previous_visible);
  static_cast<void>(require_hash_oracle(draw.board, coverage));

  Game opponent = draw.clone_light();
  opponent.board.begin_editor_mutation().current_player = 0;
  require(opponent.requires_forced_pass(),
          "draw fixture opponent still has an ordinary move");
  require(draw.requires_forced_pass(),
          "draw fixture side to move still has an ordinary move");
  Game terminal = verified_child(draw, ordered_actions(draw).front(), coverage);
  require(terminal.is_game_over() && terminal.board.winner == -2,
          "two-sided forced pass did not resolve as a draw");
}

void verify_editor_fallback(Coverage &coverage) {
  Game game(42);
  const uint64_t before_hash = require_hash_oracle(game.board, coverage);
  Board before = game.board;
  std::vector<std::vector<int>> invalid_visible(
      3, std::vector<int>(Board::CARDS_PER_LEVEL, -1));
  invalid_visible[2][3] = CARD_COUNT;
  bool rejected = false;
  try {
    csplendor::state::editor::set_visible(game.board, invalid_visible);
  } catch (const std::invalid_argument &) {
    rejected = true;
  }
  require(rejected, "invalid editor visible payload was accepted");
  require(board_with_cache_equal(game.board, before),
          "failed editor mutation changed Board/cache");
  require(game.board.cached_hash == before_hash,
          "failed editor mutation changed cached hash");

  Board &board = game.board.begin_editor_mutation();
  board.players[0].gems = {3, 3, 3, 3, 3, 3};
  board.players[0].sync_packed();
  require(!game.board.hash_valid, "editor mutation did not invalidate cache");
  static_cast<void>(require_hash_oracle(game.board, coverage));
  require(game.legal_action_count() == MAX_MOVES,
          "editor fallback did not reach the MAX_MOVES cap");
  require(ordered_actions(game).size() == MAX_MOVES,
          "editor fallback materialization did not retain MAX_MOVES actions");
}

bool action_history_equal(const std::vector<Action> &left,
                          const std::vector<Action> &right) {
  if (left.size() != right.size())
    return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (left[index].pack() != right[index].pack())
      return false;
  }
  return true;
}

bool board_history_equal(const std::vector<Board> &left,
                         const std::vector<Board> &right) {
  if (left.size() != right.size())
    return false;
  for (size_t index = 0; index < left.size(); ++index) {
    if (!board_with_cache_equal(left[index], right[index]))
      return false;
  }
  return true;
}

void verify_copy_snapshot_and_determinization(Coverage &coverage) {
  Game game(71);
  static_cast<void>(require_hash_oracle(game.board, coverage));
  const Action action = ordered_actions(game).front();
  require(game.apply_trusted(action, true), "copy fixture apply failed");
  require_valid_cache_after_success(game.board, coverage);

  Game full = game.clone();
  Game light = game.clone_light();
  require(game_semantic_equal(game, full) && game_semantic_equal(game, light),
          "full/light clone changed game semantics");
  require(board_with_cache_equal(game.board, full.board) &&
              board_with_cache_equal(game.board, light.board),
          "full/light clone changed cache state");
  require(action_history_equal(game.history, full.history) &&
              board_history_equal(game.board_history, full.board_history),
          "full clone did not preserve history");
  require(light.history.empty() && light.board_history.empty(),
          "light clone retained history");

  Game invalid = game.clone_light();
  invalid.board.invalidate_hash();
  Game invalid_clone = invalid.clone_light();
  require(!invalid_clone.board.hash_valid &&
              game_semantic_equal(invalid, invalid_clone),
          "light clone did not preserve invalid-cache semantics");
  static_cast<void>(require_hash_oracle(invalid_clone.board, coverage));

  const std::string snapshot = csplendor::snapshot::serialize(game);
  Game restored = csplendor::snapshot::deserialize(snapshot);
  require(game_semantic_equal(game, restored),
          "snapshot roundtrip changed game semantics");
  require_ordered_legal_equal(game, restored);
  static_cast<void>(require_hash_oracle(restored.board, coverage));
  require(csplendor::snapshot::serialize(restored) == snapshot,
          "snapshot roundtrip bytes changed");

  Game hidden = make_hidden_reserve_fixture(coverage);
  const uint64_t hidden_hash = require_hash_oracle(hidden.board, coverage);
  Game world = hidden.shuffled_clone_portable(1, 0x51a7eULL);
  Game repeated = hidden.shuffled_clone_portable(1, 0x51a7eULL);
  require(game_semantic_equal(world, repeated),
          "portable determinization is not repeatable");
  require_ordered_legal_equal(world, repeated);
  require(!world.board.hash_valid && !repeated.board.hash_valid,
          "determinization did not invalidate exact caches");
  static_cast<void>(require_hash_oracle(world.board, coverage));
  static_cast<void>(require_hash_oracle(repeated.board, coverage));
  require(hidden.board.hash_valid && hidden.board.cached_hash == hidden_hash,
          "determinization mutated its source game/cache");
}

void verify_failed_transition_cache_contract(Coverage &coverage) {
  Action invalid;

  Game safe(83);
  static_cast<void>(require_hash_oracle(safe.board, coverage));
  const Board safe_before = safe.board;
  require(!safe.apply(invalid, true), "invalid safe action was accepted");
  require(board_with_cache_equal(safe.board, safe_before),
          "safe rejected action changed Board/cache");
  require(safe.history.empty() && safe.board_history.empty(),
          "safe rejected action added history");

  Game trusted(89);
  static_cast<void>(require_hash_oracle(trusted.board, coverage));
  const Board trusted_before = trusted.board;
  require(!trusted.apply_trusted(invalid, false),
          "invalid trusted action was accepted");
  require(board_semantic_equal(trusted.board, trusted_before),
          "failed trusted action changed Board semantics");
  require(!trusted.board.hash_valid,
          "failed trusted action retained a potentially stale valid cache");
  static_cast<void>(require_hash_oracle(trusted.board, coverage));
}

void require_coverage(const Coverage &coverage) {
  require(coverage.terminal_games == 1000,
          "terminal trajectory game count is incomplete");
  require(coverage.trajectory_transitions > 20000,
          "terminal trajectory transition count is unexpectedly small");
  require(coverage.all_legal_states > 500,
          "all-legal state corpus is unexpectedly small");
  require(coverage.all_legal_transitions > 5000,
          "all-legal transition corpus is unexpectedly small");
  for (uint64_t count : coverage.action_types)
    require(count != 0, "an ActionType received no incremental-hash coverage");
  require(coverage.token_returns != 0, "token-return path was not covered");
  require(coverage.gold_payments != 0, "gold-payment path was not covered");
  require(coverage.hidden_reserve_purchases != 0,
          "hidden-reserve purchase path was not covered");
  require(coverage.blank_refills != 0, "blank-refill path was not covered");
  require(coverage.automatic_nobles != 0,
          "automatic-noble path was not covered");
  require(coverage.multiple_noble_choices != 0,
          "multiple-noble path was not covered");
  require(coverage.final_round_entries != 0,
          "final-round path was not covered");
  require(coverage.draws != 0, "draw path was not covered");
  require(coverage.oracle_checks > coverage.trajectory_transitions * 3,
          "exact-hash oracle count is unexpectedly small");
}

} // namespace

int main() {
  Coverage coverage;
  verify_terminal_trajectories(coverage);
  verify_all_legal_successors(coverage);
  verify_reserve_payment_and_blank_paths(coverage);
  verify_noble_paths(coverage);
  verify_final_round(coverage);
  verify_forced_pass_and_draw(coverage);
  verify_editor_fallback(coverage);
  verify_copy_snapshot_and_determinization(coverage);
  verify_failed_transition_cache_contract(coverage);
  require_coverage(coverage);

  std::cout << "incremental_hash_unit: games=" << coverage.terminal_games
            << " trajectory_transitions=" << coverage.trajectory_transitions
            << " all_legal_states=" << coverage.all_legal_states
            << " all_legal_transitions=" << coverage.all_legal_transitions
            << " oracle_checks=" << coverage.oracle_checks << '\n';
  return 0;
}
