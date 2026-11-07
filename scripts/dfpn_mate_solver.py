#!/usr/bin/env python3
"""Reveal-verified mate search, visible-only solver and their CLI.

The proof search is the native ``solve_reveal_verified_mate_cpp``
(doc/mate_usage.md); this module drives it, replays its principal line and
re-exports the KIFU / strategy-DAG encoders of ``scripts.dfpn_output``.
"""
from __future__ import annotations

import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs
from csplendor.api.usi_kifu import now_iso
from scripts import dfpn_output as _dfpn_output
from scripts.dfpn_cli import main as _dfpn_cli_main
from scripts.dfpn_types import RevealVerifiedStats, SearchLimitExceeded
from scripts.mate_solver import (
    INVALID_INPUT,
    MATE,
    NO_MATE,
    UNKNOWN,
    MateSolver,
    SearchResult,
    SearchStats,
    SolverOptions,
    _parse_moves,
    apply_usi_moves,
    load_game_from_json,
    load_game_from_usi_file,
    load_game_from_usi_text,
)

# Used by scripts.dfpn_cli through this module.
_CLI_EXPORTS = (
    INVALID_INPUT,
    NO_MATE,
    _parse_moves,
    apply_usi_moves,
    load_game_from_json,
    load_game_from_usi_file,
    load_game_from_usi_text,
)

PLAYER0_WIN = "Player0Win"
PLAYER1_WIN = "Player1Win"
DRAW = "Draw"


def proof_dag_to_compact(
    proof_dag: Dict[str, Any],
    *,
    include_card_classes: bool = False,
) -> Dict[str, Any]:
    return _dfpn_output.proof_dag_to_compact(
        proof_dag,
        include_card_classes=include_card_classes,
    )


def compact_proof_dag_to_v1(compact: Dict[str, Any]) -> Dict[str, Any]:
    return _dfpn_output.compact_proof_dag_to_v1(compact)


def strategy_dag_size_report(
    v1_dag: Dict[str, Any],
    compact_dag: Dict[str, Any],
) -> Dict[str, Any]:
    return _dfpn_output.strategy_dag_size_report(v1_dag, compact_dag)


def strategy_dag_node_count(dag: Dict[str, Any]) -> int:
    return _dfpn_output.strategy_dag_node_count(dag)


def strategy_dag_max_children(
    dag: Dict[str, Any],
    *,
    player: Optional[int] = None,
) -> int:
    return _dfpn_output.strategy_dag_max_children(dag, player=player)


def principal_line_to_kifu_text(
    game: cs.Game,
    line: Sequence[Dict[str, Any]],
    *,
    attacker: int,
    reveal_hidden_reserved_ids: bool = False,
) -> str:
    return _dfpn_output.principal_line_to_kifu_text(
        game,
        line,
        attacker=attacker,
        reveal_hidden_reserved_ids=reveal_hidden_reserved_ids,
        _now_iso=now_iso,
    )


def write_principal_line_kifu(
    path: str,
    game: cs.Game,
    line: Sequence[Dict[str, Any]],
    *,
    attacker: int,
) -> None:
    _dfpn_output.write_principal_line_kifu(
        path,
        game,
        line,
        attacker=attacker,
        _now_iso=now_iso,
    )


@dataclass
class _VisibleOnlyEntry:
    winner: int
    reason: str
    action_code: Optional[int] = None
    action_count: int = 0


