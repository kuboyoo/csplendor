#include "reveal_verified_solver.h"
#include "action.h"
#include "perf_counters.h"
#include "reveal_solver_components.h"
#include "rule_transition.h"
#include "solver_action_filter.h"
#include "solver_card_equivalence.h"
#include "solver_path.h"
#include "solver_reveal_order.h"
#include "solver_search_scratch.h"
#include "solver_tt_types.h"
#include <algorithm>
#include <chrono>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using csplendor::solver_internal::ActionOrderKey;
using csplendor::solver_internal::CardEquivalenceMask;
using csplendor::solver_internal::CardIdSet;
using csplendor::solver_internal::ForceStatus;
using csplendor::solver_internal::HiddenOutcomeCatalog;
using csplendor::solver_internal::OracleActionMetadata;
using csplendor::solver_internal::ProofDagBuildAborted;
using csplendor::solver_internal::RevealProofDagBuilder;
using csplendor::solver_internal::RevealSearchCancellationToken;
using csplendor::solver_internal::RevealSearchState;
using csplendor::solver_internal::ScopedPathEntry;
using csplendor::solver_internal::SearchLimit;
using csplendor::solver_internal::SearchLimitExceeded;
using csplendor::solver_internal::StateKeyCore;
using csplendor::solver_internal::TerminalResult;

class RevealVerifiedSolver::Impl {
public:
  Impl(int attacker, int depth, uint64_t max_nodes, double time_limit_seconds,
       std::vector<uint64_t> preferred_attacker_actions, bool include_proof_dag,
       size_t proof_dag_node_limit, size_t proof_dag_edge_limit,
       uint64_t required_root_action, bool strict_preferred_attacker_actions,
       size_t strict_preferred_attacker_prefix,
       bool exhaustive_attacker_actions, bool exact_reveal_search,
       std::shared_ptr<RevealSearchCancellationToken> cancellation_token)
      : attacker_(attacker), depth_(depth), max_nodes_(max_nodes),
        limits_(max_nodes, time_limit_seconds, std::move(cancellation_token)),
        preferred_attacker_actions_(std::move(preferred_attacker_actions)),
        include_proof_dag_(include_proof_dag),
        proof_builder_(proof_dag_node_limit, proof_dag_edge_limit),
        required_root_action_(required_root_action),
        strict_preferred_attacker_actions_(strict_preferred_attacker_actions),
        strict_preferred_attacker_prefix_(strict_preferred_attacker_prefix),
        exhaustive_attacker_actions_(exhaustive_attacker_actions),
        exact_reveal_search_(exact_reveal_search) {}

  RevealVerifiedSearchResult solve(const Game &input) {
    max_cache_states_ = 0;
    Game game = begin_search(input);

    RevealVerifiedSearchResult result;
    result.attacker = attacker_;
    result.depth = depth_;
    try {
      DepthPath path(path_reserve_capacity(depth_));
      const ForceStatus status =
          forced_win(game, path, depth_, exact_reveal_search_);
      result.proven = status == ForceStatus::PROVEN;
      result.reason = result.proven ? "all_reveals_verified"
                                    : "candidate_mate_not_verified";
      result.line = principal_line(exact_reveal_search_);
      if (result.proven && include_proof_dag_)
        result.proof_dag = build_proof_dag();
    } catch (const SearchLimitExceeded &exc) {
      result.reason = exc.what();
      result.unknown_reason = exc.what();
    }
    verify_reveal_state(game);

    stats_.elapsed_ms = limits_.elapsed_ms();
    result.memoized_states =
        exact_reveal_search_ ? exact_memo_.size() : memo_.size();
    result.stats = stats_;
    return result;
  }

  RevealVerifiedSearchResult solve_reusing_exact_cache(
      const Game &input, int depth, uint64_t max_nodes,
      double time_limit_seconds,
      std::vector<uint64_t> preferred_attacker_actions,
      std::shared_ptr<RevealSearchCancellationToken> cancellation_token,
      size_t max_cache_states) {
    depth_ = depth;
    max_nodes_ = max_nodes;
    limits_ = SearchLimit(max_nodes, time_limit_seconds,
                          std::move(cancellation_token));
    preferred_attacker_actions_ = std::move(preferred_attacker_actions);
    required_root_action_ = UINT64_MAX;
    strict_preferred_attacker_actions_ = false;
    strict_preferred_attacker_prefix_ = 0;
    exhaustive_attacker_actions_ = true;
    exact_reveal_search_ = true;
    include_proof_dag_ = false;
    max_cache_states_ = max_cache_states;

    trim_exact_cache(max_cache_states);
    Game game = begin_search(input, false);
    RevealVerifiedSearchResult result;
    result.attacker = attacker_;
    result.depth = depth_;
    try {
      DepthPath path(path_reserve_capacity(depth_));
      const ForceStatus status = forced_win(game, path, depth_, true);
      result.proven = status == ForceStatus::PROVEN;
      result.reason = result.proven ? "all_reveals_verified"
                                    : "candidate_mate_not_verified";
      result.line = principal_line(true);
    } catch (const SearchLimitExceeded &exc) {
      result.reason = exc.what();
      result.unknown_reason = exc.what();
    }
    verify_reveal_state(game);

    stats_.elapsed_ms = limits_.elapsed_ms();
    trim_exact_cache(max_cache_states);
    result.memoized_states = exact_memo_.size();
    result.stats = stats_;
    return result;
  }

  void clear_exact_cache() { exact_memo_.clear(); }

  void trim_exact_cache(size_t max_cache_states) {
    if (!max_cache_states || exact_memo_.size() <= max_cache_states)
      return;

    std::vector<uint64_t> touches;
    touches.reserve(exact_memo_.size());
    for (const auto &item : exact_memo_)
      touches.push_back(item.second.last_touched());
    const size_t remove_count = exact_memo_.size() - max_cache_states;
    std::nth_element(touches.begin(), touches.begin() + remove_count,
                     touches.end());
    const uint64_t keep_from = touches[remove_count];
    for (auto it = exact_memo_.begin();
         it != exact_memo_.end() && exact_memo_.size() > max_cache_states;) {
      if (it->second.last_touched() < keep_from)
        it = exact_memo_.erase(it);
      else
        ++it;
    }
    // last_touched is normally unique.  This also handles generation-zero
    // aggregate entries and counter wrap without exceeding the hard bound.
    for (auto it = exact_memo_.begin();
         it != exact_memo_.end() && exact_memo_.size() > max_cache_states;) {
      if (it->second.last_touched() == keep_from)
        it = exact_memo_.erase(it);
      else
        ++it;
    }
  }

  size_t exact_cache_size() const noexcept { return exact_memo_.size(); }

  RevealVerifiedFrontierResult expand_frontier(const Game &input,
                                               size_t edge_limit) {
    Game game = begin_search(input);

    RevealVerifiedFrontierResult result;
    result.attacker = attacker_;
    result.depth = depth_;
    result.player = game.current_player();
    result.winner = game.winner();
    result.waiting_noble = game.board.waiting_noble;
    result.kind = game.is_game_over()             ? "terminal"
                  : can_resolve_final_round(game) ? "final_round_summary"
                                                  : "state";
    if (game.is_game_over()) {
      result.resolution =
          game.winner() == attacker_ ? "attacker_win" : "non_attacker_win";
    }
    try {
      DepthPath path(path_reserve_capacity(depth_));
      const ForceStatus status = forced_win(game, path, depth_, false);
      result.proven = status == ForceStatus::PROVEN;
      if (result.proven) {
        if (!game.is_game_over())
          expand_frontier_node(game, depth_, edge_limit, result.edges);
        result.complete = true;
        result.reason = "all_reveals_verified";
      } else {
        result.reason = "candidate_mate_not_verified";
      }
    } catch (const SearchLimitExceeded &exc) {
      result.edges.clear();
      result.reason = "frontier_not_materialized";
      result.unknown_reason = exc.what();
    } catch (const ProofDagBuildAborted &exc) {
      result.edges.clear();
      result.reason = "frontier_not_materialized";
      result.unknown_reason = exc.what();
    }
    verify_reveal_state(game);

    stats_.elapsed_ms = limits_.elapsed_ms();
    result.memoized_states = memo_.size();
    result.stats = stats_;
    return result;
  }

  RevealVerifiedFrontierResult split_root(const Game &input,
                                          size_t edge_limit) {
    Game game = begin_search(input);

    RevealVerifiedFrontierResult result;
    result.attacker = attacker_;
    result.depth = depth_;
    result.player = game.current_player();
    result.winner = game.winner();
    result.waiting_noble = game.board.waiting_noble;
    result.kind = game.is_game_over()             ? "terminal"
                  : can_resolve_final_round(game) ? "final_round_summary"
                                                  : "state";
    if (game.is_game_over()) {
      result.complete = true;
      result.resolution =
          game.winner() == attacker_ ? "attacker_win" : "non_attacker_win";
      result.reason = "terminal_root";
    } else {
      try {
        std::vector<OrderedAction> actions = proof_ordered_actions(game);
        if (game.current_player() == attacker_)
          actions = forced_attacker_actions(game, actions, depth_, true);
        stats_.legal_moves += actions.size();

        const int current_player = game.current_player();
        const bool waiting_noble_action = game.board.waiting_noble;
        const bool preserve_child_depth = can_resolve_final_round(game);
        for (const OrderedAction &ordered : actions) {
          const bool completed =
              for_each_proof_outcome(game, ordered, [&](int reveal_card) {
                check_limits();
                if (edge_limit && result.edges.size() >= edge_limit)
                  throw ProofDagBuildAborted("root split edge limit exceeded");
                const int next_depth =
                    waiting_noble_action || preserve_child_depth
                        ? depth_
                        : depth_ - static_cast<int>(
                                       current_player == attacker_ &&
                                       game.current_player() != current_player);
                result.edges.push_back(RevealVerifiedFrontierEdge{
                    ordered.code, reveal_card, next_depth, game.clone_light()});
                return true;
              });
          if (!completed)
            throw ProofDagBuildAborted(
                "root split could not materialize a legal action");
        }
        result.complete = true;
        result.reason = result.edges.empty() ? "root_has_no_legal_edges"
                                             : "root_split_materialized";
      } catch (const SearchLimitExceeded &exc) {
        result.edges.clear();
        result.reason = "root_split_not_materialized";
        result.unknown_reason = exc.what();
      } catch (const ProofDagBuildAborted &exc) {
        result.edges.clear();
        result.reason = "root_split_not_materialized";
        result.unknown_reason = exc.what();
      }
    }
    verify_reveal_state(game);

    stats_.elapsed_ms = limits_.elapsed_ms();
    result.memoized_states = 0;
    result.stats = stats_;
    return result;
  }

private:
  Game begin_search(const Game &input, bool clear_memo = true) {
    if (clear_memo) {
      memo_.clear();
      exact_memo_.clear();
    }
    stats_ = RevealVerifiedSearchStats();
    limits_.reset();
    reserve_active_memo();
    ++search_generation_;
    if (search_generation_ == 0) {
      // Generation zero is reserved for aggregate-initialized entries.
      memo_.clear();
      exact_memo_.clear();
      search_generation_ = 1;
    }

    Game game = input.clone_light();
    // Keep refill timing and level as blank slots. The defender may use any
    // initially hidden card of a blank level, which dominates every concrete
    // reveal sequence without enumerating deck permutations.
    game.blank_refill_mode = true;
    hidden_catalog_.remember_initial_position(game);
    reveal_state_.initialize(game, hidden_catalog_);
    // initialize() materializes the exact hash used by the sidecar. Preserve
    // that root so replay never pairs the cached sidecar with a stale cache.
    root_ = game.clone_light();
    root_reveal_state_ = reveal_state_;
    return game;
  }

