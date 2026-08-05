#include "game.h"
#include "undo_record.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
#include <vector>

namespace {

std::atomic<unsigned long long> allocations{0};

bool player_equal(const PlayerState &lhs, const PlayerState &rhs) {
  return lhs.gems == rhs.gems && lhs.packed_gems == rhs.packed_gems &&
         lhs.bonuses == rhs.bonuses &&
         lhs.packed_bonuses == rhs.packed_bonuses &&
         lhs.points == rhs.points && lhs.reserved == rhs.reserved &&
         lhs.reserved_is_hidden == rhs.reserved_is_hidden &&
         lhs.reserved_count == rhs.reserved_count &&
         lhs.purchased_count == rhs.purchased_count &&
         lhs.purchased_cards == rhs.purchased_cards &&
         lhs.acquired_nobles == rhs.acquired_nobles &&
         lhs.noble_eligibility_mask == rhs.noble_eligibility_mask;
}

template <typename T, size_t Capacity>
bool stack_equal(const FixedStack<T, Capacity> &lhs,
                 const FixedStack<T, Capacity> &rhs) {
  if (lhs.count != rhs.count)
    return false;
  for (size_t index = 0; index < lhs.size(); ++index) {
    if (lhs[index] != rhs[index])
      return false;
  }
  return true;
}

bool board_equal(const Board &lhs, const Board &rhs) {
  if (lhs.bank != rhs.bank || lhs.visible != rhs.visible ||
      !stack_equal(lhs.nobles, rhs.nobles))
    return false;
  for (int level = 0; level < 3; ++level) {
    if (!stack_equal(lhs.decks[level], rhs.decks[level]))
      return false;
  }
  for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
    if (!player_equal(lhs.players[player], rhs.players[player]))
      return false;
  }
  return lhs.current_player == rhs.current_player && lhs.turn == rhs.turn &&
         lhs.final_round == rhs.final_round &&
         lhs.waiting_noble == rhs.waiting_noble &&
         lhs.winner == rhs.winner && lhs.cached_hash == rhs.cached_hash &&
         lhs.hash_valid == rhs.hash_valid;
}

bool verify_actions(Game &game, unsigned &type_mask, int &transitions) {
  const std::vector<Action> actions = game.legal_actions();
  for (const Action &action : actions) {
    (void)game.board.hash();
    const Board snapshot = game.board;
    const auto record = csplendor::detail::UndoRecord::capture(game.board);
    if (!game.apply_trusted(action, false) || !record.restore(game.board) ||
        !board_equal(game.board, snapshot))
      return false;
    type_mask |= 1U << static_cast<unsigned>(action.type);
    ++transitions;
  }
  return true;
}

Game purchase_fixture(bool with_reserved) {
  Game game(17);
  PlayerState &player = game.board.players[game.board.current_player];
  player.bonuses = {10, 10, 10, 10, 10};
  player.sync_packed_bonuses_and_noble_eligibility();
  game.board.nobles.clear();
  if (with_reserved) {
    player.reserved[0] = game.board.visible[0][0];
    player.reserved_is_hidden[0] = true;
    player.reserved_count = 1;
  }
  game.board.invalidate_hash();
  return game;
}

bool verify_multi_undo(int &steps) {
  Game game(29);
  std::vector<Board> snapshots;
  std::vector<csplendor::detail::UndoRecord> records;
  snapshots.reserve(80);
  records.reserve(80);

  for (int ply = 0; ply < 80 && !game.is_game_over(); ++ply) {
    const std::vector<Action> actions = game.legal_actions();
    if (actions.empty())
      break;

    size_t selected = (static_cast<size_t>(ply) * 17 + 3) % actions.size();
    for (size_t index = 0; index < actions.size(); ++index) {
      if (actions[index].type == PURCHASE) {
        selected = index;
        break;
      }
    }

    (void)game.board.hash();
    snapshots.push_back(game.board);
    records.push_back(csplendor::detail::UndoRecord::capture(game.board));
    if (!game.apply_trusted(actions[selected], false))
      return false;
  }

  steps = static_cast<int>(records.size());
  for (size_t remaining = records.size(); remaining > 0; --remaining) {
    const size_t index = remaining - 1;
    if (!records[index].restore(game.board) ||
        !board_equal(game.board, snapshots[index]))
      return false;
  }
  return true;
}

