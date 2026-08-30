"""Solution uniqueness and countermate validation for generated puzzles."""

from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
import os
import time
from typing import Any, Callable, Optional

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi
from scripts.mate_solver import MATE, SearchResult, SolverOptions


def find_verified_winning_actions(
    game: cs.Game,
    *,
    attacker: int,
    depth: int,
    max_depth: Optional[int] = None,
    max_winning_actions: int,
    node_limit: int,
    time_limit: float,
    jobs: int = 1,
    positive_time_limit: float = 0.0,
) -> dict[str, object]:
    if jobs < 0:
        raise ValueError("jobs must be non-negative")
    if positive_time_limit < 0:
        raise ValueError("positive_time_limit must be non-negative")
    effective_jobs = max(1, (os.cpu_count() or 1) if jobs == 0 else jobs)
    depth_ceiling = max(depth, depth if max_depth is None else int(max_depth))
    actions = list(game.legal_actions)

    def check_action(action: cs.Action) -> dict[str, object]:
        started = time.monotonic()
        usi = action_to_usi(action, game=game)
        positive_attempts: list[dict[str, object]] = []
        positive_nodes = 0
        positive_stop_reason: Optional[str] = None
        if positive_time_limit > 0.0:
            positive_budget = positive_time_limit
            if time_limit > 0.0:
                positive_budget = min(positive_budget, time_limit)
            positive = cs.search_reveal_verified_mate_anytime(
                game,
                attacker=attacker,
                min_depth=depth,
                max_depth=depth_ceiling,
                max_nodes=max(0, node_limit),
                time_limit_seconds=positive_budget,
                required_root_action=int(action.pack()),
                jobs=1,
            )
            positive_nodes = int(positive["stats"].get("nodes", 0))
            positive_stop_reason = str(positive["stop_reason"])
            positive_attempts = [
                {
                    "depth": int(attempt["depth"]),
                    "status": str(attempt["status"]),
                    "phase": "positive_proof",
                    "nodes": int(attempt["stats"].get("nodes", 0)),
                    "elapsed_ms": float(
                        attempt["stats"].get("elapsed_ms", 0.0)
                    ),
                    "unknown_reason": attempt.get("unknown_reason"),
                }
                for attempt in positive["attempts"]
            ]
            if positive["status"] == "mate":
                return {
                    "action": usi,
                    "status": "mate",
                    "mate_depth": positive["mate_depth"],
                    "mate_depth_minimal": False,
                    "verified_no_mate_through_depth": None,
                    "permanent_no_mate_proven": False,
                    "permanent_no_mate_certificate": None,
                    "stop_reason": positive_stop_reason,
                    "positive_proof_attempted": True,
                    "positive_stop_reason": positive_stop_reason,
                    "depth_attempts": positive_attempts,
                }
            if positive["status"] == "permanent_no_mate":
                return {
                    "action": usi,
                    "status": "permanent_no_mate",
                    "mate_depth": None,
                    "mate_depth_minimal": False,
                    "verified_no_mate_through_depth": depth_ceiling,
                    "permanent_no_mate_proven": True,
                    "permanent_no_mate_certificate": positive.get(
                        "permanent_no_mate_certificate"
                    ),
                    "stop_reason": positive_stop_reason,
                    "positive_proof_attempted": True,
                    "positive_stop_reason": positive_stop_reason,
                    "depth_attempts": positive_attempts,
                }

        remaining_nodes = (
            0 if node_limit == 0 else max(0, node_limit - positive_nodes)
        )
        remaining_time = (
            0.0
            if time_limit == 0.0
            else max(0.0, time_limit - (time.monotonic() - started))
        )
        budget_exhausted = (
            node_limit > 0 and remaining_nodes == 0
        ) or (time_limit > 0.0 and remaining_time == 0.0)
        if budget_exhausted:
            return {
                "action": usi,
                "status": "unknown",
                "mate_depth": None,
                "mate_depth_minimal": False,
                "verified_no_mate_through_depth": None,
                "permanent_no_mate_proven": False,
                "permanent_no_mate_certificate": None,
                "stop_reason": "uniqueness budget exhausted after positive proof search",
                "positive_proof_attempted": positive_time_limit > 0.0,
                "positive_stop_reason": positive_stop_reason,
                "depth_attempts": positive_attempts,
            }

        search = cs.search_reveal_verified_mate_depths(
            game,
            attacker=attacker,
            min_depth=depth,
            max_depth=depth_ceiling,
            max_nodes=remaining_nodes,
            time_limit_seconds=remaining_time,
            required_root_action=int(action.pack()),
            jobs=1,
        )
        attempts = list(search["attempts"])
        return {
            "action": usi,
            "status": str(search["status"]),
            "mate_depth": search["mate_depth"],
            "mate_depth_minimal": bool(search["status"] == "mate"),
            "verified_no_mate_through_depth": search[
                "verified_no_mate_through_depth"
            ],
            "permanent_no_mate_proven": bool(
                search.get("permanent_no_mate_proven", False)
            ),
            "permanent_no_mate_certificate": search.get(
                "permanent_no_mate_certificate"
            ),
            "stop_reason": str(search["stop_reason"]),
            "positive_proof_attempted": positive_time_limit > 0.0,
            "positive_stop_reason": positive_stop_reason,
            "depth_attempts": positive_attempts + [
                {
                    "depth": int(attempt["depth"]),
                    "status": str(attempt["status"]),
                    "phase": "exact_refutation",
                    "nodes": int(attempt["stats"].get("nodes", 0)),
                    "elapsed_ms": float(attempt["stats"].get("elapsed_ms", 0.0)),
                    "unknown_reason": attempt["unknown_reason"],
                }
                for attempt in attempts
            ],
        }

    action_results: list[dict[str, object]] = []
    if effective_jobs > 1 and len(actions) > 1:
        with ThreadPoolExecutor(
            max_workers=min(effective_jobs, len(actions)),
            thread_name_prefix="csplendor-uniqueness",
        ) as executor:
            action_results = list(executor.map(check_action, actions))
    else:
        for action in actions:
            result_summary = check_action(action)
            action_results.append(result_summary)
            winning_so_far = sum(
                result["status"] == "mate" for result in action_results
            )
            if winning_so_far > max_winning_actions:
                break

    winning_actions: list[str] = []
    winning_action_depths: list[dict[str, object]] = []
    unknown_actions: list[str] = []
    for result_summary in action_results:
        usi = str(result_summary["action"])
        if result_summary["status"] == "mate":
            winning_actions.append(usi)
            winning_action_depths.append(
                {"action": usi, "depth": int(result_summary["mate_depth"])}
            )
        elif result_summary["status"] == "unknown":
            unknown_actions.append(usi)
    truncated = len(winning_actions) > max_winning_actions
    return {
        "checks": len(action_results),
        "depth_checks": sum(
            len(result["depth_attempts"]) for result in action_results
        ),
        "jobs": min(effective_jobs, max(1, len(actions))),
        "positive_time_limit": positive_time_limit,
        "min_depth": depth,
        "max_depth": depth_ceiling,
        "winning_actions": winning_actions,
        "winning_action_depths": winning_action_depths,
        "unknown_actions": unknown_actions,
        "action_results": action_results,
        "complete": not unknown_actions and not truncated,
        "truncated": truncated,
    }