  class ScopedBranchRollback {
  public:
    ScopedBranchRollback(Impl &owner, Game &game)
        : owner_(owner), game_(game), previous_board_(game.board),
          previous_state_(owner.reveal_state_),
          previous_blank_refill_(game.blank_refill_mode) {
      CSPLENDOR_PERF_INC(BoardSnapshotCopies);
    }

    ScopedBranchRollback(const ScopedBranchRollback &) = delete;
    ScopedBranchRollback &operator=(const ScopedBranchRollback &) = delete;

    ~ScopedBranchRollback() { restore(); }

    void mark_mutated() noexcept { mutated_ = true; }

    void restore() noexcept {
      if (!mutated_)
        return;
      game_.board = previous_board_;
      game_.blank_refill_mode = previous_blank_refill_;
      owner_.reveal_state_ = previous_state_;
      mutated_ = false;
      CSPLENDOR_PERF_INC(BoardRestores);
      CSPLENDOR_PERF_INC(SolverBoardRollbacks);
    }

  private:
    Impl &owner_;
    Game &game_;
    Board previous_board_;
    RevealSearchState previous_state_;
    bool previous_blank_refill_ = false;
    bool mutated_ = false;
  };

  class ScopedRevealStateRestore {
  public:
    explicit ScopedRevealStateRestore(Impl &owner)
        : owner_(owner), previous_(owner.reveal_state_) {}
    ScopedRevealStateRestore(const ScopedRevealStateRestore &) = delete;
    ScopedRevealStateRestore &
    operator=(const ScopedRevealStateRestore &) = delete;
    ~ScopedRevealStateRestore() { owner_.reveal_state_ = previous_; }

  private:
    Impl &owner_;
    RevealSearchState previous_;
  };

  template <typename Apply> bool apply_tracked(Game &game, Apply apply) {
    const RevealSearchState::TransitionObservation before =
        reveal_state_.observe_before(game.board);
    if (!apply())
      return false;
    reveal_state_.observe_after(before, game.board, hidden_catalog_);
    return true;
  }

  // Count visits even on early visitor cutoff or exception. The generated
  // list alone is NOT evidence that purchase-common work can be amortized.
  // The entire scope vanishes when instrumentation is disabled.
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
  class RefillVisitCounter {
  public:
    RefillVisitCounter(const Action &action, size_t generated) noexcept {
      purchase_ = action.type == PURCHASE;
      if (purchase_) {
        CSPLENDOR_PERF_INC(SolverVisiblePurchaseRefills);
        CSPLENDOR_PERF_ADD(SolverVisiblePurchaseGenerated, generated);
      }
    }
    void visit() noexcept {
      if (purchase_) {
        ++visited_;
        CSPLENDOR_PERF_INC(SolverVisiblePurchaseVisited);
      }
    }
    ~RefillVisitCounter() {
      if (purchase_) {
        if (visited_ == 0) CSPLENDOR_PERF_INC(SolverVisiblePurchaseVisited0);
        else if (visited_ == 1) CSPLENDOR_PERF_INC(SolverVisiblePurchaseVisited1);
        else if (visited_ <= 4) CSPLENDOR_PERF_INC(SolverVisiblePurchaseVisited2To4);
        else CSPLENDOR_PERF_INC(SolverVisiblePurchaseVisited5Plus);
      }
    }
  private:
    size_t visited_ = 0;
    bool purchase_ = false;
  };
#endif

  bool apply_action_tracked(Game &game, const Action &action) {
    return apply_tracked(game,
                         [&] { return game.apply_trusted(action, false); });
  }

  bool apply_action_code_tracked(Game &game, uint64_t code) {
    return apply_tracked(
        game, [&] { return game.apply_action_code_trusted(code, false); });
  }

  void verify_reveal_state(const Game &game) const noexcept {
#if defined(CSPLENDOR_REUSE_SEARCH_SCRATCH) && defined(CSPLENDOR_PERF_INSTRUMENTATION)
    // Diagnostic-only retained payload, including pointer slots but excluding
    // allocator bookkeeping. No memory is reserved from the mate depth limit.
    assert(scratch_.active() == 0);
    size_t payload = scratch_.frames().size() * sizeof(SearchScratchFrame) +
                     scratch_.frames().capacity() * sizeof(void *);
    for (const auto &frame : scratch_.frames()) {
      payload += frame->actions.capacity() * sizeof(OrderedAction) +
                 frame->reveal_cards.capacity() * sizeof(int);
      CSPLENDOR_PERF_MAX(SolverScratchActionCapacityMax, frame->actions.capacity());
      CSPLENDOR_PERF_MAX(SolverScratchRevealCapacityMax, frame->reveal_cards.capacity());
    }
    CSPLENDOR_PERF_MAX(SolverScratchFrameCountMax, scratch_.frames().size());
    CSPLENDOR_PERF_MAX(SolverScratchPayloadBytesMax, payload);
#endif
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
    reveal_state_.verify_or_abort(game.board, hidden_catalog_);
#else
    (void)game;
#endif
  }

  template <typename MemoType>
  static void reserve_memo(MemoType &memo, size_t target) {
    const double capacity =
        static_cast<double>(memo.bucket_count()) * memo.max_load_factor();
    if (static_cast<double>(target) > capacity)
      memo.reserve(target);
  }

  void reserve_active_memo() {
    // Deep exact searches retain roughly one transposition for every several
    // visited nodes.  Reserving a conservative fraction avoids repeated
    // bucket-table growth without making the node limit a memory commitment.
    if (max_nodes_ < 4096)
      return;
    constexpr uint64_t MAX_RESERVED_STATES = 2'000'000;
    const uint64_t reserve_limit =
        max_cache_states_
            ? std::min<uint64_t>(MAX_RESERVED_STATES, max_cache_states_)
            : MAX_RESERVED_STATES;
    const size_t target =
        static_cast<size_t>(std::min<uint64_t>(reserve_limit, max_nodes_ / 12));
    if (exact_reveal_search_)
      reserve_memo(exact_memo_, target);
    else
      reserve_memo(memo_, target);
  }

  using StateKey = csplendor::solver_internal::RevealStateKey;
  using StateKeyHash = csplendor::solver_internal::RevealStateKeyHash;
  using DepthStateKey = csplendor::solver_internal::RevealDepthStateKey;
  using DepthStateKeyHash = csplendor::solver_internal::RevealDepthStateKeyHash;
  using ExactDepthStateKey =
      csplendor::solver_internal::RevealExactDepthStateKey;
  using ExactDepthStateKeyHash =
      csplendor::solver_internal::RevealExactDepthStateKeyHash;

  using DepthPath =
      csplendor::solver_internal::RecursionPath<DepthStateKey,
                                                DepthStateKeyHash>;

  using Entry = csplendor::solver_internal::RevealMemoEntry;
  using PersistentEntry = csplendor::solver_internal::RevealPersistentEntry;

  struct OrderedAction : ActionOrderKey, OracleActionMetadata {
    OrderedAction() = default;
    OrderedAction(int rank_value, int neg_points_value, uint64_t code_value) {
      rank = rank_value;
      neg_points = neg_points_value;
      code = code_value;
    }

    bool operator<(const OrderedAction &other) const {
      const ActionOrderKey &base = *this;
      const ActionOrderKey &other_base = other;
      if (base < other_base)
        return true;
      if (other_base < base)
        return false;
      return less_than(other);
    }
  };

  struct SearchScratchFrame {
    std::vector<OrderedAction> actions;
    std::vector<int> reveal_cards;
    void clear() noexcept {
      actions.clear();
      reveal_cards.clear();
    }
  };
#ifdef CSPLENDOR_REUSE_SEARCH_SCRATCH
  using SearchScratch =
      csplendor::solver_internal::RecursionScratch<SearchScratchFrame>;
  SearchScratch scratch_;
#endif

  int attacker_ = 0;
  int depth_ = 0;
  uint64_t max_nodes_ = 0;
  size_t max_cache_states_ = 0;
  SearchLimit limits_;
  RevealVerifiedSearchStats stats_;
  using Memo = std::unordered_map<DepthStateKey, Entry, DepthStateKeyHash>;
  using ExactMemo = std::unordered_map<ExactDepthStateKey, PersistentEntry,
                                       ExactDepthStateKeyHash>;
  Memo memo_;
  ExactMemo exact_memo_;
  uint64_t search_generation_ = 0;
  uint64_t cache_touch_counter_ = 0;
  Game root_{0};
  HiddenOutcomeCatalog hidden_catalog_;
  RevealSearchState reveal_state_;
  RevealSearchState root_reveal_state_;
  std::vector<uint64_t> preferred_attacker_actions_;
  bool include_proof_dag_ = false;
  RevealProofDagBuilder proof_builder_;
  uint64_t required_root_action_ = UINT64_MAX;
  bool strict_preferred_attacker_actions_ = false;
  size_t strict_preferred_attacker_prefix_ = 0;
  bool exhaustive_attacker_actions_ = false;
  bool exact_reveal_search_ = false;
  std::unordered_map<DepthStateKey, size_t, DepthStateKeyHash> proof_node_ids_;
  std::unordered_map<DepthStateKey, size_t, DepthStateKeyHash>
      proof_terminal_node_ids_;
  static constexpr size_t ATTACKER_TAKE_LIMIT = 6;
  static constexpr size_t ATTACKER_RESERVE_LIMIT = 3;

  static size_t path_reserve_capacity(int remaining_depth) noexcept {
    const size_t depth = static_cast<size_t>(std::max(remaining_depth, 1));
    return depth * 4 + 4;
  }

  ForceStatus forced_win(Game &game, DepthPath &path, int depth,
                         bool exact_refinement) {
    if (exact_refinement)
      return forced_win_with_memo(game, path, depth, true, exact_memo_);
    return forced_win_with_memo(game, path, depth, false, memo_);
  }

