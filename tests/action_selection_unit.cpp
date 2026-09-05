#include "action_encoder.h"
#include "game_snapshot.h"
#include "solver_take_groups.h"
#include "solver_types.h"
#include "undo_record.h"
#include <iostream>
#include <map>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
using csplendor::solver_internal::ActionOrderKey;
ActionOrderKey order(uint64_t code) {
  Action a = Action::unpack(code);
  int rank = (a.type == PURCHASE || a.type == VISIT_NOBLE) ? 0 :
      (a.type == RESERVE_VISIBLE ? 1 :
       (a.type == TAKE_DIFFERENT || a.type == TAKE_SAME ? 2 : 3));
  return {rank, is_valid_card_id(a.card_id) ? -get_card(a.card_id).points : 0, code};
}
auto representatives(const Game &game, const std::vector<uint64_t> &codes) {
  std::map<std::string, ActionOrderKey> groups;
  for (uint64_t code : codes) {
    if (Action::unpack(code).type == RESERVE_DECK) continue;
    Game child = game.clone_light();
    if (!child.apply_action_code_trusted(code)) continue;
    auto snapshot = csplendor::snapshot::serialize(child);
    auto item = groups.find(snapshot);
    if (item == groups.end() || order(code) < item->second)
      groups[snapshot] = order(code);
  }
  return groups;
}
size_t verify_groups(const Game &game) {
  const auto codes = game.legal_action_codes();
  const auto reduced = csplendor::solver_internal::group_take_candidates(game, codes);
  for (auto code : reduced)
    require(std::find(codes.begin(), codes.end(), code) != codes.end(), "invented candidate");
  const auto original = representatives(game, codes);
  const auto grouped = representatives(game, reduced);
  require(original.size() == grouped.size(), "child group count changed");
  auto a = original.begin();
  auto b = grouped.begin();
  for (; a != original.end(); ++a, ++b)
    require(a->first == b->first && a->second.code == b->second.code,
            "child snapshot or minimum ActionOrderKey changed");
  // The production path sorts these same keys after final child grouping.
  return codes.size() - reduced.size();
}
void group_corpus() {
  size_t removed = 0;
  for (int seed = 1; seed <= 8; ++seed) for (bool simple : {false, true}) {
    Game game(seed);
    game.simple_payment_mode = simple;
    for (int ply = 0; ply < 64 && !game.is_game_over(); ++ply) {
      removed += verify_groups(game);
      require(game.apply_random_action(static_cast<uint64_t>(seed * 7919 + ply * 97)), "progress");
    }
  }
  Game game(42);
  for (int tokens : {8, 9, 10, 11, 15}) {
    game.board.players[0].gems = {2, 2, 2, 1, 1, static_cast<uint8_t>(tokens - 8)};
    game.board.players[0].sync_packed();
    game.board.invalidate_hash();
    removed += verify_groups(game);
    if (tokens > 10)
      require(csplendor::solver_internal::group_take_candidates(game, game.legal_action_codes()) ==
              game.legal_action_codes(), "editor fallback changed candidates");
  }
  require(removed > 0, "corpus never exercises grouping");
  std::cout << "take candidates removed in oracle corpus: " << removed << '\n';
}

size_t verify_indices(const Game &game) {
  // Reference remains the existing full enumeration, NOT count/rank selection.
  const auto actions = game.legal_actions();
  const auto codes = game.legal_action_codes();
  require(actions.size() == codes.size() && codes.size() == game.legal_action_count(), "count parity");
  for (size_t index = 0; index < actions.size(); ++index) {
    require(codes[index] == actions[index].pack() && game.legal_action_code_at(index) == codes[index], "rank code parity");
    Game expected = game.clone_light(), actual = game.clone_light();
    bool result = expected.apply_trusted(actions[index], true);
    require(actual.apply_legal_action_index(index, true) == result &&
            csplendor::detail::UndoRecord::boards_equal(expected.board, actual.board), "rank child parity");
    require(actual.history.size() == expected.history.size() && actual.board_history.size() == expected.board_history.size(), "rank history parity");
    actual = game.clone_light();
    require(actual.apply_random_action(index + actions.size() * 7919ULL, true) == result &&
            csplendor::detail::UndoRecord::boards_equal(expected.board, actual.board), "RNG modulo/child parity");
  }
  for (uint16_t index : {static_cast<uint16_t>(codes.size()), uint16_t(MAX_MOVES), uint16_t(UINT16_MAX)}) {
    Game actual = game.clone_light();
    require(game.legal_action_code_at(index) == 0 && !actual.apply_legal_action_index(index, true) &&
            csplendor::detail::UndoRecord::boards_equal(game.board, actual.board) && actual.history.empty(), "invalid index contract");
  }
  return actions.size();
}
void index_corpus() {
  size_t checked = 0;
  for (int seed = 1; seed <= 8; ++seed) for (bool simple : {false, true}) {
    Game game(seed);
    game.simple_payment_mode = simple;
    for (int ply = 0; ply < 64 && !game.is_game_over(); ++ply) {
      checked += verify_indices(game);
      const auto actions = game.legal_actions();
      require(game.apply_trusted(actions[(seed * 7919 + ply * 97) % actions.size()]), "reference progress");
    }
  }
  Game game(1);
  game.board.players[0].gems = {3,3,3,3,3,3};
  game.board.players[0].sync_packed();
  game.board.invalidate_hash();
  require(game.legal_action_count() == MAX_MOVES, "final cap fixture");
  checked += verify_indices(game);
  game.board.bank = {0,0,0,0,0,0};
  for (auto &row : game.board.visible) row = {86,86,86,86};
  game.board.players[0].gems = {5,3,0,3,3,14};
  game.board.players[0].reserved = {86,86,86};
  game.board.players[0].reserved_count = 3;
  game.board.players[0].sync_packed();
  require(game.legal_action_count() == MAX_MOVES, "base cap fixture");
  checked += verify_indices(game);
  for (auto &row : game.board.visible) row.fill(-1);
  for (auto &deck : game.board.decks) deck.clear();
  game.board.players[0].reserved.fill(-1);
  game.board.players[0].reserved_count = 0;
  require(game.requires_forced_pass(), "forced pass fixture");
  checked += verify_indices(game);
  game.board.winner = -2;
  checked += verify_indices(game);
  require(!game.apply_random_action(UINT64_MAX), "terminal random");
  game.board.winner = -1;
  game.board.current_player = 255;
  checked += verify_indices(game);
  std::cout << "full-action indices checked: " << checked << '\n';
}
} // namespace
int main() {
  try { group_corpus(); index_corpus(); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