class VisibleOnlyWinnerSolver:
    """Visible-card mate proof with full-response minimax fallback."""

    def __init__(self, options: Optional[SolverOptions] = None):
        self.options = options or SolverOptions()
        self.stats = SearchStats()
        self._start_time = 0.0
        self._helper = MateSolver(attacker=0, max_depth=0, options=self.options)
        self._memo: Dict[Tuple[Any, ...], _VisibleOnlyEntry] = {}
        self._root_game: Optional[cs.Game] = None

    def solve(self, game: cs.Game) -> SearchResult:
        self.stats = SearchStats()
        self._memo = {}
        self._start_time = time.monotonic()
        working = self._visible_only_game(game)
        self._root_game = working.clone_light()

        if hasattr(cs, "solve_visible_only_winner_cpp"):
            return self._solve_cpp(working)

        try:
            winner = self._minimax(working, set(), self._known_card_count(working))
            root_key = self._hash(working)
            root_entry = self._memo.get(root_key)
            reason = root_entry.reason if root_entry is not None else self._terminal_reason(working)
            unknown_reason = None
        except SearchLimitExceeded as exc:
            winner = -1
            reason = str(exc)
            unknown_reason = str(exc)
            self.stats.unknown_reason = unknown_reason

        self.stats.elapsed_ms = (time.monotonic() - self._start_time) * 1000.0
        status = self._winner_status(winner)
        line = self._principal_line() if self.options.include_proof else None
        final_scores = line[-1]["scores_after"] if line else [int(v) for v in working.scores]
        payload = {
            "mode": "visible_only_winner",
            "winner": winner,
            "winner_reason": reason,
            "unknown_reason": unknown_reason,
            "assumptions": {
                "hidden_decks_ignored": True,
                "visible_refill": "none",
                "reserve_deck": "disabled",
                "max_depth_ignored": True,
                "mate_proof": False,
                "policy": "full_legal_minimax_with_cycle_score_adjudication",
                "purchase_payments": self._payment_policy(working),
                "all_visible_only_responses_read": True,
                "equivalent_child_states_collapsed": True,
            },
            "final_scores": final_scores,
            "memoized_states": len(self._memo),
            "line": line,
        }
        return SearchResult(status, None, payload if self.options.include_proof else None, None, self.stats)

    def _solve_cpp(self, game: cs.Game) -> SearchResult:
        raw = cs.solve_visible_only_winner_cpp(
            game,
            max_nodes=max(0, int(self.options.max_nodes)),
            time_limit_seconds=max(0.0, float(self.options.time_limit)),
        )
        stats = raw["stats"]
        self.stats = SearchStats(
            nodes=int(stats["nodes"]),
            memo_hits=int(stats["memo_hits"]),
            terminal_nodes=int(stats["terminal_nodes"]),
            legal_moves=int(stats["legal_moves"]),
            elapsed_ms=float(stats["elapsed_ms"]),
            unknown_reason=raw["unknown_reason"],
        )
        winner = int(raw["winner"])
        forced_win_depth = raw["forced_win_depth"]
        policy = (
            "bounded_forced_win_with_all_defender_responses"
            if forced_win_depth is not None
            else "full_legal_minimax_with_cycle_score_adjudication"
        )
        line = self._cpp_principal_line(raw["line"]) if self.options.include_proof else None
        final_scores = line[-1]["scores_after"] if line else [int(v) for v in game.scores]
        payload = {
            "mode": "visible_only_winner",
            "winner": winner,
            "winner_reason": str(raw["winner_reason"]),
            "unknown_reason": raw["unknown_reason"],
            "assumptions": {
                "hidden_decks_ignored": True,
                "visible_refill": "none",
                "reserve_deck": "disabled",
                "max_depth_ignored": True,
                "mate_proof": forced_win_depth is not None,
                "policy": policy,
                "purchase_payments": self._payment_policy_for_mode(
                    bool(raw["simple_payment_mode"])
                ),
                "attacker_candidate_policy": (
                    "heuristic_subset_for_bounded_proof"
                    if forced_win_depth is not None
                    else "all_legal_actions"
                ),
                "all_visible_only_responses_read": True,
                "equivalent_child_states_collapsed": True,
            },
            "final_scores": final_scores,
            "forced_win_depth": forced_win_depth,
            "memoized_states": int(raw["memoized_states"]),
            "line": line,
        }
        return SearchResult(
            self._winner_status(winner),
            None,
            payload if self.options.include_proof else None,
            None,
            self.stats,
        )

    def _minimax(self, game: cs.Game, path: set, known_card_count: int) -> int:
        self._check_limits()
        self.stats.nodes += 1

        if game.is_game_over():
            self.stats.terminal_nodes += 1
            return int(game.winner)

        if known_card_count == 0:
            self.stats.terminal_nodes += 1
            return self._adjudicate_by_score(game)

        state_hash = self._hash(game)
        entry = self._memo.get(state_hash)
        if entry is not None:
            self.stats.memo_hits += 1
            return entry.winner

        if state_hash in path:
            self.stats.terminal_nodes += 1
            return self._adjudicate_by_score(game)

        action_codes = self._ordered_representative_action_codes(game)
        self.stats.legal_moves += len(action_codes)
        if not action_codes:
            self.stats.terminal_nodes += 1
            winner = self._adjudicate_by_score(game)
            self._memo[state_hash] = _VisibleOnlyEntry(
                winner=winner,
                reason="no_visible_only_action_adjudicated_by_score",
                action_count=0,
            )
            return winner

        current_player = int(game.board.current_player)
        path.add(state_hash)
        draw_action: Optional[int] = None
        losing_action: Optional[int] = None
        losing_winner: Optional[int] = None

        try:
            for action_code in action_codes:
                action_type = int(cs.Action.unpack(int(action_code)).type)
                if not game.apply_action_code_trusted(int(action_code), True):
                    continue
                try:
                    child_winner = self._minimax(
                        game,
                        path,
                        known_card_count - int(action_type == int(cs.ActionType.PURCHASE)),
                    )
                finally:
                    game.undo()

                if child_winner == current_player:
                    self._memo[state_hash] = _VisibleOnlyEntry(
                        winner=child_winner,
                        reason="current_player_can_force_visible_only_win",
                        action_code=int(action_code),
                        action_count=len(action_codes),
                    )
                    return child_winner
                if child_winner == -2 and draw_action is None:
                    draw_action = int(action_code)
                elif child_winner in (0, 1) and losing_action is None:
                    losing_action = int(action_code)
                    losing_winner = int(child_winner)
        finally:
            path.remove(state_hash)

        if draw_action is not None:
            entry = _VisibleOnlyEntry(
                winner=-2,
                reason="current_player_can_force_visible_only_draw",
                action_code=draw_action,
                action_count=len(action_codes),
            )
        else:
            entry = _VisibleOnlyEntry(
                winner=losing_winner if losing_winner is not None else -2,
                reason="all_visible_only_responses_lose_for_current_player",
                action_code=losing_action,
                action_count=len(action_codes),
            )
        self._memo[state_hash] = entry
        return entry.winner

    def _check_limits(self) -> None:
        if self.options.max_nodes and self.stats.nodes >= self.options.max_nodes:
            raise SearchLimitExceeded("node limit exceeded")
        if (
            self.options.time_limit
            and self._start_time
            and (time.monotonic() - self._start_time) >= self.options.time_limit
        ):
            raise SearchLimitExceeded("time limit exceeded")

    @staticmethod
    def _visible_only_game(game: cs.Game) -> cs.Game:
        visible_only = game.clone()
        visible_only.blank_refill_mode = True
        visible_only.board.decks = [[], [], []]
        return visible_only

    @staticmethod
    def _winner_status(winner: int) -> str:
        if winner == 0:
            return PLAYER0_WIN
        if winner == 1:
            return PLAYER1_WIN
        if winner == -2:
            return DRAW
        return UNKNOWN

    @staticmethod
    def _payment_policy(game: cs.Game) -> str:
        return VisibleOnlyWinnerSolver._payment_policy_for_mode(
            bool(game.simple_payment_mode)
        )

    @staticmethod
    def _payment_policy_for_mode(simple_payment_mode: bool) -> str:
        return (
            "canonical_minimal_gold_only"
            if simple_payment_mode
            else "all_legal_patterns"
        )

    @staticmethod
    def _known_card_count(game: cs.Game) -> int:
        board = game.board
        count = sum(1 for row in board.visible for card_id in row if int(card_id) >= 0)
        for player_idx in range(2):
            player = board.get_player(player_idx)
            count += sum(1 for card_id in player.reserved if int(card_id) >= 0)
        return count

    @staticmethod
    def _adjudicate_by_score(game: cs.Game) -> int:
        board = game.board
        player0 = board.get_player(0)
        player1 = board.get_player(1)
        if int(player0.points) > int(player1.points):
            return 0
        if int(player1.points) > int(player0.points):
            return 1
        if int(player0.purchased_count) < int(player1.purchased_count):
            return 0
        if int(player1.purchased_count) < int(player0.purchased_count):
            return 1
        return -2

    @staticmethod
    def _hash(game: cs.Game) -> Tuple[Any, ...]:
        board = game.board
        return (
            int(board.hash()),
            bool(board.final_round),
            int(board.winner),
            tuple(
                (
                    int(board.get_player(player_idx).points),
                    int(board.get_player(player_idx).purchased_count),
                )
                for player_idx in range(2)
            ),
        )

    @staticmethod
    def _terminal_reason(game: cs.Game) -> str:
        if game.is_game_over():
            return "game_over"
        if VisibleOnlyWinnerSolver._known_card_count(game) == 0:
            return "visible_cards_exhausted_adjudicated_by_score"
        return "unknown"

    def _ordered_representative_action_codes(self, game: cs.Game) -> List[int]:
        representatives: Dict[Tuple[Any, ...], Tuple[Tuple[int, int, int], int]] = {}
        for raw_code in game.legal_action_codes:
            action_code = int(raw_code)
            action = cs.Action.unpack(action_code)
            if int(action.type) == int(cs.ActionType.RESERVE_DECK):
                continue
            if not game.apply_action_code_trusted(action_code, True):
                continue
            child_hash = self._hash(game)
            game.undo()
            order_key = self._action_order_key(action)
            current = representatives.get(child_hash)
            if current is None or order_key < current[0]:
                representatives[child_hash] = (order_key, action_code)
        return [
            action_code
            for _, action_code in sorted(representatives.values(), key=lambda item: (item[0], item[1]))
        ]

    @staticmethod
    def _action_order_key(action: cs.Action) -> Tuple[int, int, int]:
        action_type = int(action.type)
        rank = {
            int(cs.ActionType.PURCHASE): 0,
            int(cs.ActionType.VISIT_NOBLE): 0,
            int(cs.ActionType.RESERVE_VISIBLE): 1,
            int(cs.ActionType.TAKE_DIFFERENT): 2,
            int(cs.ActionType.TAKE_SAME): 2,
        }.get(action_type, 9)
        points = 0
        card_id = int(action.card_id)
        if card_id >= 0:
            points = int(cs.get_card(card_id).points)
        return (rank, -points, int(action.pack()))

    def _principal_line(self, max_plies: int = 200) -> List[Dict[str, Any]]:
        if self._root_game is None:
            return []
        game = self._root_game.clone()
        line: List[Dict[str, Any]] = []
        seen = set()
        for ply in range(max_plies):
            if game.is_game_over() or self._known_card_count(game) == 0:
                break
            state_hash = self._hash(game)
            if state_hash in seen:
                break
            seen.add(state_hash)
            entry = self._memo.get(state_hash)
            if entry is None or entry.action_code is None:
                break
            action = cs.Action.unpack(entry.action_code)
            before_player = int(game.board.current_player)
            before_scores = [int(v) for v in game.scores]
            action_summary = self._helper._action_summary(action, game)
            if not game.apply_action_code(entry.action_code, False):
                break
            line.append(
                {
                    "ply": ply,
                    "player": before_player,
                    "action": action_summary,
                    "scores_before": before_scores,
                    "scores_after": [int(v) for v in game.scores],
                    "node_winner": entry.winner,
                    "node_reason": entry.reason,
                    "legal_response_count": entry.action_count,
                }
            )
        return line

    def _cpp_principal_line(self, raw_line: Sequence[Dict[str, Any]]) -> List[Dict[str, Any]]:
        if self._root_game is None:
            return []
        game = self._root_game.clone()
        line: List[Dict[str, Any]] = []
        for ply, raw_entry in enumerate(raw_line):
            action_code = int(raw_entry["action_code"])
            action = cs.Action.unpack(action_code)
            before_player = int(game.board.current_player)
            before_scores = [int(v) for v in game.scores]
            action_summary = self._helper._action_summary(action, game)
            if not game.apply_action_code(action_code, False):
                break
            line.append(
                {
                    "ply": ply,
                    "player": before_player,
                    "action": action_summary,
                    "scores_before": before_scores,
                    "scores_after": [int(v) for v in game.scores],
                    "node_winner": int(raw_entry["winner"]),
                    "node_reason": str(raw_entry["reason"]),
                    "legal_response_count": int(raw_entry["action_count"]),
                }
            )
        return line