  template <typename MemoType>
  ForceStatus forced_win_with_memo(Game &game, DepthPath &path, int depth,
                                   bool exact_refinement, MemoType &memo) {
    check_limits();
    ++stats_.nodes;

    const TerminalResult terminal{game.is_game_over(), game.winner(),
                                  "game_over"};
    if (terminal.terminal) {
      ++stats_.terminal_nodes;
      return terminal.winner == attacker_ ? ForceStatus::PROVEN
                                          : ForceStatus::REFUTED;
    }
    if (game.current_player() == attacker_ && depth <= 0)
      return ForceStatus::REFUTED;

    const StateKey state = state_key(game, exact_refinement);
    const DepthStateKey path_key{state, depth};
    using MemoKey = typename MemoType::key_type;
    using MemoEntry = typename MemoType::mapped_type;
    const MemoKey key{state, depth};
    CSPLENDOR_PERF_TT_PROBE_SCOPE(tt_probe);
    auto memo_it = memo.find(key);
    CSPLENDOR_PERF_TT_PROBE_FINISH(tt_probe);
    if (memo_it != memo.end()) {
      CSPLENDOR_PERF_INC(SolverTtHits);
      ++stats_.memo_hits;
      if constexpr (MemoEntry::tracks_persistence) {
        if (memo_it->second.generation() != search_generation_)
          ++stats_.persistent_memo_hits;
        memo_it->second.set_generation(search_generation_);
        memo_it->second.set_last_touched(++cache_touch_counter_);
      }
      return memo_it->second.status();
    }
    if (path.contains(path_key)) {
      ++stats_.terminal_nodes;
      return ForceStatus::UNKNOWN;
    }
    if (can_resolve_final_round(game)) {
      const MemoEntry resolved =
          resolve_final_round<MemoEntry>(game, exact_refinement);
      store_entry(memo, key, resolved);
      return resolved.status();
    }

#ifdef CSPLENDOR_REUSE_SEARCH_SCRATCH
    SearchScratch::Lease scratch_lease(scratch_);
    auto &actions = scratch_lease.get().actions;
    fill_ordered_actions(game, actions, exact_refinement);
#else
    std::vector<OrderedAction> actions =
        exact_refinement ? proof_ordered_actions(game) : ordered_actions(game);
#endif
    CSPLENDOR_PERF_INC(SolverTemporaryVectorAllocations);
    if (exact_refinement)
      prefer_iterative_action(state, depth, game.current_player(), actions);
    if (game.current_player() == attacker_) {
#ifdef CSPLENDOR_REUSE_SEARCH_SCRATCH
      // The exhaustive path only reordered a copy. Keep this frame's capacity;
      // constrained/filtering paths retain the existing selection algorithm.
      if (exhaustive_attacker_actions_ && !strict_preferred_attacker_actions_ &&
          strict_preferred_attacker_prefix_ == 0 &&
          !(path.empty() && required_root_action_ != UINT64_MAX))
        prefer_candidate_action(actions, depth);
      else
#endif
        actions = forced_attacker_actions(game, actions, depth, path.empty());
    }
    stats_.legal_moves += actions.size();
    if (actions.empty()) {
      ++stats_.terminal_nodes;
      store_entry(memo, key, MemoEntry{ForceStatus::REFUTED, 0, -1, false, 0});
      return ForceStatus::REFUTED;
    }

    const int current_player = game.current_player();
    ScopedPathEntry<DepthPath> path_entry(path, path_key);
    bool has_unknown = false;
    MemoEntry representative;
    representative.set_action_count(actions.size());

    for (const OrderedAction &ordered : actions) {
      bool action_unknown = false;
      bool action_refuted = false;
      int representative_reveal = -1;
      bool has_representative_reveal = false;
      const auto visit_outcome = [&](int reveal_card) {
        const int next_depth =
            depth - static_cast<int>(current_player == attacker_ &&
                                     game.current_player() != current_player);
        const ForceStatus child = forced_win_with_memo(game, path, next_depth,
                                                       exact_refinement, memo);
        if (!has_representative_reveal) {
          representative_reveal = reveal_card;
          has_representative_reveal = true;
        }
        if (child == ForceStatus::REFUTED) {
          action_refuted = true;
          return false;
        }
        action_unknown = action_unknown || child == ForceStatus::UNKNOWN;
        return true;
      };
      const bool completed =
          exact_refinement
              ? for_each_proof_outcome(game, ordered, visit_outcome)
              : for_each_search_outcome(game, ordered, visit_outcome);
      if (!completed && !action_refuted)
        action_unknown = true;

      if (!representative.has_action()) {
        representative =
            MemoEntry{ForceStatus::UNKNOWN,  ordered.code,
                      representative_reveal, true,
                      actions.size(),        is_replayable(ordered)};
      }
      if (current_player == attacker_ && !action_refuted && !action_unknown) {
        store_entry(memo, key,
                    MemoEntry{ForceStatus::PROVEN, ordered.code,
                              representative_reveal, true, actions.size(),
                              is_replayable(ordered)});
        return ForceStatus::PROVEN;
      }
      if (current_player != attacker_ && action_refuted) {
        store_entry(memo, key,
                    MemoEntry{ForceStatus::REFUTED, ordered.code,
                              representative_reveal, true, actions.size(),
                              is_replayable(ordered)});
        return ForceStatus::REFUTED;
      }
      has_unknown = has_unknown || action_unknown;
    }

    if (has_unknown)
      return ForceStatus::UNKNOWN;
    const ForceStatus status = current_player == attacker_
                                   ? ForceStatus::REFUTED
                                   : ForceStatus::PROVEN;
    representative.set_status(status);
    store_entry(memo, key, representative);
    return status;
  }

  template <typename MemoType>
  void store_entry(MemoType &memo, const typename MemoType::key_type &key,
                   typename MemoType::mapped_type entry) {
    CSPLENDOR_PERF_INC(SolverTtStores);
    using MemoEntry = typename MemoType::mapped_type;
    if constexpr (MemoEntry::tracks_persistence) {
      entry.set_generation(search_generation_);
      entry.set_last_touched(++cache_touch_counter_);
    }
    memo[key] = std::move(entry);
  }

  void prefer_iterative_action(const StateKey &state, int depth,
                               int current_player,
                               std::vector<OrderedAction> &actions) {
    if (actions.size() < 2 || depth <= 0)
      return;
    for (int cached_depth = depth - 1; cached_depth >= 0; --cached_depth) {
      CSPLENDOR_PERF_TT_PROBE_SCOPE(iterative_memo_probe);
      const auto cached =
          exact_memo_.find(ExactDepthStateKey{state, cached_depth});
      CSPLENDOR_PERF_TT_PROBE_FINISH(iterative_memo_probe);
      if (cached != exact_memo_.end())
        CSPLENDOR_PERF_INC(SolverTtHits);
      if (cached == exact_memo_.end() || !cached->second.has_action())
        continue;
      const bool useful = current_player == attacker_
                              ? cached->second.status() == ForceStatus::PROVEN
                              : cached->second.status() == ForceStatus::REFUTED;
      if (!useful)
        continue;
      const auto action = std::find_if(
          actions.begin(), actions.end(), [&](const OrderedAction &candidate) {
            return candidate.code == cached->second.action_code();
          });
      if (action == actions.end())
        continue;
      std::rotate(actions.begin(), action, action + 1);
      ++stats_.iterative_order_hits;
      return;
    }
  }

  template <typename Visitor>
  bool for_each_search_outcome(Game &game, const OrderedAction &ordered,
                               Visitor visitor) {
    if (ordered.oracle_card >= 0 || ordered.oracle_reserve) {
      ScopedBranchRollback rollback(*this, game);
      rollback.mark_mutated();
      if (!apply_tracked(game,
                         [&] { return apply_oracle_action(game, ordered); }))
        return false;
      return visitor(-1);
    }

    const Action action = Action::unpack(ordered.code);
    if (action.type == RESERVE_DECK)
      return for_each_deck_reserve_outcome(game, action, visitor);

    ScopedBranchRollback rollback(*this, game);
    rollback.mark_mutated();
    if (!apply_action_tracked(game, action))
      return false;
    return visitor(-1);
  }

  template <typename Visitor>
  bool for_each_proof_outcome(Game &game, const OrderedAction &ordered,
                              Visitor visitor) {
    const Action action = Action::unpack(ordered.code);
    if (action.type == RESERVE_DECK)
      return for_each_deck_reserve_outcome(game, action, visitor);

    const int level = visible_refill_level(action);
    if (level >= 0 && !game.board.decks[level].empty())
      return for_each_visible_refill_outcome(game, action, visitor);

    ScopedBranchRollback rollback(*this, game);
    rollback.mark_mutated();
    if (!apply_action_tracked(game, action))
      return false;
    return visitor(-1);
  }

  static bool is_replayable(const OrderedAction &ordered) {
    return ordered.oracle_card < 0 && !ordered.oracle_reserve;
  }

  template <typename Visitor>
  bool for_each_visible_refill_outcome(Game &game, const Action &action,
                                       Visitor visitor) {
    const int level = visible_refill_level(action);
    const int slot = visible_refill_slot(game.board, action);
    if (level < 0 || level >= 3 || slot < 0 || game.board.decks[level].empty())
      return false;
#ifdef CSPLENDOR_REUSE_SEARCH_SCRATCH
    SearchScratch::Lease scratch_lease(scratch_);
    auto &cards = scratch_lease.get().reveal_cards;
#else
    std::vector<int> cards;
#endif
    visible_refill_cards(game, action, level, slot, cards);
    if (cards.empty())
      return false;
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    RefillVisitCounter visits(action, cards.size());
#endif
    ScopedBranchRollback rollback(*this, game);
    for (int card_id : cards) {
      rollback.restore();
      rollback.mark_mutated();
      if (!apply_visible_refill_outcome(game, action, level, slot, card_id))
        continue;
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
      visits.visit();
#endif
      if (!visitor(card_id))
        return false;
    }
    return true;
  }

  template <typename Visitor>
  bool for_each_deck_reserve_outcome(Game &game, const Action &action,
                                     Visitor visitor) {
#ifdef CSPLENDOR_REUSE_SEARCH_SCRATCH
    SearchScratch::Lease scratch_lease(scratch_);
    auto &cards = scratch_lease.get().reveal_cards;
#else
    std::vector<int> cards;
#endif
    deck_reserve_cards(game, action, cards);
    stats_.deck_reserve_candidates += cards.size();
    if (cards.empty())
      return false;
    ScopedBranchRollback rollback(*this, game);
    for (int card_id : cards) {
      rollback.restore();
      rollback.mark_mutated();
      if (!apply_deck_reserve_outcome(game, action, card_id))
        continue;
      ++stats_.deck_reserve_branches;
      if (!visitor(card_id))
        return false;
    }
    return true;
  }

  struct CardEquivalenceKey {
    int level = 0;
    int points = 0;
    int bonus = 0;
    std::array<uint8_t, 5> cost = {0};

    bool operator<(const CardEquivalenceKey &other) const {
      if (level != other.level)
        return level < other.level;
      if (points != other.points)
        return points < other.points;
      if (bonus != other.bonus)
        return bonus < other.bonus;
      return cost < other.cost;
    }
  };

  static CardEquivalenceKey card_equivalence_key(int card_id) {
    const Card &card = get_card(card_id);
    return CardEquivalenceKey{card.level, card.points, card.bonus, card.cost};
  }

  static bool
  remember_card_equivalence(std::set<CardEquivalenceKey> &legacy_seen,
                            CardEquivalenceMask &class_seen, int card_id) {
    CSPLENDOR_PERF_INC(SolverCardEquivalenceLookups);
    if (csplendor::solver_internal::card_equivalence_classes_enabled) {
      return class_seen.insert(
          csplendor::solver_internal::card_equivalence_class(card_id));
    }
    return legacy_seen.insert(card_equivalence_key(card_id)).second;
  }