struct BenchmarkResult {
  long long snapshot_ns = 0;
  long long delta_ns = 0;
  unsigned long long snapshot_allocations = 0;
  unsigned long long delta_allocations = 0;
};

BenchmarkResult benchmark_purchase_undo() {
  constexpr int iterations = 30000;
  Game snapshot_game = purchase_fixture(false);
  PlayerState &snapshot_player =
      snapshot_game.board.players[snapshot_game.board.current_player];
  snapshot_player.purchased_cards = {0, 1, 2, 3, 4, 5, 6, 7};
  snapshot_player.purchased_count = 8;
  const std::vector<Action> actions = snapshot_game.legal_actions();
  const Action action = *std::find_if(
      actions.begin(), actions.end(),
      [](const Action &candidate) { return candidate.type == PURCHASE; });
  Game delta_game = snapshot_game.clone_light();

  BenchmarkResult result;
  allocations.store(0, std::memory_order_relaxed);
  const auto snapshot_start = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    Board previous = snapshot_game.board;
    if (!snapshot_game.apply_trusted(action, false))
      std::abort();
    snapshot_game.board = std::move(previous);
  }
  result.snapshot_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - snapshot_start)
                           .count();
  result.snapshot_allocations = allocations.load(std::memory_order_relaxed);

  allocations.store(0, std::memory_order_relaxed);
  const auto delta_start = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    const auto record =
        csplendor::detail::UndoRecord::capture(delta_game.board);
    if (!delta_game.apply_trusted(action, false) ||
        !record.restore(delta_game.board))
      std::abort();
  }
  result.delta_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - delta_start)
                        .count();
  result.delta_allocations = allocations.load(std::memory_order_relaxed);
  return result;
}

} // namespace

void *operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *memory = std::malloc(size))
    return memory;
  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }

int main() {
  unsigned type_mask = 0;
  int transitions = 0;
  bool all_equal = true;

  for (uint64_t seed = 0; seed < 8; ++seed) {
    Game game(seed);
    all_equal &= verify_actions(game, type_mask, transitions);
    game.blank_refill_mode = true;
    all_equal &= verify_actions(game, type_mask, transitions);
  }

  Game purchase = purchase_fixture(false);
  all_equal &= verify_actions(purchase, type_mask, transitions);
  purchase.blank_refill_mode = true;
  all_equal &= verify_actions(purchase, type_mask, transitions);
  Game reserved_purchase = purchase_fixture(true);
  all_equal &= verify_actions(reserved_purchase, type_mask, transitions);

  Game noble(31);
  noble.board.players[0].bonuses = {10, 10, 10, 10, 10};
  noble.board.players[0].sync_packed_bonuses_and_noble_eligibility();
  noble.board.waiting_noble = true;
  noble.board.invalidate_hash();
  all_equal &= verify_actions(noble, type_mask, transitions);

  Game automatic_noble(37);
  automatic_noble.board.players[0].bonuses = {10, 10, 10, 10, 10};
  automatic_noble.board.players[0].sync_packed_bonuses_and_noble_eligibility();
  const uint8_t kept_noble = automatic_noble.board.nobles[0];
  automatic_noble.board.nobles.clear();
  automatic_noble.board.nobles.push_back(kept_noble);
  automatic_noble.board.invalidate_hash();
  all_equal &= verify_actions(automatic_noble, type_mask, transitions);

  Game returns(41);
  returns.board.players[0].gems = {2, 2, 2, 2, 2, 0};
  returns.board.players[0].sync_packed_gems();
  returns.board.invalidate_hash();
  all_equal &= verify_actions(returns, type_mask, transitions);

  Game failed(43);
  (void)failed.board.hash();
  const Board failed_snapshot = failed.board;
  const auto failed_record =
      csplendor::detail::UndoRecord::capture(failed.board);
  const bool rejected = !failed.apply(Action{}, false);
  const bool failed_equal = board_equal(failed.board, failed_snapshot) &&
                            failed_record.restore(failed.board) &&
                            board_equal(failed.board, failed_snapshot);

  Game failed_trusted(45);
  (void)failed_trusted.board.hash();
  const Board failed_trusted_snapshot = failed_trusted.board;
  const auto failed_trusted_record =
      csplendor::detail::UndoRecord::capture(failed_trusted.board);
  const bool trusted_rejected =
      !failed_trusted.apply_trusted(Action{}, false);
  const bool failed_trusted_equal =
      failed_trusted_record.restore(failed_trusted.board) &&
      board_equal(failed_trusted.board, failed_trusted_snapshot);

  Game hash_invalid(46);
  const Board hash_invalid_snapshot = hash_invalid.board;
  const auto hash_invalid_record =
      csplendor::detail::UndoRecord::capture(hash_invalid.board);
  const Action hash_invalid_action = hash_invalid.legal_actions().front();
  const bool hash_invalid_equal =
      hash_invalid.apply_trusted(hash_invalid_action, false) &&
      hash_invalid_record.restore(hash_invalid.board) &&
      board_equal(hash_invalid.board, hash_invalid_snapshot);

  Game edited(47);
  edited.board.players[0].purchased_cards = {1, 2, 3};
  const auto guarded_record =
      csplendor::detail::UndoRecord::capture(edited.board);
  edited.board.players[0].purchased_cards.clear();
  const bool editor_guard = !guarded_record.can_restore(edited.board);

  Game invalid_player(49);
  invalid_player.board.current_player = Board::NUM_PLAYERS;
  const auto invalid_player_record =
      csplendor::detail::UndoRecord::capture(invalid_player.board);
  const bool invalid_player_guard =
      !invalid_player_record.can_restore(invalid_player.board);

  int multi_steps = 0;
  const bool multi_equal = verify_multi_undo(multi_steps);

  Game debug_history(53);
  (void)debug_history.board.hash();
  const Board debug_snapshot = debug_history.board;
  const Action debug_action = debug_history.legal_actions().front();
  const bool debug_dual_run = debug_history.apply(debug_action, true) &&
                              debug_history.undo() &&
                              board_equal(debug_history.board, debug_snapshot);
  const BenchmarkResult benchmark = benchmark_purchase_undo();

  std::cout << "{\"sizeof_record\":"
            << sizeof(csplendor::detail::UndoRecord)
            << ",\"sizeof_board\":" << sizeof(Board)
            << ",\"type_mask\":" << type_mask
            << ",\"transitions\":" << transitions
            << ",\"multi_steps\":" << multi_steps
            << ",\"all_equal\":" << all_equal
            << ",\"failed_rejected\":" << rejected
            << ",\"failed_equal\":" << failed_equal
            << ",\"trusted_rejected\":" << trusted_rejected
            << ",\"failed_trusted_equal\":" << failed_trusted_equal
            << ",\"hash_invalid_equal\":" << hash_invalid_equal
            << ",\"multi_equal\":" << multi_equal
            << ",\"debug_dual_run\":" << debug_dual_run
            << ",\"editor_guard\":" << editor_guard
            << ",\"invalid_player_guard\":" << invalid_player_guard
            << ",\"snapshot_ns\":" << benchmark.snapshot_ns
            << ",\"delta_ns\":" << benchmark.delta_ns
            << ",\"snapshot_allocations\":"
            << benchmark.snapshot_allocations
            << ",\"delta_allocations\":" << benchmark.delta_allocations
            << "}\n";
}