def solve_visible_only_winner(
    game: cs.Game,
    options: Optional[SolverOptions] = None,
) -> SearchResult:
    return VisibleOnlyWinnerSolver(options).solve(game)


def solve_reveal_verified_mate(
    game: cs.Game,
    attacker: int,
    options: Optional[SolverOptions] = None,
    *,
    include_proof_dag: bool = False,
    proof_dag_node_limit: int = 100000,
    proof_dag_edge_limit: int = 500000,
    proof_dag_format: str = "v1",
) -> SearchResult:
    options = options or SolverOptions()
    if proof_dag_format not in {"v1", "compact", "both"}:
        raise ValueError("proof_dag_format must be 'v1', 'compact', or 'both'")
    if attacker != int(game.board.current_player):
        raise ValueError("reveal-verified mate requires attacker to be the current player")
    if not hasattr(cs, "solve_reveal_verified_mate_cpp"):
        raise RuntimeError("reveal-verified mate requires the C++ solver extension")

    start_time = time.monotonic()
    candidate = solve_visible_only_winner(
        game,
        options=SolverOptions(
            max_nodes=options.max_nodes,
            time_limit=options.time_limit,
            include_proof=True,
            allow_deck_reserve=options.allow_deck_reserve,
            use_memo=options.use_memo,
            jobs=1,
        ),
    )
    expected_status = PLAYER0_WIN if attacker == 0 else PLAYER1_WIN
    candidate_tree = candidate.proof_tree or {}
    candidate_depth = candidate_tree.get("forced_win_depth")
    if candidate.status != expected_status or candidate_depth is None:
        stats = RevealVerifiedStats(
            nodes=int(candidate.stats.nodes),
            memo_hits=int(candidate.stats.memo_hits),
            terminal_nodes=int(candidate.stats.terminal_nodes),
            legal_moves=int(candidate.stats.legal_moves),
            elapsed_ms=(time.monotonic() - start_time) * 1000.0,
            unknown_reason="visible-only candidate mate was not found",
            candidate_nodes=int(candidate.stats.nodes),
            candidate_elapsed_ms=float(candidate.stats.elapsed_ms),
        )
        refutation = {
            "mode": "reveal_verified_mate",
            "reason": "visible_only_candidate_mate_not_found",
            "candidate": candidate.to_dict(),
        }
        return SearchResult(UNKNOWN, None, None, refutation, stats)

    elapsed = time.monotonic() - start_time
    remaining_time = max(0.001, float(options.time_limit) - elapsed) if options.time_limit else 0.0
    remaining_nodes = (
        max(1, int(options.max_nodes) - int(candidate.stats.nodes))
        if options.max_nodes
        else 0
    )
    preferred_attacker_actions = [
        int(entry["action"]["pack"])
        for entry in candidate_tree.get("line", [])
        if int(entry["player"]) == attacker
    ]

    def run_full_verification(
        verification_nodes: int,
        verification_time: float,
    ) -> Dict[str, Any]:
        if int(options.jobs) != 1 and not include_proof_dag:
            depth_search = cs.search_reveal_verified_mate_anytime(
                game,
                attacker=int(attacker),
                min_depth=int(candidate_depth),
                max_depth=int(candidate_depth),
                max_nodes=max(0, int(verification_nodes)),
                time_limit_seconds=max(0.0, float(verification_time)),
                preferred_attacker_actions=preferred_attacker_actions,
                jobs=int(options.jobs),
            )
            attempts = list(depth_search["attempts"])
            if attempts:
                attempt = dict(attempts[-1])
                proven = attempt.get("status") == "mate"
                return {
                    "proven": proven,
                    "reason": str(attempt["reason"]),
                    "unknown_reason": (
                        None
                        if proven
                        else str(
                            attempt.get("unknown_reason")
                            or "parallel positive mate proof was not found"
                        )
                    ),
                    "memoized_states": 0,
                    "stats": dict(attempt["stats"]),
                    "line": [dict(entry) for entry in attempt.get("line", [])],
                    "proof_dag": {},
                }
            return {
                "proven": False,
                "reason": str(depth_search["stop_reason"]),
                "unknown_reason": (
                    None
                    if bool(depth_search["permanent_no_mate_proven"])
                    else str(depth_search["stop_reason"])
                ),
                "memoized_states": 0,
                "stats": dict(depth_search["stats"]),
                "line": [],
                "proof_dag": {},
            }
        return cs.solve_reveal_verified_mate_cpp(
            game,
            attacker=int(attacker),
            depth=int(candidate_depth),
            max_nodes=verification_nodes,
            time_limit_seconds=verification_time,
            preferred_attacker_actions=preferred_attacker_actions,
            include_proof_dag=bool(include_proof_dag),
            proof_dag_node_limit=max(0, int(proof_dag_node_limit)),
            proof_dag_edge_limit=max(0, int(proof_dag_edge_limit)),
            proof_dag_format=(
                "compact"
                if include_proof_dag and proof_dag_format == "compact"
                else "v1"
            ),
        )

    strict_verification_stats = None
    used_strict_result = False
    if (
        preferred_attacker_actions
        and remaining_time > 0.0
        and remaining_nodes != 1
        and not include_proof_dag
    ):
        strict_time = min(remaining_time, max(0.001, remaining_time * 0.25))
        strict_nodes = (
            max(1, int(remaining_nodes * 0.25))
            if remaining_nodes
            else 0
        )
        strict_start = time.monotonic()
        raw = None
        strict_verification_stats = {}
        for strict_prefix in range(len(preferred_attacker_actions), 0, -1):
            elapsed_strict = time.monotonic() - strict_start
            if elapsed_strict >= strict_time:
                break
            strict_remaining_time = max(0.001, strict_time - elapsed_strict)
            strict_raw = cs.solve_reveal_verified_mate_cpp(
                game,
                attacker=int(attacker),
                depth=int(candidate_depth),
                max_nodes=strict_nodes,
                time_limit_seconds=strict_remaining_time,
                preferred_attacker_actions=preferred_attacker_actions,
                include_proof_dag=bool(include_proof_dag),
                proof_dag_node_limit=max(0, int(proof_dag_node_limit)),
                proof_dag_edge_limit=max(0, int(proof_dag_edge_limit)),
                strict_preferred_attacker_prefix=int(strict_prefix),
                proof_dag_format="v1",
            )
            for key, value in dict(strict_raw["stats"]).items():
                if isinstance(value, (int, float)):
                    strict_verification_stats[key] = (
                        strict_verification_stats.get(key, 0) + value
                    )
            if bool(strict_raw["proven"]):
                raw = strict_raw
                used_strict_result = True
                break

        if raw is None:
            elapsed = time.monotonic() - start_time
            remaining_time = (
                max(0.001, float(options.time_limit) - elapsed)
                if options.time_limit
                else 0.0
            )
            remaining_nodes = (
                max(
                    1,
                    int(options.max_nodes)
                    - int(candidate.stats.nodes)
                    - int(strict_verification_stats.get("nodes", 0)),
                )
                if options.max_nodes
                else 0
            )
            raw = run_full_verification(remaining_nodes, remaining_time)
    else:
        raw = run_full_verification(remaining_nodes, remaining_time)
    verification_stats = raw["stats"]
    if strict_verification_stats is not None and not used_strict_result:
        verification_stats = dict(verification_stats)
        for key, value in strict_verification_stats.items():
            if isinstance(value, (int, float)) and key in verification_stats:
                verification_stats[key] += value
    unknown_reason = raw["unknown_reason"]
    if not raw["proven"] and unknown_reason is None:
        unknown_reason = "visible-only candidate mate has a reveal counterexample"
    stats = RevealVerifiedStats(
        nodes=int(candidate.stats.nodes) + int(verification_stats["nodes"]),
        memo_hits=int(candidate.stats.memo_hits) + int(verification_stats["memo_hits"]),
        terminal_nodes=int(candidate.stats.terminal_nodes) + int(verification_stats["terminal_nodes"]),
        reveal_branches=int(verification_stats["reveal_branches"]),
        legal_moves=int(candidate.stats.legal_moves) + int(verification_stats["legal_moves"]),
        elapsed_ms=(time.monotonic() - start_time) * 1000.0,
        max_depth_reached=int(candidate_depth),
        unknown_reason=unknown_reason,
        candidate_nodes=int(candidate.stats.nodes),
        candidate_elapsed_ms=float(candidate.stats.elapsed_ms),
        verification_nodes=int(verification_stats["nodes"]),
        verification_elapsed_ms=float(verification_stats["elapsed_ms"]),
        final_round_reveal_collapses=int(verification_stats["final_round_reveal_collapses"]),
        final_round_score_prunes=int(verification_stats["final_round_score_prunes"]),
        final_round_direct_resolutions=int(verification_stats["final_round_direct_resolutions"]),
        oracle_purchase_actions=int(verification_stats["oracle_purchase_actions"]),
        oracle_reserve_actions=int(verification_stats["oracle_reserve_actions"]),
        deck_reserve_candidates=int(verification_stats["deck_reserve_candidates"]),
        deck_reserve_branches=int(verification_stats["deck_reserve_branches"]),
    )
    verification = {
        "all_reveals_verified": bool(raw["proven"]),
        "reason": str(raw["reason"]),
        "memoized_states": int(raw["memoized_states"]),
        "stats": dict(verification_stats),
        "line": _reveal_verified_line(game, raw["line"]),
    }
    if include_proof_dag:
        proof_dag = dict(raw["proof_dag"])
        if proof_dag.get("format") == "strategy_dag_compact_v1":
            proof_dag.setdefault("semantics", _dfpn_output._strategy_dag_semantics())
            verification["proof_dag"] = proof_dag
        else:
            proof_dag["format"] = "strategy_dag_v1"
            proof_dag["semantics"] = _dfpn_output._strategy_dag_semantics()
            if proof_dag_format == "compact":
                verification["proof_dag"] = proof_dag_to_compact(proof_dag)
            elif proof_dag_format == "both":
                compact_dag = proof_dag_to_compact(proof_dag)
                verification["proof_dag"] = proof_dag
                verification["proof_dag_compact"] = compact_dag
                verification["proof_dag_size"] = strategy_dag_size_report(
                    proof_dag,
                    compact_dag,
                )
            else:
                verification["proof_dag"] = proof_dag
    proof = {
        "mode": "reveal_verified_mate",
        "attacker": int(attacker),
        "forced_win_depth": int(candidate_depth),
        "assumptions": {
            "hidden_decks_ignored_during_candidate_search": True,
            "all_defender_responses_verified": True,
            "all_reveal_shapes_verified": True,
            "reserve_deck": "all_post_root_draws_verified",
            "attacker_candidate_policy": "heuristic_subset_for_bounded_proof",
            "candidate_purchase_payments": candidate_tree["assumptions"][
                "purchase_payments"
            ],
            "hidden_reveal_verification": "explicit_legal_reveal_branching",
            "reveal_identity_abstraction": "concrete_card_id_preserved_in_strategy_dag",
            "public_card_purchase_payments_during_verification": (
                "canonical_minimal_gold_only"
                if bool(game.simple_payment_mode)
                else "all_legal_patterns"
            ),
            "hidden_card_purchase_payments_during_verification": "reserved_cards_only",
        },
        "candidate": {
            "mode": "visible_only_winner",
            "elapsed_ms": float(candidate.stats.elapsed_ms),
            "nodes": int(candidate.stats.nodes),
            "line": candidate_tree.get("line"),
        },
        "verification": verification,
        "line": candidate_tree.get("line"),
    }
    if raw["proven"]:
        return SearchResult(
            MATE,
            int(candidate_depth),
            proof if options.include_proof else None,
            None,
            stats,
        )
    refutation = {
        "mode": "reveal_verified_mate",
        "candidate": proof["candidate"],
        "verification": verification,
    }
    return SearchResult(UNKNOWN, None, None, refutation, stats)