  void deck_reserve_cards(const Game &game, const Action &action,
                          std::vector<int> &cards) const {
    const int level = action.deck_level;
    cards.clear();
    if (level < 0 || level >= 3 || game.board.decks[level].empty())
      return;
    std::set<CardEquivalenceKey> legacy_seen;
    CardEquivalenceMask class_seen;
    if (!csplendor::solver_internal::card_equivalence_classes_enabled)
      CSPLENDOR_PERF_INC(SolverTemporarySetAllocations);
    for (uint8_t card_id : game.board.decks[level]) {
      if (!reveal_state_.is_claimed(game.board, card_id) &&
          remember_card_equivalence(legacy_seen, class_seen, card_id))
        cards.push_back(static_cast<int>(card_id));
    }
    if (game.current_player() != attacker_ && cards.size() > 1) {
      CSPLENDOR_PERF_ADD(SolverDefenderReserveSortCandidates, cards.size());
      csplendor::solver_internal::sort_reveal_cards_by_score(
          cards, [&](int card_id) {
            return defender_reserved_card_threat_score(game, action, card_id);
          });
    }
    CSPLENDOR_PERF_ADD(SolverRevealCandidates, cards.size());
    record_reveal_scratch_size(cards.size());
  }

  static void record_reveal_scratch_size(size_t count) {
    if (count == 0)
      CSPLENDOR_PERF_INC(SolverScratchReveals0);
    else if (count <= 16)
      CSPLENDOR_PERF_INC(SolverScratchReveals1To16);
    else
      CSPLENDOR_PERF_INC(SolverScratchReveals17Plus);
  }

  static int defender_reserved_card_threat_score(const Game &game,
                                                 const Action &action,
                                                 int card_id) {
    CSPLENDOR_PERF_INC(SolverDefenderReserveScoreCalls);
    if (!is_valid_card_id(card_id))
      return 0;
    const Board &board = game.board;
    const PlayerState &player = board.players[board.current_player];
    std::array<int, 6> gems = {0};
    for (int color = 0; color < 6; ++color)
      gems[color] = static_cast<int>(player.gems[color]);
    if (board.bank[GOLD] > 0)
      ++gems[GOLD];
    for (int color = 0; color < 6; ++color)
      gems[color] -= static_cast<int>(action.return_gems[color]);

    const Card &card = get_card(card_id);
    int shortage = 0;
    for (int color = 0; color < 5; ++color) {
      const int need = std::max(0, static_cast<int>(card.cost[color]) -
                                       static_cast<int>(player.bonuses[color]));
      shortage += std::max(0, need - gems[color]);
    }
    const int gap = std::max(0, shortage - gems[GOLD]);
    std::array<uint8_t, 5> bonuses = player.bonuses;
    ++bonuses[card.bonus];
    const int noble_points = max_noble_points(board, bonuses);
    const int immediate_win_bonus = static_cast<int>(player.points) +
                                                static_cast<int>(card.points) +
                                                noble_points >=
                                            15
                                        ? 1000000
                                        : 0;
    return immediate_win_bonus + (gap == 0 ? 100000 : 0) +
           static_cast<int>(card.points) * 10000 + noble_points * 1000 -
           gap * 100 + static_cast<int>(card.bonus);
  }

  void visible_refill_cards(const Game &game, const Action &action,
                             int level, int slot, std::vector<int> &cards) const {
    cards.clear();
    if (level < 0 || level >= 3 || game.board.decks[level].empty())
      return;
    std::set<CardEquivalenceKey> legacy_seen;
    CardEquivalenceMask class_seen;
    if (!csplendor::solver_internal::card_equivalence_classes_enabled)
      CSPLENDOR_PERF_INC(SolverTemporarySetAllocations);
    for (uint8_t card_id : game.board.decks[level]) {
      if (!reveal_state_.is_claimed(game.board, card_id) &&
          remember_card_equivalence(legacy_seen, class_seen, card_id))
        cards.push_back(static_cast<int>(card_id));
    }
    CSPLENDOR_PERF_ADD(SolverRevealCandidates, cards.size());
    order_visible_refill_cards_by_blank_probe(game, action, level, slot, cards);
    record_reveal_scratch_size(cards.size());
  }

  void order_visible_refill_cards_by_blank_probe(
      const Game &game, const Action &action, int level, int slot,
      std::vector<int> &cards) const {
    if (cards.size() <= 1)
      return;
    Game blank = game.clone_light();
    if (!apply_visible_refill_blank_outcome(blank, action, level, slot))
      return;
    CSPLENDOR_PERF_ADD(SolverVisibleRefillSortCandidates, cards.size());
    csplendor::solver_internal::sort_reveal_cards_by_score(
        cards, [&](int card_id) {
          return reveal_counterexample_score(blank, level, card_id);
        });
  }

  static bool apply_visible_refill_blank_outcome(Game &game,
                                                 const Action &action,
                                                 int level, int slot) {
    if (level < 0 || level >= 3 || slot < 0 || slot >= Board::CARDS_PER_LEVEL ||
        game.board.decks[level].empty())
      return false;
    const bool previous_blank_refill = game.blank_refill_mode;
    game.blank_refill_mode = true;
    const bool applied = game.apply_trusted(action, false);
    game.blank_refill_mode = previous_blank_refill;
    return applied && game.board.visible[level][slot] == -1;
  }

  static int reveal_counterexample_score(const Game &blank, int level,
                                         int card_id) {
    CSPLENDOR_PERF_INC(SolverVisibleRefillScoreCalls);
    if (!is_valid_card_id(card_id))
      return 0;
    const Board &board = blank.board;
    if (board.is_game_over() || board.waiting_noble ||
        board.current_player >= Board::NUM_PLAYERS)
      return 0;
    const PlayerState &player = board.players[board.current_player];
    const Card &card = get_card(card_id);
    int score = static_cast<int>(card.points) * 10000 +
                static_cast<int>(card.bonus) * 100;
    if (player.can_afford(card)) {
      std::array<uint8_t, 5> bonuses = player.bonuses;
      ++bonuses[card.bonus];
      const int noble_points = max_noble_points(board, bonuses);
      score += 100000 + noble_points * 1000;
      if (static_cast<int>(player.points) + static_cast<int>(card.points) +
              noble_points >=
          15)
        score += 1000000;
    }
    if (player.can_reserve())
      score += level * 1000 + static_cast<int>(card.points) * 500;
    return score;
  }