def completed_turn_children(
    game: cs.Game,
    action: cs.Action,
) -> list[cs.Game]:
    child = game.clone_light()
    if not child.apply(action, False):
        raise RuntimeError("engine rejected a generated legal action")
    if child.is_game_over() or int(child.board.current_player) != int(
        game.board.current_player
    ):
        return [child]
    if not bool(child.board.waiting_noble):
        return []

    children = []
    for noble_action in child.legal_actions:
        noble_child = child.clone_light()
        if not noble_child.apply(noble_action, False):
            raise RuntimeError("engine rejected a generated noble choice")
        children.append(noble_child)
    return children


def find_visible_winning_actions(
    game: cs.Game,
    args: Any,
    *,
    max_winning_actions: int,
    arg_value: Callable[[Any, str, object], object],
    visible_only_prefilter: Callable[[cs.Game, Any], SearchResult],
    player0_win: str,
    player1_win: str,
) -> list[str]:
    expected = player0_win if int(game.board.current_player) == 0 else player1_win
    winning_actions: list[str] = []
    action_limit = int(arg_value(args, "visible_uniqueness_action_limit", 16))
    actions = list(game.legal_actions)
    if action_limit:
        actions = actions[:action_limit]
    for action in actions:
        for child in completed_turn_children(game, action):
            if child.is_game_over():
                winner = int(child.board.winner)
                status = (
                    player0_win if winner == 0 else player1_win if winner == 1 else None
                )
            else:
                status = visible_only_prefilter(child, args).status
            if status != expected:
                continue
            winning_actions.append(action_to_usi(action, game=game))
            break
        if len(winning_actions) > max_winning_actions:
            break
    return winning_actions


def find_countermate_blunders(
    game: cs.Game,
    result: SearchResult,
    *,
    min_losing_alternatives: int,
    action_limit: int,
    node_limit: int,
    time_limit: float,
    jobs: int = 1,
    solve_reveal_verified_mate: Callable[..., SearchResult],
    progress: Optional[Any] = None,
    attempt: Optional[int] = None,
) -> tuple[list[dict[str, object]], int]:
    if min_losing_alternatives <= 0:
        return [], 0

    proof = result.proof_tree or {}
    verification = proof.get("verification")
    line = verification.get("line") if isinstance(verification, dict) else None
    if not isinstance(line, list) or not line:
        line = proof.get("line")
    if not isinstance(line, list) or not line:
        return [], 0
    action_info = line[0].get("action")
    if not isinstance(action_info, dict) or "pack" not in action_info:
        return [], 0

    correct_pack = int(action_info["pack"])
    attacker = int(game.board.current_player)
    defender = 1 - attacker
    alternatives = [
        action for action in game.legal_actions if int(action.pack()) != correct_pack
    ]
    if action_limit:
        alternatives = alternatives[:action_limit]

    blunders = []
    checks = 0
    for alternative_index, action in enumerate(alternatives, start=1):
        for child in completed_turn_children(game, action):
            if child.is_game_over() or int(child.board.current_player) != defender:
                continue
            checks += 1
            if progress is not None:
                progress.emit(
                    "countermate_search",
                    force=True,
                    attempt=attempt,
                    alternative=(f"{alternative_index}/{len(alternatives)}"),
                    check=checks,
                )
            countermate = solve_reveal_verified_mate(
                child,
                attacker=defender,
                options=SolverOptions(
                    max_nodes=node_limit,
                    time_limit=time_limit,
                    include_proof=True,
                    allow_deck_reserve=True,
                    jobs=jobs,
                ),
            )
            if countermate.status != MATE:
                continue
            blunders.append(
                {
                    "action": action_to_usi(action, game=game),
                    "opponent": defender,
                    "forced_win_depth": int(countermate.depth),
                }
            )
            break
        if len(blunders) >= min_losing_alternatives:
            break
    return blunders, checks


__all__ = [
    "completed_turn_children",
    "find_countermate_blunders",
    "find_verified_winning_actions",
    "find_visible_winning_actions",
]