def _reveal_verified_line(
    root_game: cs.Game,
    raw_line: Sequence[Dict[str, Any]],
) -> List[Dict[str, Any]]:
    game = root_game.clone()
    helper = MateSolver(attacker=int(game.board.current_player), max_depth=0)
    line: List[Dict[str, Any]] = []
    for ply, raw_entry in enumerate(raw_line):
        action_code = int(raw_entry["action_code"])
        action = cs.Action.unpack(action_code)
        reveal_card = raw_entry["reveal_card"]
        before_player = int(game.board.current_player)
        before_scores = [int(v) for v in game.scores]
        action_summary = helper._action_summary(action, game)
        if reveal_card is not None:
            if int(action.type) == int(cs.ActionType.RESERVE_DECK):
                level = int(action.deck_level)
            else:
                level = int(cs.get_card(int(action.card_id)).level) - 1
            decks = [[int(card_id) for card_id in deck] for deck in game.board.decks]
            if int(reveal_card) in decks[level]:
                decks[level].remove(int(reveal_card))
            elif decks[level]:
                decks[level].pop()
            decks[level].append(int(reveal_card))
            game.board.decks = decks
        if not game.apply_action_code(action_code, False):
            break
        line.append(
            {
                "ply": ply,
                "player": before_player,
                "action": action_summary,
                "reveal_card": reveal_card,
                "scores_before": before_scores,
                "scores_after": [int(v) for v in game.scores],
                "legal_response_count": int(raw_entry["action_count"]),
            }
        )
    return line


def main(argv: Optional[Sequence[str]] = None) -> int:
    """Run the CLI (scripts.dfpn_cli) with this module as its API."""
    return _dfpn_cli_main(argv, api=sys.modules[__name__])


if __name__ == "__main__":
    raise SystemExit(main())