  static int visible_refill_slot(const Board &board, const Action &action) {
    if (action.type != RESERVE_VISIBLE &&
        !(action.type == PURCHASE && !action.from_reserved))
      return -1;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
        if (board.visible[level][slot] == action.card_id)
          return slot;
      }
    }
    return -1;
  }

  static bool remove_card_from_deck_legacy(Board &board, int level,
                                           int card_id) {
    if (level < 0 || level >= 3)
      return false;
    auto &deck = board.decks[level];
    const auto it =
        std::find(deck.begin(), deck.end(), static_cast<uint8_t>(card_id));
    if (it == deck.end())
      return false;
    // Exact-reveal traversal can erase an arbitrary stack position. Every
    // following positional salt changes, so this path deliberately falls
    // back to the uncached oracle instead of using the normal top-pop delta.
    board.invalidate_hash();
    deck.erase(it);
    return true;
  }

  bool prepare_reveal_card(Game &game, int level, int card_id) {
    if (reveal_state_.active()) {
      if (reveal_state_.move_deck_card_to_back(game.board, level, card_id))
        return true;
      if (reveal_state_.active())
        return false;
    }
    if (!remove_card_from_deck_legacy(game.board, level, card_id))
      return false;
    return game.board.decks[level].try_push_back(static_cast<uint8_t>(card_id));
  }

  bool apply_visible_refill_outcome(Game &game, const Action &action, int level,
                                  int slot, int card_id) {
#ifdef CSPLENDOR_PERF_INSTRUMENTATION
    if (action.type == PURCHASE)
      CSPLENDOR_PERF_INC(SolverVisiblePurchaseApplyCalls);
#endif
    if (!is_valid_card_id(card_id) || get_card(card_id).level - 1 != level ||
        slot < 0 || slot >= Board::CARDS_PER_LEVEL ||
        !prepare_reveal_card(game, level, card_id))
      return false;
    const bool previous_blank_refill = game.blank_refill_mode;
    game.blank_refill_mode = false;
    const bool applied = apply_action_tracked(game, action);
    game.blank_refill_mode = previous_blank_refill;
    if (!applied)
      return false;
    if (game.board.visible[level][slot] != card_id)
      return false;
    return true;
  }

  bool apply_deck_reserve_outcome(Game &game, const Action &action,
                                  int card_id) {
    Board &board = game.board;
    if (board.is_game_over() || board.waiting_noble ||
        board.current_player >= Board::NUM_PLAYERS ||
        action.type != RESERVE_DECK || action.deck_level < 0 ||
        action.deck_level >= 3 || board.decks[action.deck_level].empty())
      return false;
    const int reserving_player = board.current_player;
    PlayerState &player = board.players[reserving_player];
    if (!player.can_reserve() || !is_valid_card_id(card_id) ||
        get_card(card_id).level - 1 != action.deck_level)
      return false;

    if (!prepare_reveal_card(game, action.deck_level, card_id))
      return false;
    if (!apply_action_tracked(game, action))
      return false;
    const PlayerState &reserved_by = board.players[reserving_player];
    return std::find(reserved_by.reserved.begin(), reserved_by.reserved.end(),
                     card_id) != reserved_by.reserved.end();
  }

  static int visible_refill_level(const Action &action) {
    if (action.type == RESERVE_VISIBLE)
      return get_card(action.card_id).level - 1;
    if (action.type == PURCHASE && !action.from_reserved)
      return get_card(action.card_id).level - 1;
    return -1;
  }

  static bool can_resolve_final_round(const Game &game) {
    return game.board.final_round && !game.board.waiting_noble &&
           game.current_player() == 1;
  }

  template <typename EntryType>
  EntryType resolve_final_round(Game &game, bool exact_refinement) {
    ForceStatus direct_status = ForceStatus::UNKNOWN;
    if (resolve_final_round_direct(game, direct_status, exact_refinement)) {
      ++stats_.final_round_direct_resolutions;
      if (direct_status == ForceStatus::PROVEN)
        ++stats_.final_round_score_prunes;
      return EntryType{direct_status, 0, -1, false, 0};
    }
    if (max_final_round_score(game) < game.board.players[0].points) {
      ++stats_.final_round_score_prunes;
      return EntryType{attacker_ == 0 ? ForceStatus::PROVEN
                                      : ForceStatus::REFUTED,
                       0, -1, false, 0};
    }

    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions =
        exact_refinement ? proof_ordered_actions(game) : ordered_actions(game);
    stats_.legal_moves += actions.size();
    if (actions.empty())
      return EntryType{ForceStatus::REFUTED, 0, -1, false, 0};

    bool has_unknown = false;
    EntryType representative;
    representative.set_action_count(actions.size());
    for (const OrderedAction &ordered : actions) {
      ScopedBranchRollback rollback(*this, game);
      rollback.mark_mutated();
      const Action action = Action::unpack(ordered.code);
      const int reveal_card = final_round_representative_reveal(game, action);
      if (!apply_action_tracked(game, action))
        continue;
      const ForceStatus child = game.board.waiting_noble
                                    ? resolve_final_round_noble(game)
                                    : terminal_status(game);

      if (!representative.has_action()) {
        representative =
            EntryType{child, ordered.code,   reveal_card,
                      true,  actions.size(), is_replayable(ordered)};
      }
      if (current_player == attacker_ && child == ForceStatus::PROVEN)
        return EntryType{ForceStatus::PROVEN, ordered.code,
                         reveal_card,         true,
                         actions.size(),      is_replayable(ordered)};
      if (current_player != attacker_ && child == ForceStatus::REFUTED)
        return EntryType{ForceStatus::REFUTED, ordered.code,
                         reveal_card,          true,
                         actions.size(),       is_replayable(ordered)};
      has_unknown = has_unknown || child == ForceStatus::UNKNOWN;
    }

    representative.set_status(has_unknown ? ForceStatus::UNKNOWN
                              : current_player == attacker_
                                  ? ForceStatus::REFUTED
                                  : ForceStatus::PROVEN);
    return representative;
  }

  ForceStatus resolve_final_round_noble(Game &game) {
    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions = proof_ordered_actions(game);
    stats_.legal_moves += actions.size();
    bool has_unknown = false;
    for (const OrderedAction &ordered : actions) {
      ScopedBranchRollback rollback(*this, game);
      rollback.mark_mutated();
      if (!apply_action_code_tracked(game, ordered.code))
        continue;
      const ForceStatus child = terminal_status(game);
      if (current_player == attacker_ && child == ForceStatus::PROVEN)
        return ForceStatus::PROVEN;
      if (current_player != attacker_ && child == ForceStatus::REFUTED)
        return ForceStatus::REFUTED;
      has_unknown = has_unknown || child == ForceStatus::UNKNOWN;
    }
    return has_unknown                   ? ForceStatus::UNKNOWN
           : current_player == attacker_ ? ForceStatus::REFUTED
                                         : ForceStatus::PROVEN;
  }

  static int final_round_representative_reveal(const Game &game,
                                               const Action &action) {
    if (action.type == RESERVE_DECK) {
      const int level = action.deck_level;
      if (level >= 0 && level < 3 && !game.board.decks[level].empty())
        return static_cast<int>(game.board.decks[level].back());
    }
    const int level = visible_refill_level(action);
    if (level >= 0 && !game.board.decks[level].empty())
      return static_cast<int>(game.board.decks[level].back());
    return -1;
  }

  ForceStatus terminal_status(const Game &game) {
    if (!game.is_game_over())
      return ForceStatus::UNKNOWN;
    ++stats_.terminal_nodes;
    return game.winner() == attacker_ ? ForceStatus::PROVEN
                                      : ForceStatus::REFUTED;
  }

  static int max_final_round_score(const Game &game) {
    const Board &board = game.board;
    const PlayerState &player = board.players[1];
    int max_score = static_cast<int>(player.points) +
                    max_noble_points(board, player.bonuses);
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        max_score = std::max(
            max_score,
            score_after_purchase(board, player, board.visible[level][slot]));
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      max_score =
          std::max(max_score,
                   score_after_purchase(board, player, player.reserved[slot]));
    return max_score;
  }

  bool resolve_final_round_direct(const Game &game, ForceStatus &status,
                                  bool exact_refinement) const {
    const Board &board = game.board;
    const PlayerState &player = board.players[1];
    const bool allow_oracle = !exact_refinement && attacker_ != 1;
    bool has_action = false;
    bool has_player0_only_outcomes = true;
    bool has_player1_win = false;
    const auto consider = [&](int points, int purchased_count) {
      has_action = true;
      const int winner = final_round_winner(board, points, purchased_count);
      has_player0_only_outcomes = has_player0_only_outcomes && winner == 0;
      has_player1_win = has_player1_win || winner == 1;
    };

    if (has_nonpurchase_action(board, allow_oracle)) {
      consider(static_cast<int>(player.points) +
                   max_noble_points(board, player.bonuses),
               player.purchased_count);
    }
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot)
        consider_purchase(board, player, board.visible[level][slot], consider);
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot)
      consider_purchase(board, player, player.reserved[slot], consider);
    if (allow_oracle) {
      for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
        if (hidden_catalog_.is_initially_hidden(card_id) &&
            !reveal_state_.is_claimed(board, card_id) &&
            has_blank_slot_at_level(board, get_card(card_id).level - 1))
          consider_purchase(board, player, card_id, consider);
      }
    }

    if (!has_action)
      return false;
    status = attacker_ == 0 ? (has_player0_only_outcomes ? ForceStatus::PROVEN
                                                         : ForceStatus::REFUTED)
                            : (has_player1_win ? ForceStatus::PROVEN
                                               : ForceStatus::REFUTED);
    return true;
  }

  template <typename Visitor>
  static void consider_purchase(const Board &board, const PlayerState &player,
                                int card_id, Visitor visitor) {
    if (!is_valid_card_id(card_id))
      return;
    const Card &card = get_card(card_id);
    if (!player.can_afford(card))
      return;
    std::array<uint8_t, 5> bonuses = player.bonuses;
    ++bonuses[card.bonus];
    visitor(static_cast<int>(player.points) + card.points +
                max_noble_points(board, bonuses),
            player.purchased_count + 1);
  }

  static bool has_nonpurchase_action(const Board &board, bool allow_oracle) {
    if (allow_oracle && has_blank_slot(board))
      return true;
    for (int color = 0; color < 5; ++color) {
      if (board.bank[color] > 0)
        return true;
    }
    if (!board.players[1].can_reserve())
      return false;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
        if (is_valid_card_id(board.visible[level][slot]))
          return true;
      }
    }
    return false;
  }

  static bool has_blank_slot(const Board &board) {
    for (int level = 0; level < 3; ++level) {
      if (has_blank_slot_at_level(board, level))
        return true;
    }
    return false;
  }

  static bool has_blank_slot_at_level(const Board &board, int level) {
    for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
      if (!is_valid_card_id(board.visible[level][slot]))
        return true;
    }
    return false;
  }

  static int final_round_winner(const Board &board, int player1_points,
                                int player1_purchased_count) {
    const PlayerState &player0 = board.players[0];
    if (player0.points > player1_points)
      return 0;
    if (player1_points > player0.points)
      return 1;
    if (player0.purchased_count < player1_purchased_count)
      return 0;
    if (player1_purchased_count < player0.purchased_count)
      return 1;
    return -2;
  }

  static int score_after_purchase(const Board &board, const PlayerState &player,
                                  int card_id) {
    if (!is_valid_card_id(card_id))
      return player.points;
    const Card &card = get_card(card_id);
    if (!player.can_afford(card))
      return player.points;
    std::array<uint8_t, 5> bonuses = player.bonuses;
    ++bonuses[card.bonus];
    return static_cast<int>(player.points) + card.points +
           max_noble_points(board, bonuses);
  }

  static int max_noble_points(const Board &board,
                              const std::array<uint8_t, 5> &bonuses) {
    int points = 0;
    for (uint8_t noble_id : board.nobles) {
      const Noble &noble = get_noble(noble_id);
      bool eligible = true;
      for (int color = 0; color < 5; ++color)
        eligible = eligible && bonuses[color] >= noble.requirement[color];
      if (eligible)
        points = std::max(points, static_cast<int>(noble.points));
    }
    return points;
  }

  std::vector<OrderedAction> ordered_actions(const Game &game) {
    std::vector<OrderedAction> actions;
    fill_ordered_actions(game, actions, false);
    return actions;
  }

  void fill_ordered_actions(const Game &game,
                             std::vector<OrderedAction> &actions, bool exact) {
    ordinary_ordered_actions(game, actions);
    if (!exact && game.current_player() != attacker_ && !game.board.waiting_noble)
      add_oracle_actions(game, actions);
    if (!exact)
      std::sort(actions.begin(), actions.end());
  }

  static std::vector<OrderedAction> proof_ordered_actions(const Game &game) {
    return ordinary_ordered_actions(game);
  }

  static std::vector<OrderedAction> ordinary_ordered_actions(const Game &game) {
    std::vector<OrderedAction> actions;
    ordinary_ordered_actions(game, actions);
    return actions;
  }

  static void ordinary_ordered_actions(const Game &game,
                                       std::vector<OrderedAction> &actions) {
    actions.clear();
    for (uint64_t code : game.legal_action_codes()) {
      const Action action = Action::unpack(code);
      actions.push_back(ordered_action(action, code));
    }
    std::sort(actions.begin(), actions.end());
    if (actions.empty())
      CSPLENDOR_PERF_INC(SolverScratchActions0);
    else if (actions.size() <= 16)
      CSPLENDOR_PERF_INC(SolverScratchActions1To16);
    else
      CSPLENDOR_PERF_INC(SolverScratchActions17Plus);
  }

  void add_oracle_actions(const Game &game,
                          std::vector<OrderedAction> &actions) {
    const PlayerState &player = game.board.players[game.current_player()];
    for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
      if (!hidden_catalog_.is_initially_hidden(card_id) ||
          reveal_state_.is_claimed(game.board, card_id) ||
          !has_blank_slot_at_level(game.board, get_card(card_id).level - 1))
        continue;
      const Card &card = get_card(card_id);
      if (!player.can_afford(card))
        continue;
      std::array<int, 5> effective_cost = {0};
      for (int color = 0; color < 5; ++color) {
        effective_cost[color] =
            std::max(0, static_cast<int>(card.cost[color]) -
                            static_cast<int>(player.bonuses[color]));
      }
      add_oracle_purchase_actions(player, card, effective_cost, 0, 0, {},
                                  actions);
    }
    if (player.can_reserve()) {
      for (int card_id = 0; card_id < CARD_COUNT; ++card_id) {
        if (!hidden_catalog_.is_initially_hidden(card_id) ||
            reveal_state_.is_claimed(game.board, card_id) ||
            !has_blank_slot_at_level(game.board, get_card(card_id).level - 1))
          continue;
        const int total_gems = player.total_gems();
        if (game.board.bank[GOLD] == 0 || total_gems < Board::MAX_TOKENS) {
          add_oracle_reserve_action(card_id, -1, actions);
        } else {
          for (int color = 0; color < 6; ++color) {
            if (player.gems[color] > 0 || color == GOLD)
              add_oracle_reserve_action(card_id, color, actions);
          }
        }
      }
    }
  }

  void add_oracle_reserve_action(int card_id, int return_color,
                                 std::vector<OrderedAction> &actions) {
    OrderedAction action;
    action.rank = 1;
    action.oracle_reserve = true;
    action.oracle_reserve_card = card_id;
    action.oracle_return_color = return_color;
    actions.push_back(action);
    ++stats_.oracle_reserve_actions;
  }

  void add_oracle_purchase_actions(const PlayerState &player, const Card &card,
                                   const std::array<int, 5> &effective_cost,
                                   int color, int gold_used,
                                   std::array<uint8_t, 5> gold_as,
                                   std::vector<OrderedAction> &actions) {
    if (color == 5) {
      OrderedAction action;
      action.rank = 0;
      action.neg_points = -static_cast<int>(card.points);
      action.oracle_card = card.id;
      action.oracle_gold_as = gold_as;
      actions.push_back(action);
      ++stats_.oracle_purchase_actions;
      return;
    }
    const int min_gold = std::max(0, effective_cost[color] -
                                         static_cast<int>(player.gems[color]));
    const int max_gold = std::min(
        effective_cost[color], static_cast<int>(player.gems[GOLD]) - gold_used);
    for (int amount = min_gold; amount <= max_gold; ++amount) {
      gold_as[color] = static_cast<uint8_t>(amount);
      add_oracle_purchase_actions(player, card, effective_cost, color + 1,
                                  gold_used + amount, gold_as, actions);
    }
  }

  template <bool MaintainExactHash>
  bool apply_oracle_action_with_mutator(Game &game,
                                        const OrderedAction &ordered) const {
    Board &board = game.board;
    if (board.is_game_over() || board.waiting_noble ||
        board.current_player >= Board::NUM_PLAYERS)
      return false;
    PlayerState &player = board.players[board.current_player];
    if (ordered.oracle_card >= 0 && !is_valid_card_id(ordered.oracle_card))
      return false;
    Board::RuleMutator<MaintainExactHash> mutation(board);
    if (ordered.oracle_card >= 0) {
      const Card &card = get_card(ordered.oracle_card);
      if (!player.can_afford(card))
        return false;
      if (!csplendor::detail::purchase_card<true>(board, mutation, card,
                                                  ordered.oracle_gold_as))
        return false;
    } else if (ordered.oracle_reserve) {
      if (!player.can_reserve() ||
          !is_valid_card_id(ordered.oracle_reserve_card) ||
          !hidden_catalog_.is_initially_hidden(ordered.oracle_reserve_card) ||
          reveal_state_.is_claimed(board, ordered.oracle_reserve_card) ||
          !has_blank_slot_at_level(
              board, get_card(ordered.oracle_reserve_card).level - 1))
        return false;
      csplendor::detail::reserve_card_unchecked(
          board, mutation, ordered.oracle_reserve_card, true);
      const bool granted_gold =
          csplendor::detail::grant_reserve_gold(board, mutation);
      std::array<uint8_t, 6> returned = {0};
      if (granted_gold && ordered.oracle_return_color >= 0)
        returned[ordered.oracle_return_color] = 1;
      if (!csplendor::detail::return_gems_checked(board, mutation, returned))
        return false;
    } else {
      return false;
    }

    csplendor::detail::finish_standard_action(board, mutation);
    mutation.commit();
    return true;
  }

  bool apply_oracle_action(Game &game, const OrderedAction &ordered) const {
#ifdef CSPLENDOR_INCREMENTAL_EXACT_HASH
    if (game.board.hash_valid)
      return apply_oracle_action_with_mutator<true>(game, ordered);
#endif
    return apply_oracle_action_with_mutator<false>(game, ordered);
  }

  std::vector<OrderedAction>
  forced_attacker_actions(const Game &game,
                          const std::vector<OrderedAction> &actions, int depth,
                          bool is_root = false) {
    if (is_root && required_root_action_ != UINT64_MAX) {
      std::vector<OrderedAction> required;
      std::copy_if(actions.begin(), actions.end(), std::back_inserter(required),
                   [&](const OrderedAction &action) {
                     return action.code == required_root_action_;
                   });
      return required;
    }
    const size_t strict_prefix = strict_preferred_attacker_actions_
                                     ? preferred_attacker_actions_.size()
                                     : strict_preferred_attacker_prefix_;
    if (strict_prefix > 0) {
      const int index = depth_ - depth;
      if (index >= 0 && static_cast<size_t>(index) < strict_prefix) {
        if (index >= static_cast<int>(preferred_attacker_actions_.size()))
          return {};
        const uint64_t preferred = preferred_attacker_actions_[index];
        std::vector<OrderedAction> required;
        std::copy_if(actions.begin(), actions.end(),
                     std::back_inserter(required),
                     [&](const OrderedAction &action) {
                       return action.code == preferred;
                     });
        return required;
      }
    }

    if (exhaustive_attacker_actions_) {
      std::vector<OrderedAction> exhaustive = actions;
      prefer_candidate_action(exhaustive, depth);
      return exhaustive;
    }

    std::vector<OrderedAction> filtered;
    if (csplendor::solver_internal::compact_forced_actions_enabled) {
      filtered = csplendor::solver_internal::compact_forced_attacker_actions<
          ATTACKER_TAKE_LIMIT, ATTACKER_RESERVE_LIMIT>(
          actions, [&](const Action &action) {
            return attacker_take_score(game, action);
          });
    } else {
      std::vector<OrderedAction> purchases;
      std::vector<std::pair<int, OrderedAction>> takes;
      std::vector<OrderedAction> reserves;
      std::vector<OrderedAction> passthrough;
      for (const OrderedAction &ordered : actions) {
        const Action action = Action::unpack(ordered.code);
        switch (action.type) {
        case PURCHASE:
        case VISIT_NOBLE:
          purchases.push_back(ordered);
          break;
        case TAKE_DIFFERENT:
        case TAKE_SAME:
          takes.push_back({attacker_take_score(game, action), ordered});
          break;
        case RESERVE_VISIBLE:
          reserves.push_back(ordered);
          break;
        default:
          passthrough.push_back(ordered);
          break;
        }
      }

      std::sort(takes.begin(), takes.end(),
                [](const auto &left, const auto &right) {
                  if (left.first != right.first)
                    return left.first > right.first;
                  return left.second < right.second;
                });
      std::sort(reserves.begin(), reserves.end());

      filtered.insert(filtered.end(), purchases.begin(), purchases.end());
      for (size_t index = 0;
           index < takes.size() && index < ATTACKER_TAKE_LIMIT; ++index)
        filtered.push_back(takes[index].second);
      for (size_t index = 0;
           index < reserves.size() && index < ATTACKER_RESERVE_LIMIT; ++index)
        filtered.push_back(reserves[index]);
      filtered.insert(filtered.end(), passthrough.begin(), passthrough.end());
    }
    prefer_candidate_action(filtered, depth);
    return filtered;
  }

  void prefer_candidate_action(std::vector<OrderedAction> &actions,
                               int depth) const {
    const int index = depth_ - depth;
    if (index < 0 ||
        index >= static_cast<int>(preferred_attacker_actions_.size()))
      return;
    const uint64_t preferred = preferred_attacker_actions_[index];
    const auto it =
        std::find_if(actions.begin(), actions.end(), [&](const auto &action) {
          return action.code == preferred;
        });
    if (it != actions.end())
      std::rotate(actions.begin(), it, it + 1);
  }

  static int attacker_take_score(const Game &game, const Action &action) {
    const PlayerState &player = game.board.players[game.current_player()];
    std::array<int, 6> after_gems = {0};
    for (int color = 0; color < 6; ++color) {
      const int take = color < 5 ? action.take[color] : 0;
      after_gems[color] = static_cast<int>(player.gems[color]) + take -
                          action.return_gems[color];
    }

    int best = 0;
    for (int level = 0; level < 3; ++level) {
      for (int slot = 0; slot < Board::CARDS_PER_LEVEL; ++slot) {
        const int card_id = game.board.visible[level][slot];
        if (is_valid_card_id(card_id))
          best = std::max(best, attacker_take_score_for_card(
                                    player, after_gems, get_card(card_id)));
      }
    }
    for (int slot = 0; slot < Board::MAX_RESERVED; ++slot) {
      const int card_id = player.reserved[slot];
      if (is_valid_card_id(card_id))
        best = std::max(best, attacker_take_score_for_card(player, after_gems,
                                                           get_card(card_id)));
    }
    return best;
  }

  static int attacker_take_score_for_card(const PlayerState &player,
                                          const std::array<int, 6> &after_gems,
                                          const Card &card) {
    const int before_gap = card_payment_gap(player, player.gems, card);
    const int after_gap = card_payment_gap(player, after_gems, card);
    return card.points * 1000 + (before_gap - after_gap) * 100 - after_gap * 10;
  }

  template <typename Gems>
  static int card_payment_gap(const PlayerState &player, const Gems &gems,
                              const Card &card) {
    int shortage = 0;
    for (int color = 0; color < 5; ++color) {
      const int need = std::max(0, static_cast<int>(card.cost[color]) -
                                       static_cast<int>(player.bonuses[color]));
      shortage += std::max(0, need - static_cast<int>(gems[color]));
    }
    return std::max(0, shortage - static_cast<int>(gems[GOLD]));
  }

  static OrderedAction ordered_action(const Action &action, uint64_t code) {
    int rank = 9;
    switch (action.type) {
    case PURCHASE:
    case VISIT_NOBLE:
      rank = 0;
      break;
    case RESERVE_VISIBLE:
      rank = 1;
      break;
    case TAKE_DIFFERENT:
    case TAKE_SAME:
      rank = 2;
      break;
    default:
      break;
    }
    int points = 0;
    if (is_valid_card_id(action.card_id))
      points = get_card(action.card_id).points;
    return OrderedAction{rank, -points, code};
  }

  StateKey state_key(const Game &game, bool root_independent = false) const {
    CSPLENDOR_PERF_INC(SolverStateKeyCalls);
    const Board &board = game.board;
    CardIdSet unseen;
    CardIdSet acquired_hidden;
    uint64_t rule_position_hash = 0;
    if (reveal_state_.active()) {
      CSPLENDOR_PERF_INC(SolverRevealStateFastKeyReads);
#ifdef CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE
      reveal_state_.verify_or_abort(board, hidden_catalog_);
#endif
      unseen = reveal_state_.remaining_all();
      acquired_hidden =
          root_independent ? CardIdSet{} : reveal_state_.acquired_hidden();
      rule_position_hash = reveal_state_.rule_hash();
    } else {
      unseen = hidden_catalog_.unseen_cards(board);
      acquired_hidden = root_independent
                            ? CardIdSet{}
                            : hidden_catalog_.acquired_hidden_cards(board);
      rule_position_hash = board.compute_set_deck_search_hash();
    }
    return StateKey{StateKeyCore{rule_position_hash, board.players[0].points,
                                 board.players[1].points,
                                 board.players[0].purchased_count,
                                 board.players[1].purchased_count,
                                 board.final_round, board.winner},
                    unseen.low,
                    unseen.high,
                    acquired_hidden.low,
                    acquired_hidden.high,
                    board.players[0].reserved_count,
                    board.players[1].reserved_count};
  }

  bool collect_frontier_action(
      Game &game, const OrderedAction &ordered, int depth,
      bool preserve_child_depth, size_t edge_limit,
      std::vector<RevealVerifiedFrontierEdge> &action_edges) {
    const int current_player = game.current_player();
    const bool waiting_noble_action = game.board.waiting_noble;
    bool all_children_proven = true;
    const bool completed =
        for_each_proof_outcome(game, ordered, [&](int reveal_card) {
          const int next_depth =
              waiting_noble_action || preserve_child_depth
                  ? depth
                  : depth - static_cast<int>(current_player == attacker_ &&
                                             game.current_player() !=
                                                 current_player);
          DepthPath child_path(path_reserve_capacity(next_depth));
          const ForceStatus child_status =
              forced_win(game, child_path, next_depth, false);
          if (child_status != ForceStatus::PROVEN) {
            all_children_proven = false;
            return false;
          }
          if (edge_limit && action_edges.size() >= edge_limit)
            throw ProofDagBuildAborted("frontier edge limit exceeded");
          action_edges.push_back(RevealVerifiedFrontierEdge{
              ordered.code, reveal_card, next_depth, game.clone_light()});
          return true;
        });
    return completed && !action_edges.empty() && all_children_proven;
  }

  void expand_frontier_node(
      Game &game, int depth, size_t edge_limit,
      std::vector<RevealVerifiedFrontierEdge> &frontier_edges) {
    const DepthStateKey key{state_key(game), depth};
    CSPLENDOR_PERF_TT_PROBE_SCOPE(frontier_memo_probe);
    const auto memo_it = memo_.find(key);
    CSPLENDOR_PERF_TT_PROBE_FINISH(frontier_memo_probe);
    if (memo_it != memo_.end())
      CSPLENDOR_PERF_INC(SolverTtHits);
    if (memo_it == memo_.end() ||
        memo_it->second.status() != ForceStatus::PROVEN) {
      throw ProofDagBuildAborted(
          "frontier references an unmaterialized proof state");
    }
    const Entry entry = memo_it->second;
    const int current_player = game.current_player();
    const bool preserve_child_depth = can_resolve_final_round(game);
    std::vector<OrderedAction> actions = proof_ordered_actions(game);

    if (current_player == attacker_ && entry.has_action()) {
      actions = forced_attacker_actions(game, actions, depth, true);
      actions.erase(std::remove_if(actions.begin(), actions.end(),
                                   [&](const OrderedAction &ordered) {
                                     return ordered.code != entry.action_code();
                                   }),
                    actions.end());
    }

    bool expanded = false;
    for (const OrderedAction &ordered : actions) {
      std::vector<RevealVerifiedFrontierEdge> action_edges;
      if (edge_limit && frontier_edges.size() >= edge_limit)
        throw ProofDagBuildAborted("frontier edge limit exceeded");
      const size_t remaining_limit =
          edge_limit ? edge_limit - frontier_edges.size() : 0;
      const bool action_proven =
          collect_frontier_action(game, ordered, depth, preserve_child_depth,
                                  remaining_limit, action_edges);
      if (current_player == attacker_ && !action_proven)
        continue;
      if (!action_proven)
        throw ProofDagBuildAborted(
            "frontier reveal refinement failed for a defender response");
      if (edge_limit &&
          frontier_edges.size() + action_edges.size() > edge_limit) {
        throw ProofDagBuildAborted("frontier edge limit exceeded");
      }
      frontier_edges.insert(frontier_edges.end(),
                            std::make_move_iterator(action_edges.begin()),
                            std::make_move_iterator(action_edges.end()));
      expanded = true;
      if (current_player == attacker_)
        break;
    }
    if (!expanded)
      throw ProofDagBuildAborted(
          "frontier could not select a replayable proven action");
  }

  RevealVerifiedProofDag build_proof_dag() {
    RevealVerifiedProofDag dag;
    dag.requested = true;
    exact_memo_.clear();
    proof_node_ids_.clear();
    proof_terminal_node_ids_.clear();
    proof_builder_.reset();
    const RevealVerifiedSearchStats search_stats = stats_;
    try {
      Game game = root_.clone_light();
      reveal_state_ = root_reveal_state_;
      dag.root = build_proof_node(game, depth_);
      reveal_state_ = root_reveal_state_;
      validate_proof_dag(game, depth_, dag.root);
      dag.complete = true;
      dag.validated = true;
      dag.nodes = proof_builder_.release_nodes();
    } catch (const ProofDagBuildAborted &exc) {
      dag.omitted_reason = exc.what();
      proof_builder_.reset();
      proof_node_ids_.clear();
      proof_terminal_node_ids_.clear();
    } catch (const SearchLimitExceeded &exc) {
      dag.omitted_reason = exc.what();
      proof_builder_.reset();
      proof_node_ids_.clear();
      proof_terminal_node_ids_.clear();
    }
    stats_ = search_stats;
    return dag;
  }

  void validate_proof_dag(Game &game, int depth, size_t root_id) {
    std::unordered_map<size_t, DepthStateKey> seen;
    validate_proof_dag_node(game, depth, root_id, seen);
  }

  void
  validate_proof_dag_node(Game &game, int depth, size_t node_id,
                          std::unordered_map<size_t, DepthStateKey> &seen) {
    if (node_id >= proof_builder_.node_count())
      throw ProofDagBuildAborted("proof DAG validation found invalid node id");
    const DepthStateKey key{state_key(game), depth};
    const auto known = seen.find(node_id);
    if (known != seen.end()) {
      if (!(known->second == key))
        throw ProofDagBuildAborted(
            "proof DAG validation found inconsistent shared node state");
      return;
    }
    seen.emplace(node_id, key);

    const RevealVerifiedProofNode &node = proof_builder_.nodes()[node_id];
    validate_proof_node_summary(game, depth, node);
    if (game.is_game_over()) {
      if (!node.children.empty())
        throw ProofDagBuildAborted(
            "proof DAG validation found terminal node with children");
      return;
    }

    const bool preserve_child_depth = node.kind == "final_round_summary";
    for (const RevealVerifiedProofEdge &edge : node.children) {
      validate_proof_edge(game, depth, edge, preserve_child_depth, seen);
    }
  }

  void validate_proof_node_summary(const Game &game, int depth,
                                   const RevealVerifiedProofNode &node) const {
    if (node.depth != depth || node.player != game.current_player() ||
        node.winner != game.winner() ||
        node.waiting_noble != game.board.waiting_noble ||
        node.scores[0] != static_cast<int>(game.board.players[0].points) ||
        node.scores[1] != static_cast<int>(game.board.players[1].points)) {
      std::ostringstream message;
      message << "proof DAG validation found node summary mismatch"
              << " id=" << node.id << " expected_depth=" << depth
              << " actual_depth=" << node.depth
              << " expected_player=" << game.current_player()
              << " actual_player=" << node.player
              << " expected_winner=" << game.winner()
              << " actual_winner=" << node.winner
              << " expected_waiting=" << game.board.waiting_noble
              << " actual_waiting=" << node.waiting_noble << " expected_scores="
              << static_cast<int>(game.board.players[0].points) << ","
              << static_cast<int>(game.board.players[1].points)
              << " actual_scores=" << node.scores[0] << "," << node.scores[1];
      throw ProofDagBuildAborted(message.str());
    }
  }

  void validate_proof_edge(Game &game, int depth,
                           const RevealVerifiedProofEdge &edge,
                           bool preserve_child_depth,
                           std::unordered_map<size_t, DepthStateKey> &seen) {
    if (edge.oracle_card >= 0 || edge.oracle_reserve ||
        edge.oracle_reserve_card >= 0 || edge.oracle_return_color >= 0) {
      throw ProofDagBuildAborted(
          "proof DAG validation found non-replayable oracle edge");
    }
    const std::vector<uint64_t> legal_codes = game.legal_action_codes();
    if (std::find(legal_codes.begin(), legal_codes.end(), edge.action_code) ==
        legal_codes.end()) {
      throw ProofDagBuildAborted(
          "proof DAG validation found illegal action edge");
    }

    const int current_player = game.current_player();
    const bool previous_waiting_noble = game.board.waiting_noble;
    ScopedBranchRollback rollback(*this, game);
    rollback.mark_mutated();
    const Action action = Action::unpack(edge.action_code);
    bool applied = false;
    if (action.type == RESERVE_DECK) {
      if (edge.reveal_card < 0)
        throw ProofDagBuildAborted(
            "proof DAG validation found deck reserve without reveal card");
      applied = apply_deck_reserve_outcome(game, action, edge.reveal_card);
    } else {
      const int level = visible_refill_level(action);
      if (level >= 0 && !game.board.decks[level].empty()) {
        const int slot = visible_refill_slot(game.board, action);
        if (edge.reveal_card < 0)
          throw ProofDagBuildAborted(
              "proof DAG validation found visible refill without reveal card");
        applied = apply_visible_refill_outcome(game, action, level, slot,
                                               edge.reveal_card);
      } else {
        if (edge.reveal_card >= 0)
          throw ProofDagBuildAborted(
              "proof DAG validation found unexpected reveal card");
        applied = apply_action_code_tracked(game, edge.action_code);
      }
    }
    if (!applied) {
      throw ProofDagBuildAborted(
          "proof DAG validation could not replay edge transition");
    }
    const int next_depth =
        previous_waiting_noble || preserve_child_depth
            ? depth
            : depth - static_cast<int>(current_player == attacker_ &&
                                       game.current_player() != current_player);
    validate_proof_dag_node(game, next_depth, edge.child, seen);
  }

  void append_proof_edge(size_t id, const OrderedAction &ordered,
                         int reveal_card, size_t child) {
    proof_builder_.append_edge(
        id, RevealVerifiedProofEdge{
                ordered.code, reveal_card, ordered.oracle_card,
                ordered.oracle_reserve, ordered.oracle_reserve_card,
                ordered.oracle_return_color, ordered.oracle_gold_as, child});
  }

  size_t build_terminal_proof_node(const Game &game, int depth) {
    const DepthStateKey key{state_key(game), depth};
    auto known = proof_terminal_node_ids_.find(key);
    if (known != proof_terminal_node_ids_.end())
      return known->second;
    const size_t id = proof_builder_.node_count();
    proof_terminal_node_ids_[key] = id;
    proof_builder_.append_node(make_proof_node(id, game, depth));
    proof_builder_.nodes()[id].kind = "terminal";
    proof_builder_.nodes()[id].resolution =
        game.winner() == attacker_ ? "attacker_win" : "non_attacker_win";
    return id;
  }

  std::pair<size_t, bool> build_final_round_noble_proof_node(Game &game,
                                                             int depth) {
    const size_t id = proof_builder_.node_count();
    proof_builder_.append_node(make_proof_node(id, game, depth));
    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions = proof_ordered_actions(game);
    bool proven = current_player != attacker_;
    bool expanded = false;
    for (const OrderedAction &ordered : actions) {
      ScopedBranchRollback rollback(*this, game);
      rollback.mark_mutated();
      if (!apply_action_code_tracked(game, ordered.code))
        continue;
      if (!game.is_game_over()) {
        throw ProofDagBuildAborted(
            "final round noble choice did not reach terminal state");
      }
      const bool child_proven = game.winner() == attacker_;
      const size_t child = build_proof_node(game, depth);
      if (current_player == attacker_) {
        if (!child_proven)
          continue;
        append_proof_edge(id, ordered, -1, child);
        expanded = true;
        proven = true;
        break;
      }
      append_proof_edge(id, ordered, -1, child);
      expanded = true;
      proven = proven && child_proven;
    }
    return {id, expanded && proven};
  }

  void expand_final_round_proof_summary(Game &game, int depth, size_t id) {
    if (!can_resolve_final_round(game))
      throw ProofDagBuildAborted(
          "proof DAG cannot expand non-final-round summary");
    proof_builder_.nodes()[id].kind = "final_round_summary";
    const int current_player = game.current_player();
    const std::vector<OrderedAction> actions = proof_ordered_actions(game);
    bool proven = current_player != attacker_;
    bool expanded = false;
    for (const OrderedAction &ordered : actions) {
      std::vector<std::pair<int, size_t>> edges;
      bool action_proven = true;
      const bool completed =
          for_each_proof_outcome(game, ordered, [&](int reveal) {
            size_t child = 0;
            bool child_proven = false;
            if (game.board.waiting_noble) {
              std::tie(child, child_proven) =
                  build_final_round_noble_proof_node(game, depth);
            } else {
              if (!game.is_game_over())
                throw ProofDagBuildAborted(
                    "final round action did not reach terminal state");
              child_proven = game.winner() == attacker_;
              child = build_proof_node(game, depth);
            }
            edges.push_back({reveal, child});
            action_proven = action_proven && child_proven;
            return true;
          });
      action_proven = completed && !edges.empty() && action_proven;
      if (current_player == attacker_ && !action_proven)
        continue;
      for (const auto &[reveal, child] : edges)
        append_proof_edge(id, ordered, reveal, child);
      expanded = true;
      if (current_player == attacker_) {
        proven = true;
        break;
      }
      proven = proven && action_proven;
    }
    if (!expanded || !proven)
      throw ProofDagBuildAborted(
          "final round summary could not be expanded as proof");
  }

  size_t build_proof_node(Game &game, int depth) {
    if (game.is_game_over())
      return build_terminal_proof_node(game, depth);
    const DepthStateKey key{state_key(game), depth};
    auto known = proof_node_ids_.find(key);
    if (known != proof_node_ids_.end())
      return known->second;
    const size_t id = proof_builder_.node_count();
    proof_node_ids_[key] = id;
    proof_builder_.append_node(make_proof_node(id, game, depth));
    CSPLENDOR_PERF_TT_PROBE_SCOPE(proof_memo_probe);
    auto memo_it = memo_.find(key);
    CSPLENDOR_PERF_TT_PROBE_FINISH(proof_memo_probe);
    if (memo_it != memo_.end())
      CSPLENDOR_PERF_INC(SolverTtHits);
    if (memo_it == memo_.end() ||
        memo_it->second.status() != ForceStatus::PROVEN) {
      DepthPath path(path_reserve_capacity(depth));
      const ForceStatus refined = forced_win(game, path, depth, false);
      if (refined != ForceStatus::PROVEN)
        throw ProofDagBuildAborted("proof DAG reveal refinement failed");
      CSPLENDOR_PERF_TT_PROBE_SCOPE(refined_proof_memo_probe);
      memo_it = memo_.find(key);
      CSPLENDOR_PERF_TT_PROBE_FINISH(refined_proof_memo_probe);
      if (memo_it != memo_.end())
        CSPLENDOR_PERF_INC(SolverTtHits);
    }
    if (memo_it == memo_.end() ||
        memo_it->second.status() != ForceStatus::PROVEN) {
      throw ProofDagBuildAborted("proof DAG references unmaterialized subtree");
    }
    const Entry &entry = memo_it->second;
    if (!entry.has_action()) {
      expand_final_round_proof_summary(game, depth, id);
      return id;
    }

    std::vector<OrderedAction> actions = proof_ordered_actions(game);
    if (game.current_player() == attacker_) {
      actions = forced_attacker_actions(game, actions, depth, id == 0);
      actions.erase(std::remove_if(actions.begin(), actions.end(),
                                   [&](const OrderedAction &ordered) {
                                     return ordered.code != entry.action_code();
                                   }),
                    actions.end());
    }
    const int current_player = game.current_player();
    const bool waiting_noble_action = game.board.waiting_noble;
    for (const OrderedAction &ordered : actions) {
      for_each_proof_outcome(game, ordered, [&](int reveal_card) {
        const int next_depth =
            waiting_noble_action
                ? depth
                : depth -
                      static_cast<int>(current_player == attacker_ &&
                                       game.current_player() != current_player);
        const size_t child = build_proof_node(game, next_depth);
        append_proof_edge(id, ordered, reveal_card, child);
        return true;
      });
    }
    return id;
  }

  static RevealVerifiedProofNode make_proof_node(size_t id, const Game &game,
                                                 int depth) {
    RevealVerifiedProofNode node;
    node.id = id;
    node.player = game.current_player();
    node.depth = depth;
    node.scores = {static_cast<int>(game.board.players[0].points),
                   static_cast<int>(game.board.players[1].points)};
    node.winner = game.winner();
    node.waiting_noble = game.board.waiting_noble;
    for (uint8_t noble_id : game.board.nobles)
      node.nobles.push_back(static_cast<int>(noble_id));
    for (int player = 0; player < Board::NUM_PLAYERS; ++player) {
      for (uint8_t noble_id : game.board.players[player].acquired_nobles)
        node.acquired_nobles[player].push_back(static_cast<int>(noble_id));
    }
    return node;
  }

  std::vector<RevealVerifiedLineEntry>
  principal_line(bool exact_refinement = false) {
    ScopedRevealStateRestore restore_state(*this);
    Game game = root_.clone_light();
    reveal_state_ = root_reveal_state_;
    if (exact_refinement)
      return principal_line_from_memo(game, true, exact_memo_);
    return principal_line_from_memo(game, false, memo_);
  }

  template <typename MemoType>
  std::vector<RevealVerifiedLineEntry>
  principal_line_from_memo(Game &game, bool exact_refinement,
                           const MemoType &memo) {
    std::vector<RevealVerifiedLineEntry> line;
    CSPLENDOR_PERF_INC(SolverTemporarySetAllocations);
    std::unordered_set<DepthStateKey, DepthStateKeyHash> seen;
    int depth = depth_;
    for (int ply = 0; ply < 200 && depth >= 0; ++ply) {
      if (game.is_game_over())
        break;
      const StateKey state = state_key(game, exact_refinement);
      const DepthStateKey seen_key{state, depth};
      if (seen.find(seen_key) != seen.end())
        break;
      seen.insert(seen_key);
      const typename MemoType::key_type key{state, depth};
      CSPLENDOR_PERF_TT_PROBE_SCOPE(line_memo_probe);
      auto it = memo.find(key);
      CSPLENDOR_PERF_TT_PROBE_FINISH(line_memo_probe);
      if (it != memo.end())
        CSPLENDOR_PERF_INC(SolverTtHits);
      if (it == memo.end() || !it->second.has_action() ||
          !it->second.replayable())
        break;
      const int current_player = game.current_player();
      const Action action = Action::unpack(it->second.action_code());
      const int reveal_card = it->second.reveal_card();
      bool applied = false;
      if (action.type == RESERVE_DECK && reveal_card >= 0) {
        applied = apply_deck_reserve_outcome(game, action, reveal_card);
      } else if (reveal_card >= 0 && visible_refill_level(action) >= 0) {
        const int level = visible_refill_level(action);
        const int slot = visible_refill_slot(game.board, action);
        applied = apply_visible_refill_outcome(game, action, level, slot,
                                               reveal_card);
      } else {
        applied = apply_action_code_tracked(game, it->second.action_code());
      }
      if (!applied)
        break;
      line.push_back(RevealVerifiedLineEntry{
          it->second.action_code(), reveal_card, it->second.action_count()});
      depth -= static_cast<int>(current_player == attacker_ &&
                                game.current_player() != current_player);
    }
    return line;
  }

  void check_limits() const { limits_.check(stats_.nodes); }
};

