#ifndef CSPLENDOR_DFPN_MATE_SOLVER_H
#define CSPLENDOR_DFPN_MATE_SOLVER_H

// df-pn mate search beside the depth-first reveal-verified solver
// (doc/mate_dfpn_cpp_design.md). The search itself is DfpnSearch<Host>,
// instantiated by RevealVerifiedSolver::Impl: it calls the depth-first
// solver's move generation, transitions, keys, cuts and decided tables, so
// both searches prove the same AND/OR graph and differ only in the order.

#include "action.h"
#include "game.h"
#include "solver_types.h"
#include "solver_win_threshold.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <vector>

enum class MateSearchAlgorithm : uint8_t { DFS, DFPN };

struct DfpnSearchStats {
  uint64_t expansions = 0;         // nodes whose moves were generated
  uint64_t table_stores = 0;       // undecided (pn, dn) stored
  uint64_t table_replacements = 0; // stores that evicted another node
  size_t table_bytes = 0;          // bytes allocated by the df-pn tables
};

namespace csplendor::solver_internal {

using DfpnNumber = uint32_t;
inline constexpr DfpnNumber DFPN_INF = UINT32_MAX;
// Undecided sums saturate below INF, so INF always means "decided".
inline constexpr DfpnNumber DFPN_LIMIT = DFPN_INF - 1;

inline DfpnNumber dfpn_add(DfpnNumber a, DfpnNumber b) noexcept {
  if (a == DFPN_INF || b == DFPN_INF)
    return DFPN_INF;
  const uint64_t sum = uint64_t{a} + b;
  return sum >= DFPN_LIMIT ? DFPN_LIMIT : static_cast<DfpnNumber>(sum);
}

// Threshold for a child that takes `child` of a sum `total` < `threshold`.
inline DfpnNumber dfpn_share(DfpnNumber threshold, DfpnNumber total,
                             DfpnNumber child) noexcept {
  if (threshold == DFPN_INF)
    return DFPN_INF;
  return threshold - total + child; // total is exact below a finite threshold
}

// The 1 + epsilon bound on the second-best sibling (epsilon = 1/4): the
// chosen child may grow somewhat past its sibling before control returns.
inline DfpnNumber dfpn_second_bound(DfpnNumber second) noexcept {
  if (second == DFPN_INF)
    return DFPN_INF;
  const uint64_t bound = uint64_t{second} + 1 + second / 4;
  return bound >= DFPN_LIMIT ? DFPN_INF : static_cast<DfpnNumber>(bound);
}

// Undecided df-pn values, fixed capacity (doc/mate_dfpn_cpp_design.md 第 5 節).
// Buckets of WAYS slots; a bucket without a free slot overwrites its entry
// with the least work (nodes visited below it). The bucket count grows from
// MIN_BUCKETS by doubling and jumps from max/8 to the maximum derived from the
// byte budget, so a small search never initialises a large table and the
// table never exceeds the budget (the last growth briefly holds 1/8 more).
template <typename Key, typename Hash> class DfpnTable {
public:
  struct Slot {
    Key key;
    DfpnNumber pn = 0;
    DfpnNumber dn = 0;
    uint32_t work = 0;
  };

  static constexpr size_t WAYS = 4;
  static constexpr size_t MIN_BUCKETS = 256;

  static constexpr size_t buckets_for_bytes(size_t bytes) noexcept {
    const size_t buckets = bytes / (WAYS * sizeof(Slot));
    return buckets < 1 ? 1 : buckets;
  }

  explicit DfpnTable(size_t budget_bytes)
      : max_buckets_(buckets_for_bytes(budget_bytes)) {}

  size_t size() const noexcept { return size_; }
  size_t bucket_count() const noexcept { return slots_.size() / WAYS; }
  size_t max_bucket_count() const noexcept { return max_buckets_; }
  size_t memory_bytes() const noexcept { return slots_.size() * sizeof(Slot); }
  uint64_t replacements() const noexcept { return replacements_; }

  const Slot *find(const Key &key) const noexcept {
    if (slots_.empty())
      return nullptr;
    const Slot *bucket = &slots_[home(key, bucket_count())];
    for (size_t way = 0; way < WAYS; ++way)
      if (!bucket[way].key.empty() && bucket[way].key == key)
        return &bucket[way];
    return nullptr;
  }

  // Below the maximum the table grows instead of replacing: at 3/4 load or
  // when the key's bucket is full.
  void store(const Key &key, DfpnNumber pn, DfpnNumber dn, uint32_t work) {
    while (bucket_count() < max_buckets_ &&
           (size_ + 1 > bucket_count() * WAYS * 3 / 4 || bucket_full(key)))
      grow();
    place(Slot{key, pn, dn, work});
  }

  void erase(const Key &key) noexcept {
    Slot *slot = const_cast<Slot *>(find(key));
    if (slot != nullptr) {
      *slot = Slot{};
      --size_;
    }
  }

private:
  static uint64_t mix(uint64_t h) noexcept {
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h;
  }

  static size_t home(const Key &key, size_t buckets) noexcept {
    const uint64_t h = mix(static_cast<uint64_t>(Hash{}(key)));
    return static_cast<size_t>(((h >> 32) * buckets) >> 32) * WAYS;
  }

  bool bucket_full(const Key &key) const noexcept {
    const Slot *bucket = &slots_[home(key, bucket_count())];
    for (size_t way = 0; way < WAYS; ++way)
      if (bucket[way].key.empty() || bucket[way].key == key)
        return false;
    return true;
  }

  void place(const Slot &entry) noexcept {
    Slot *bucket = &slots_[home(entry.key, bucket_count())];
    Slot *victim = nullptr;
    for (size_t way = 0; way < WAYS; ++way) {
      Slot &slot = bucket[way];
      if (!slot.key.empty() && slot.key == entry.key) {
        slot = entry;
        return;
      }
      if (slot.key.empty()) {
        if (victim == nullptr || !victim->key.empty())
          victim = &slot;
      } else if (victim == nullptr ||
                 (!victim->key.empty() && slot.work < victim->work)) {
        victim = &slot;
      }
    }
    if (victim->key.empty())
      ++size_;
    else
      ++replacements_;
    *victim = entry;
  }

  void grow() {
    size_t buckets = MIN_BUCKETS;
    if (!slots_.empty()) {
      buckets = bucket_count() * 2;
      if (buckets > max_buckets_ / 8)
        buckets = max_buckets_;
    }
    buckets = std::min(buckets, max_buckets_);
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(buckets * WAYS, Slot{});
    size_ = 0;
    for (const Slot &slot : old)
      if (!slot.key.empty())
        place(slot);
  }

  std::vector<Slot> slots_;
  size_t size_ = 0;
  size_t max_buckets_ = 1;
  uint64_t replacements_ = 0;
};

// df-pn+ with thresholds over the reveal-verified AND/OR graph
// (doc/mate_dfpn_cpp_design.md 第 2〜4 節). Host is RevealVerifiedSolver::Impl,
// which declares this template a friend.
template <typename Host> class DfpnSearch {
  using OrderedAction = typename Host::OrderedAction;
  using Rollback = typename Host::ScopedBranchRollback;
  using ExactMemo = typename Host::ExactMemo;
  using BlankMemo = typename Host::Memo;
  using ExactTable = DfpnTable<typename ExactMemo::key_type,
                               typename Host::ExactDepthStateKeyHash>;
  using BlankTable =
      DfpnTable<typename BlankMemo::key_type, typename Host::DepthStateKeyHash>;

public:
  // `exact`: the root is searched with concrete reveals (the blank children
  // of the blank dominance cut are depth-first searches, so the whole budget
  // goes to the exact table).
  DfpnSearch(Host &host, size_t table_bytes, bool exact,
             DfpnSearchStats &stats)
      : host_(host), stats_(stats), exact_table_(exact ? table_bytes : 0),
        blank_table_(exact ? 0 : table_bytes) {
    // The depth-first search treats an empty path as its root (required
    // root action): a sentinel keeps the leaf searches off that rule.
    leaf_path_.push(typename Host::DepthStateKey{});
  }

  ForceStatus solve(Game &game, int depth, bool exact) {
    const Value value = exact ? search<true>(game, depth, DFPN_INF, DFPN_INF, 0)
                              : search<false>(game, depth, DFPN_INF, DFPN_INF, 0);
    return value.pn == 0   ? ForceStatus::PROVEN
           : value.dn == 0 ? ForceStatus::REFUTED
                           : ForceStatus::UNKNOWN;
  }

private:
  struct Value {
    DfpnNumber pn = 1;
    DfpnNumber dn = 1;
  };
  static constexpr Value PROVEN_VALUE{0, DFPN_INF};
  static constexpr Value REFUTED_VALUE{DFPN_INF, 0};

  static Value value_of(ForceStatus status) {
    if (status == ForceStatus::PROVEN)
      return PROVEN_VALUE;
    if (status == ForceStatus::REFUTED)
      return REFUTED_VALUE;
    // Only a final-round node whose last move leaves the game running.
    throw SearchLimitExceeded("dfpn: undecided final-round node");
  }

  // A reveal of a move (doc/mate_dfpn_cpp_design.md 第 2.1 節): CONCRETE
  // counts, BLANK (the blank dominance child) counts until refuted, DORMANT
  // reveals wake up when the BLANK child is refuted, DEAD is a refuted BLANK.
  enum class Kind : uint8_t { CONCRETE, BLANK, DORMANT, DEAD };
  // The estimate of unseen siblings: the largest pn and dn among the
  // searched siblings that are still undecided (1/1 before there is one).
  struct Estimate {
    Value most;
    bool any = false;
    void add(const Value &value) {
      if (value.pn == 0 || value.dn == 0)
        return;
      most = any ? Value{std::max(most.pn, value.pn),
                         std::max(most.dn, value.dn)}
                 : value;
      any = true;
    }
    Value get() const { return most; }
  };
  // A child not searched yet takes its siblings' Estimate (doc 第 6 節): an
  // unseen move is assumed as hard as the hardest seen one, so a node neither
  // sweeps every sibling before it looks deeper nor leaves a deep sibling
  // whenever an unseen one looks cheaper.
  struct Outcome {
    int reveal = -1;
    Kind kind = Kind::CONCRETE;
    bool known = false;
    Value value;
  };
  struct Move {
    OrderedAction action;
    size_t first = 0;
    size_t count = 0;
    bool expanded = false;
    bool known = false;
    Value value;
    Estimate estimate; // of the reveals (see Outcome)
  };
  struct Frame {
    std::vector<OrderedAction> actions;
    std::vector<Move> moves;
    std::vector<Outcome> outcomes;
    std::vector<int> cards;
  };

  static bool counts(Kind kind) noexcept {
    return kind == Kind::CONCRETE || kind == Kind::BLANK;
  }

  Frame &frame_at(size_t ply) {
    while (frames_.size() <= ply)
      frames_.push_back(std::make_unique<Frame>());
    return *frames_[ply];
  }

  template <bool Exact> auto &memo() {
    if constexpr (Exact)
      return host_.exact_memo_;
    else
      return host_.memo_;
  }

  template <bool Exact> auto &table() {
    if constexpr (Exact)
      return exact_table_;
    else
      return blank_table_;
  }

  void record_table_bytes() noexcept {
    stats_.table_bytes = std::max(
        stats_.table_bytes, exact_table_.memory_bytes() + blank_table_.memory_bytes());
  }

  // One position node; returns its (pn, dn) when it is decided or reaches a
  // threshold. `game` is at the node and is left there.
  template <bool Exact>
  Value search(Game &game, int depth, DfpnNumber th_pn, DfpnNumber th_dn,
               size_t ply) {
    host_.check_limits();
    const uint64_t nodes_before = ++host_.stats_.nodes;
    const int attacker = host_.attacker_;

    if (game.is_game_over()) {
      ++host_.stats_.terminal_nodes;
      return game.winner() == attacker ? PROVEN_VALUE : REFUTED_VALUE;
    }
    if (game.current_player() == attacker && depth <= 0)
      return REFUTED_VALUE;
    if (Host::can_resolve_final_round(game)) {
      ForceStatus direct = ForceStatus::UNKNOWN;
      if (host_.resolve_final_round_direct(game, direct, Exact)) {
        ++host_.stats_.final_round_direct_resolutions;
        host_.stats_.final_round_score_prunes += direct == ForceStatus::PROVEN;
        return value_of(direct);
      }
    }

    auto &decided = memo<Exact>();
    using MemoKey = typename std::remove_reference_t<decltype(decided)>::key_type;
    using MemoEntry =
        typename std::remove_reference_t<decltype(decided)>::mapped_type;
    const typename Host::StateKey state = host_.state_key(game, Exact);
    const MemoKey key{state, depth};
    if (MemoEntry *hit = decided.find(key)) {
      ++host_.stats_.memo_hits;
      if constexpr (MemoEntry::tracks_persistence) {
        if (hit->generation() != host_.search_generation_)
          ++host_.stats_.persistent_memo_hits;
        hit->set_generation(host_.search_generation_);
        hit->set_last_touched(++host_.cache_touch_counter_);
      }
      return value_of(hit->status());
    }
    if (ply > 0 && depth <= LEAF_DEPTH) {
      // Near the horizon the subtrees are small: the depth-first search
      // decides them outright (and stores them in the shared table).
      const ForceStatus status = host_.forced_win_with_memo(
          game, leaf_path_, depth, Exact, decided);
      return value_of(status);
    }
    auto &undecided = table<Exact>();
    if (const auto *slot = undecided.find(key)) {
      if (slot->pn >= th_pn || slot->dn >= th_dn)
        return Value{slot->pn, slot->dn};
    }

    const auto decide = [&](ForceStatus status, MemoEntry entry) {
      entry.set_status(status);
      host_.store_entry(decided, key, entry);
      undecided.erase(key);
      return value_of(status);
    };
    const MemoEntry no_action{ForceStatus::UNKNOWN, 0, -1, false, 0};
    if (Host::can_resolve_final_round(game)) {
      const MemoEntry resolved =
          host_.template resolve_final_round<MemoEntry>(game, Exact);
      return decide(resolved.status(), resolved);
    }
    const bool last_move = last_move_cut_applies(game, attacker, depth);
    if (last_move && !can_win_this_turn(game.board)) {
      ++host_.stats_.terminal_nodes;
      return decide(ForceStatus::REFUTED, no_action);
    }
    if (reach_cut_applies(game, attacker, depth)) {
      CardList oracle_now;
      HiddenCards hidden;
      host_.collect_hidden_cards(game, Exact, oracle_now, hidden);
      if (!attacker_may_reach_win_in_two_moves(game.board, attacker, hidden)) {
        ++host_.stats_.reach_cut_refutations;
        ++host_.stats_.terminal_nodes;
        return decide(ForceStatus::REFUTED, no_action);
      }
    }
    if (host_.last_reply_cut_applies(game, depth)) {
      ++host_.stats_.last_reply_cut_attempts;
      if (host_.attacker_wins_after_any_reply(game, Exact)) {
        ++host_.stats_.last_reply_cut_proofs;
        return decide(ForceStatus::PROVEN, no_action);
      }
    }

    const int mover = game.current_player();
    const bool attacker_node = mover == attacker;
    Frame &frame = frame_at(ply);
    generate_moves<Exact>(game, state, depth, attacker_node, ply == 0, last_move,
                          frame);
    if (frame.moves.empty()) {
      ++host_.stats_.terminal_nodes;
      return decide(ForceStatus::REFUTED, no_action);
    }
    ++stats_.expansions;

    std::vector<Move> &moves = frame.moves;
    const auto entry_for = [&](size_t index, int reveal) {
      const OrderedAction &action = moves[index].action;
      return MemoEntry{ForceStatus::UNKNOWN, action.code, reveal, true,
                       moves.size(), Host::is_replayable(action)};
    };
    const auto first_reveal = [&](size_t index) {
      return frame.outcomes[moves[index].first].reveal;
    };
    Estimate estimate;
    for (;;) {
      const Value typical = estimate.get();
      // OR: pn = min, dn = sum over the moves; AND: pn = sum, dn = min.
      DfpnNumber pn = attacker_node ? DFPN_INF : 0;
      DfpnNumber dn = attacker_node ? 0 : DFPN_INF;
      size_t best = 0;
      DfpnNumber best_key = DFPN_INF;
      DfpnNumber second = DFPN_INF;
      for (size_t index = 0; index < moves.size(); ++index) {
        const Value value = moves[index].known ? moves[index].value : typical;
        const DfpnNumber select = attacker_node ? value.pn : value.dn;
        if (attacker_node) {
          pn = std::min(pn, value.pn);
          dn = dfpn_add(dn, value.dn);
        } else {
          pn = dfpn_add(pn, value.pn);
          dn = std::min(dn, value.dn);
        }
        if (select < best_key) {
          second = best_key;
          best_key = select;
          best = index;
        } else if (select < second) {
          second = select;
        }
      }

      if (pn == 0 || dn == 0) {
        const bool proven = pn == 0;
        if (attacker_node == proven) {
          // The deciding move: the attacker's proof or the defender's
          // refutation (the reveal that refuted it).
          size_t index = 0;
          while (proven ? moves[index].value.pn != 0
                        : moves[index].value.dn != 0)
            ++index;
          int reveal = first_reveal(index);
          if (!proven) {
            const Move &move = moves[index];
            for (size_t o = move.first; o < move.first + move.count; ++o)
              if (counts(frame.outcomes[o].kind) &&
                  frame.outcomes[o].value.dn == 0)
                reveal = frame.outcomes[o].reveal;
          }
          if (host_.line_transplant_)
            (proven ? host_.line_proofs_ : host_.line_refutations_)
                .put(host_.line_hash_, moves[index].action.code);
          return decide(proven ? ForceStatus::PROVEN : ForceStatus::REFUTED,
                        entry_for(index, reveal));
        }
        return decide(proven ? ForceStatus::PROVEN : ForceStatus::REFUTED,
                      entry_for(0, first_reveal(0)));
      }
      if (pn >= th_pn || dn >= th_dn) {
        const uint64_t work = host_.stats_.nodes - nodes_before + 1;
        undecided.store(key, pn, dn,
                        work > UINT32_MAX ? UINT32_MAX
                                          : static_cast<uint32_t>(work));
        ++stats_.table_stores;
        stats_.table_replacements = exact_table_.replacements() +
                                    blank_table_.replacements();
        record_table_bytes();
        return Value{pn, dn};
      }

      const Value chosen = moves[best].known ? moves[best].value : typical;
      DfpnNumber move_pn = 0;
      DfpnNumber move_dn = 0;
      if (attacker_node) {
        move_pn = std::min(th_pn, dfpn_second_bound(second));
        move_dn = dfpn_share(th_dn, dn, chosen.dn);
      } else {
        move_pn = dfpn_share(th_pn, pn, chosen.pn);
        move_dn = std::min(th_dn, dfpn_second_bound(second));
      }
      const uint64_t line_before = host_.line_hash_;
      if (attacker_node)
        host_.line_hash_ = Host::mix_line_hash(line_before, moves[best].action.code);
      search_move<Exact>(game, depth, mover, frame, moves[best], move_pn,
                         move_dn, ply);
      host_.line_hash_ = line_before;
      estimate.add(moves[best].value);
    }
  }

  // The depth-first solver's move list for this node (same generation,
  // filters and order), as unexpanded moves.
  template <bool Exact>
  void generate_moves(const Game &game, const typename Host::StateKey &state,
                      int depth, bool attacker_node, bool root, bool last_move,
                      Frame &frame) {
    std::vector<OrderedAction> &actions = frame.actions;
    frame.moves.clear();
    frame.outcomes.clear();
    if (last_move)
      Host::fill_winning_last_moves(game, actions);
    else
      host_.fill_ordered_actions(game, actions, Exact);
    host_.filter_probe_actions(attacker_node, actions);
    host_.apply_order_hints(state.board_hash(), attacker_node, actions);
    if constexpr (Exact)
      host_.prefer_iterative_action(state, depth, game.current_player(),
                                    actions);
    host_.prefer_line_move(attacker_node, actions);
    if (attacker_node)
      actions = host_.forced_attacker_actions(game, actions, depth, root);
    append_moves(actions, frame);
  }

  void append_moves(const std::vector<OrderedAction> &actions, Frame &frame) {
    host_.stats_.legal_moves += actions.size();
    for (const OrderedAction &action : actions) {
      Move move;
      move.action = action;
      frame.moves.push_back(move);
    }
  }


  // A move is the AND of its reveals: pn = sum, dn = min over the counted ones.
  template <bool Exact>
  void search_move(Game &game, int depth, int mover, Frame &frame, Move &move,
                   DfpnNumber th_pn, DfpnNumber th_dn, size_t ply) {
    if (!move.expanded)
      expand_move<Exact>(game, frame, move);
    for (;;) {
      DfpnNumber pn = 0;
      DfpnNumber dn = DFPN_INF;
      size_t best = move.first;
      DfpnNumber second = DFPN_INF;
      bool found = false;
      // The blank child only proves: as in the depth-first search it is
      // searched once the first reveal is proven, never to refute the move.
      const Outcome &first = frame.outcomes[move.first];
      const bool blank_open = first.known && first.value.pn == 0;
      for (size_t index = move.first; index < move.first + move.count;
           ++index) {
        const Outcome &outcome = frame.outcomes[index];
        if (!counts(outcome.kind))
          continue;
        const Value value = outcome.known ? outcome.value : move.estimate.get();
        pn = dfpn_add(pn, value.pn);
        if (outcome.kind == Kind::BLANK && !blank_open)
          continue;
        if (!found || value.dn < dn) {
          if (found)
            second = dn;
          dn = value.dn;
          best = index;
          found = true;
        } else if (value.dn < second) {
          second = value.dn;
        }
      }
      move.value = Value{pn, dn};
      move.known = true;
      if (pn == 0 || dn == 0 || pn >= th_pn || dn >= th_dn)
        return;

      Outcome &outcome = frame.outcomes[best];
      const Value current = outcome.known ? outcome.value : move.estimate.get();
      const DfpnNumber child_pn = dfpn_share(th_pn, pn, current.pn);
      const DfpnNumber child_dn = std::min(th_dn, dfpn_second_bound(second));
      const bool blank = outcome.kind == Kind::BLANK;
      const bool first_blank_visit = blank && !outcome.known;
      {
        Rollback rollback(host_, game);
        rollback.mark_mutated();
        if (!apply_outcome<Exact>(game, move.action, outcome))
          throw SearchLimitExceeded("dfpn: a reveal could not be applied");
        const int next_depth = host_.child_depth(depth, mover, game);
        // The blank child is decided outright by the depth-first blank
        // search, as in the depth-first solver: it only matters when proven,
        // and thresholds would revisit it at every re-expansion.
        outcome.value =
            blank ? (host_.forced_win_with_memo(game, leaf_path_, next_depth,
                                                false, host_.memo_) ==
                             ForceStatus::PROVEN
                         ? PROVEN_VALUE
                         : REFUTED_VALUE)
                  : search<Exact>(game, next_depth, child_pn, child_dn, ply + 1);
        outcome.known = true;
      }
      move.estimate.add(outcome.value);
      if (!blank)
        continue;
      host_.stats_.blank_cut_attempts += first_blank_visit;
      host_.stats_.blank_cut_proofs += outcome.value.pn == 0;
      if (outcome.value.dn == 0) {
        // The blank child proves nothing about the rest: reveal them.
        outcome.kind = Kind::DEAD;
        for (size_t index = move.first; index < move.first + move.count;
             ++index)
          if (frame.outcomes[index].kind == Kind::DORMANT)
            frame.outcomes[index].kind = Kind::CONCRETE;
      }
    }
  }

  // The reveals of a move, as the depth-first solver enumerates them
  // (for_each_proof_outcome in the exact search, for_each_search_outcome in
  // the blank search); the exact refill keeps the first reveal, the blank
  // child and the dormant rest.
  template <bool Exact>
  void expand_move(const Game &game, Frame &frame, Move &move) {
    std::vector<Outcome> &outcomes = frame.outcomes;
    std::vector<int> &cards = frame.cards;
    move.first = outcomes.size();
    const OrderedAction &ordered = move.action;
    const bool oracle = !Host::is_replayable(ordered);
    const Action action = oracle ? Action{} : Action::unpack(ordered.code);
    const auto push = [&](int reveal, Kind kind) {
      Outcome outcome;
      outcome.reveal = reveal;
      outcome.kind = kind;
      outcomes.push_back(outcome);
    };
    if (!oracle && action.type == RESERVE_DECK) {
      host_.deck_reserve_cards(game, action, cards);
      host_.stats_.deck_reserve_candidates += cards.size();
      for (int card : cards)
        push(card, Kind::CONCRETE);
    } else if (Exact && Host::visible_refill_level(action) >= 0 &&
               !game.board.decks[Host::visible_refill_level(action)].empty()) {
      const int level = Host::visible_refill_level(action);
      const int slot = Host::visible_refill_slot(game.board, action);
      host_.visible_refill_cards(game, action, level, slot, cards);
      for (size_t index = 0; index < cards.size(); ++index) {
        if (index == 1)
          push(-1, Kind::BLANK);
        push(cards[index], index == 0 ? Kind::CONCRETE : Kind::DORMANT);
      }
    } else {
      push(-1, Kind::CONCRETE);
    }
    move.count = outcomes.size() - move.first;
    if (move.count == 0)
      throw SearchLimitExceeded("dfpn: a move has no reveal");
    move.expanded = true;
  }

  template <bool Exact>
  bool apply_outcome(Game &game, const OrderedAction &ordered,
                     const Outcome &outcome) {
    if (!Host::is_replayable(ordered))
      return host_.apply_tracked(
          game, [&] { return host_.apply_oracle_action(game, ordered); });
    const Action action = Action::unpack(ordered.code);
    if (action.type == RESERVE_DECK) {
      ++host_.stats_.deck_reserve_branches;
      return host_.apply_deck_reserve_outcome(game, action, outcome.reveal);
    }
    if (Exact && outcome.reveal >= 0) {
      const int level = Host::visible_refill_level(action);
      const int slot = Host::visible_refill_slot(game.board, action);
      return host_.apply_visible_refill_outcome(game, action, level, slot,
                                                outcome.reveal);
    }
    if (outcome.kind == Kind::BLANK)
      game.blank_refill_mode = true; // restored by the rollback
    return host_.apply_action_tracked(game, action);
  }

  Host &host_;
  DfpnSearchStats &stats_;
  // Nodes with at most this many attacker moves left are decided by the
  // depth-first search (doc 第 3 節).
  static constexpr int LEAF_DEPTH = 1;
  typename Host::DepthPath leaf_path_{64};
  ExactTable exact_table_;
  BlankTable blank_table_;
  std::vector<std::unique_ptr<Frame>> frames_;
};

} // namespace csplendor::solver_internal

#endif // CSPLENDOR_DFPN_MATE_SOLVER_H
