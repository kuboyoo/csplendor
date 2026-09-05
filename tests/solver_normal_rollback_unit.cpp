#include "solver_normal_rollback.h"
#include "reveal_verified_solver.h"
#include "visible_only_solver.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

namespace {
std::atomic<size_t> allocations{0};
using csplendor::solver_internal::NormalBranchRollback;
using csplendor::detail::UndoRecord;
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void verify(Game &game, const Action &action, unsigned &types) {
  const Board parent = game.board;
  Game reference = game.clone_light();
  const bool expected = reference.apply_trusted(action, false);
  const auto history = game.history.size();
  const auto board_history = game.board_history.size();
  const bool blank = game.blank_refill_mode;
  const bool simple = game.simple_payment_mode;
  {
    NormalBranchRollback rollback(game);
    const bool actual = rollback.apply(action);
    require(actual == expected && UndoRecord::boards_equal(game.board, reference.board),
            "ordinary child differs from reference");
    if (actual) types |= 1U << static_cast<unsigned>(action.type);
    game.blank_refill_mode = !game.blank_refill_mode;
    game.simple_payment_mode = !game.simple_payment_mode;
    const auto before = allocations.load();
    rollback.restore();
    require(allocations.load() == before, "rollback allocated");
    require(UndoRecord::boards_equal(game.board, parent), "parent fields not restored");
    require(game.blank_refill_mode == blank && game.simple_payment_mode == simple,
            "parent modes not restored");
    rollback.restore(); // Must be idempotent, including false apply.
  }
  require(game.history.size() == history && game.board_history.size() == board_history,
          "normal guard changed public history");
}

void verify_game(Game &game, unsigned &types) {
  for (const auto &action : game.legal_actions()) verify(game, action, types);
}

void transition_corpus(unsigned &types, size_t &visited) {
  for (bool simple : {false, true}) for (bool blank : {false, true}) {
    Game game(42);
    game.simple_payment_mode = simple;
    game.blank_refill_mode = blank;
    for (int ply = 0; ply < 55 && !game.is_game_over(); ++ply) {
      if (ply % 2) (void)game.board.hash();
      else { game.board.hash_valid = false; game.board.cached_hash = 0x1234; }
      const auto actions = game.legal_actions();
      if (actions.empty()) break;
      for (const auto &action : actions) { verify(game, action, types); ++visited; }
      size_t chosen = (ply * 17 + 3) % actions.size();
      for (size_t i = 0; i < actions.size(); ++i)
        if (actions[i].type == PURCHASE) { chosen = i; break; }
      require(game.apply_trusted(actions[chosen], false), "corpus progression failed");
    }
  }
}

void special_cases(unsigned &types) {
  Game game(17);
  auto &player = game.board.players[0];
  player.bonuses = {10, 10, 10, 10, 10};
  player.gems = {2, 2, 2, 2, 2, 0};
  player.purchased_cards = {0, 1, 2, 3, 4, 5, 6, 7};
  player.purchased_count = 8;
  player.sync_packed();
  game.board.invalidate_hash();
  verify_game(game, types); // auto/deferred nobles, return patterns, gold.
  game.board.waiting_noble = true;
  verify_game(game, types);
  game.board.waiting_noble = false;
  for (int slot = 0; slot < 3; ++slot) {
    player.reserved[slot] = game.board.visible[0][slot];
    player.reserved_is_hidden[slot] = slot % 2 == 0;
  }
  player.reserved_count = 3;
  verify_game(game, types); // reserved first/middle/last slot shifts.
  game.board.final_round = true;
  game.board.current_player = 1;
  game.board.players[1] = player;
  verify_game(game, types);
  for (auto &deck : game.board.decks) deck.count = 1;
  verify_game(game, types);
  for (auto &deck : game.board.decks) deck.clear();
  verify_game(game, types);

  Action pass;
  pass.type = PASS;
  verify(game, pass, types);

  Action invalid;
  invalid.type = RESERVE_VISIBLE;
  invalid.card_id = -1;
  verify(game, invalid, types); // false with an invalidated hash transaction.
}

void nested_and_failure() {
  Game game(29);
  const Board parent = game.board;
  try {
    NormalBranchRollback outer(game);
    Action reserve;
    reserve.type = RESERVE_DECK;
    reserve.deck_level = 0;
    require(outer.apply(reserve), "outer reserve failed");
    const auto after_outer = game.board;
    {
      NormalBranchRollback inner(game);
      require(inner.apply(game.legal_actions().front()), "inner apply failed");
    }
    require(UndoRecord::boards_equal(game.board, after_outer), "inner touched parent");
    // Simulate a nested reveal restoring only its ACTIVE prefix. The outer
    // popped slot is inactive here, but must become the correct card again.
    game.board.decks[0].data[parent.decks[0].count - 1] = 89;
    throw std::runtime_error("visitor cancellation");
  } catch (const std::runtime_error &error) {
    require(std::string(error.what()) == "visitor cancellation", "unexpected nested failure");
  }
  require(UndoRecord::boards_equal(game.board, parent), "nested unwind lost deck top");

  // Actual validate-then-mutate payment failure after the earlier colour.
  game.board.players[0].gems = {4, 0, 0, 0, 0, 0};
  game.board.players[0].sync_packed();
  game.board.invalidate_hash();
  const Board before = game.board;
  {
    NormalBranchRollback rollback(game);
    rollback.mark_mutated();
    Board::RuleMutator<false> mutation(game.board);
    const auto &card = get_card(1); // first colour costs 1, last colour fails.
    auto gold = std::array<uint8_t, 5>{};
    gold[4] = 255;
    const bool applied = csplendor::detail::purchase_card<true>(game.board, mutation, card, gold);
    require(!applied, "invalid partial payment unexpectedly succeeded");
    require(game.board.bank != before.bank, "partial payment did not mutate an earlier colour");
    // mutation dies before rollback; no late hash publication is possible.
  }
  require(UndoRecord::boards_equal(game.board, before), "partial failure not restored");
}

void visitor_exit_cases() {
  Game game(29);
  const Board parent = game.board;
  const auto actions = game.legal_actions();
  for (bool eligible : {false, true})
    for (size_t stop : {size_t{0}, actions.size() / 2, actions.size() - 1})
      for (bool throwing : {false, true}) {
        size_t visited = 0;
        bool caught = false;
        auto visit = [&]() {
          for (size_t index = 0; index < actions.size(); ++index) {
            NormalBranchRollback rollback(game, eligible);
            require(rollback.apply(actions[index]), "visitor action failed");
            ++visited;
            if (index == stop) {
              if (throwing) throw std::logic_error("visitor stop");
              return false; // Early return, with the current child still live.
            }
          }
          return true;
        };
        try { require(!visit(), "visitor did not stop"); }
        catch (const std::logic_error &error) {
          require(std::string(error.what()) == "visitor stop", "unexpected visitor failure");
          caught = true;
        }
        require(caught == throwing && visited == stop + 1, "wrong visitor exit position");
        require(UndoRecord::boards_equal(game.board, parent), "visitor exit lost parent");
      }
}

#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
using csplendor::solver_internal::RollbackFaultPoint;
struct InjectedFailure {};
RollbackFaultPoint requested;
int remaining_hits = 0;
void inject(RollbackFaultPoint point) {
  if (point == requested && --remaining_hits == 0) throw InjectedFailure{};
}
struct HookScope {
  HookScope(RollbackFaultPoint point, int hits = 1) {
    requested = point;
    remaining_hits = hits;
    csplendor::solver_internal::rollback_fault_hook = inject;
  }
  ~HookScope() { csplendor::solver_internal::rollback_fault_hook = nullptr; }
};

csplendor::solver_internal::RevealSearchCancellationToken *active_cancel = nullptr;
void cancel_after_transition(RollbackFaultPoint point) {
  if (point == RollbackFaultPoint::AfterHashCommit && --remaining_hits == 0)
    active_cancel->request_cancel();
}
struct CancelHookScope {
  explicit CancelHookScope(csplendor::solver_internal::RevealSearchCancellationToken &token) {
    active_cancel = &token;
    remaining_hits = 10;
    csplendor::solver_internal::rollback_fault_hook = cancel_after_transition;
  }
  ~CancelHookScope() {
    csplendor::solver_internal::rollback_fault_hook = nullptr;
    active_cancel = nullptr;
  }
};

void cancellation_during_search() {
  auto token = std::make_shared<csplendor::solver_internal::RevealSearchCancellationToken>();
  Game game(42);
  const Board parent = game.board;
  RevealVerifiedSolver solver(0, 3, 500, 0.0, {}, false, 100000, 500000,
                              UINT64_MAX, false, 0, true, true, token);
  {
    CancelHookScope cancellation(*token);
    const auto result = solver.solve(game);
    require(token->is_cancelled() && result.stats.nodes > 0 &&
                result.unknown_reason == "search cancelled", "mid-search cancellation failed");
  }
  require(UndoRecord::boards_equal(game.board, parent), "cancellation changed caller state");
  token->reset();
  const auto resumed = solver.solve(game);
  RevealVerifiedSolver fresh(0, 3, 500, 0.0, {}, false, 100000, 500000,
                             UINT64_MAX, false, 0, true, true);
  const auto expected = fresh.solve(game);
  require(resumed.unknown_reason == expected.unknown_reason &&
              resumed.stats.nodes == expected.stats.nodes && resumed.stats.legal_moves == expected.stats.legal_moves,
          "solver could not resume after mid-search cancellation");
}

void fault_injection() {
  for (bool simple : {false, true}) for (bool blank : {false, true})
    for (auto point : {RollbackFaultPoint::PaymentColour, RollbackFaultPoint::ProvenanceAppend,
                       RollbackFaultPoint::SourceRemoval, RollbackFaultPoint::NobleAcquired,
                       RollbackFaultPoint::BeforeHashCommit, RollbackFaultPoint::AfterHashCommit}) {
      Game game(17);
      game.simple_payment_mode = simple;
      game.blank_refill_mode = blank;
      auto &player = game.board.players[0];
      player.gems = {2, 2, 2, 2, 2, 0};
      if (point == RollbackFaultPoint::NobleAcquired) {
        player.bonuses = {10, 10, 10, 10, 10};
        game.board.waiting_noble = true;
      } else {
        // Keep a legal <=10-token purchase available without overflowing the
        // full-action generator with an artificial 30-token editor fixture.
        player.reserved[0] = 5;
        player.reserved_count = 1;
      }
      player.sync_packed();
      game.board.invalidate_hash();
      const auto actions = game.legal_actions();
      auto action = std::find_if(actions.begin(), actions.end(), [&](const Action &a) {
        return a.type == (point == RollbackFaultPoint::NobleAcquired ? VISIT_NOBLE : PURCHASE);
      });
      require(action != actions.end(), "injection action missing");
      for (bool valid_hash : {false, true}) {
        if (valid_hash) (void)game.board.hash();
        else { game.board.hash_valid = false; game.board.cached_hash = 123; }
        const Board parent = game.board;
        const int hits = point == RollbackFaultPoint::PaymentColour ? 5 : 1;
        for (int nth = 1; nth <= hits; ++nth) {
          bool caught = false;
          try {
            HookScope injection(point, nth);
            NormalBranchRollback rollback(game);
            rollback.apply(*action);
          } catch (const InjectedFailure &) { caught = true; }
          require(caught, "fault did not reach the real transition");
          require(UndoRecord::boards_equal(game.board, parent), "injected failure lost parent state");
        }
      }
    }

  // A real solver must propagate the exception and remain reusable afterwards.
  Game game(42);
  RevealVerifiedSolver solver(0, 3, 500, 0.0, {}, false, 100000, 500000,
                              UINT64_MAX, false, 0, true, true);
  bool caught = false;
  try {
    HookScope injection(RollbackFaultPoint::AfterHashCommit);
    (void)solver.solve(game);
  } catch (const InjectedFailure &) { caught = true; }
  require(caught, "solver swallowed injected exception");
  const auto resumed = solver.solve(game);
  RevealVerifiedSolver fresh(0, 3, 500, 0.0, {}, false, 100000, 500000,
                             UINT64_MAX, false, 0, true, true);
  const auto expected = fresh.solve(game);
  require(resumed.unknown_reason == expected.unknown_reason &&
              resumed.stats.nodes == expected.stats.nodes && resumed.stats.legal_moves == expected.stats.legal_moves,
          "solver could not resume after injected failure");

  VisibleOnlySolver visible(500, 0.0);
  caught = false;
  try {
    HookScope injection(RollbackFaultPoint::AfterHashCommit);
    (void)visible.solve(game);
  } catch (const InjectedFailure &) { caught = true; }
  require(caught, "visible solver swallowed injected exception");
  const auto visible_resumed = visible.solve(game);
  VisibleOnlySolver visible_fresh(500, 0.0);
  const auto visible_expected = visible_fresh.solve(game);
  require(visible_resumed.winner == visible_expected.winner &&
              visible_resumed.unknown_reason == visible_expected.unknown_reason &&
              visible_resumed.stats.nodes == visible_expected.stats.nodes &&
              visible_resumed.stats.legal_moves == visible_expected.stats.legal_moves,
          "visible solver could not resume after injected failure");
}
#endif
} // namespace

void *operator new(std::size_t size) {
  allocations.fetch_add(1, std::memory_order_relaxed);
  if (void *p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }

int main() {
  try {
    unsigned types = 0;
    size_t visited = 0;
    transition_corpus(types, visited);
    special_cases(types);
    nested_and_failure();
    visitor_exit_cases();
#ifdef CSPLENDOR_VERIFY_SOLVER_ROLLBACK
    fault_injection();
    cancellation_during_search();
#endif
    require(types == ((1U << 7) - 1), "ordinary action type coverage incomplete");
    std::cout << "transitions=" << visited << " types=" << types
              << " guard_bytes=" << sizeof(NormalBranchRollback)
              << " record_bytes=" << sizeof(UndoRecord) << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