RevealVerifiedSolver::RevealVerifiedSolver(
    int attacker, int depth, uint64_t max_nodes, double time_limit_seconds,
    std::vector<uint64_t> preferred_attacker_actions, bool include_proof_dag,
    size_t proof_dag_node_limit, size_t proof_dag_edge_limit,
    uint64_t required_root_action, bool strict_preferred_attacker_actions,
    size_t strict_preferred_attacker_prefix, bool exhaustive_attacker_actions,
    bool exact_reveal_search,
    std::shared_ptr<RevealSearchCancellationToken> cancellation_token)
    : impl_(std::make_unique<Impl>(
          attacker, depth, max_nodes, time_limit_seconds,
          std::move(preferred_attacker_actions), include_proof_dag,
          proof_dag_node_limit, proof_dag_edge_limit, required_root_action,
          strict_preferred_attacker_actions, strict_preferred_attacker_prefix,
          exhaustive_attacker_actions, exact_reveal_search,
          std::move(cancellation_token))) {}

RevealVerifiedSolver::~RevealVerifiedSolver() = default;

RevealVerifiedSolver::RevealVerifiedSolver(const RevealVerifiedSolver &other)
    : impl_(std::make_unique<Impl>(*other.impl_)) {}

RevealVerifiedSolver &
RevealVerifiedSolver::operator=(const RevealVerifiedSolver &other) {
  if (this != &other)
    impl_ = std::make_unique<Impl>(*other.impl_);
  return *this;
}

RevealVerifiedSolver::RevealVerifiedSolver(RevealVerifiedSolver &&) noexcept =
    default;

RevealVerifiedSolver &
RevealVerifiedSolver::operator=(RevealVerifiedSolver &&) noexcept = default;

RevealVerifiedSearchResult RevealVerifiedSolver::solve(const Game &input) {
  return impl_->solve(input);
}

RevealVerifiedSearchResult RevealVerifiedSolver::solve_reusing_exact_cache(
    const Game &input, int depth, uint64_t max_nodes, double time_limit_seconds,
    std::vector<uint64_t> preferred_attacker_actions,
    std::shared_ptr<RevealSearchCancellationToken> cancellation_token,
    size_t max_cache_states) {
  return impl_->solve_reusing_exact_cache(
      input, depth, max_nodes, time_limit_seconds,
      std::move(preferred_attacker_actions), std::move(cancellation_token),
      max_cache_states);
}

void RevealVerifiedSolver::clear_exact_cache() { impl_->clear_exact_cache(); }

void RevealVerifiedSolver::trim_exact_cache(size_t max_cache_states) {
  impl_->trim_exact_cache(max_cache_states);
}

size_t RevealVerifiedSolver::exact_cache_size() const noexcept {
  return impl_->exact_cache_size();
}

RevealVerifiedFrontierResult
RevealVerifiedSolver::split_root(const Game &input, size_t edge_limit) {
  return impl_->split_root(input, edge_limit);
}

RevealVerifiedFrontierResult
RevealVerifiedSolver::expand_frontier(const Game &input, size_t edge_limit) {
  return impl_->expand_frontier(input, edge_limit);
}
