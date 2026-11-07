"""Safe iterative deepening for reveal-verified mate searches."""

from __future__ import annotations

import time
from typing import Any, Optional

from ._csplendor import (
    Game,
    MateSearchCancellationToken,
    default_mate_jobs,
    get_card,
    get_noble,
    resolve_max_memo_states,
    resolve_search_table_caps,
    solve_reveal_verified_mate_cpp,
)
from .api.usi_kifu import action_to_usi

MATE_DEPTH_SEARCH_FORMAT = "csplendor_mate_depth_search_v1"
MATE_ANYTIME_SEARCH_FORMAT = "csplendor_mate_anytime_search_v1"
_WIN_SCORE = 15
_NO_REQUIRED_ACTION = (1 << 64) - 1


def _resolved_jobs(jobs: int) -> int:
    """Threads of the native searches: 0 is ``default_mate_jobs()``."""
    return default_mate_jobs() if jobs == 0 else jobs


def _native_status(raw: dict[str, Any]) -> str:
    if bool(raw["proven"]):
        return "mate"
    if raw["unknown_reason"] is not None:
        return "unknown"
    return "no_mate"


def _permanent_no_mate_certificate(
    game: Game, attacker: int
) -> Optional[dict[str, Any]]:
    """Return a cheap, sound certificate when the attacker can never win."""
    if game.is_game_over():
        winner = int(game.board.winner)
        if winner != attacker:
            return {
                "kind": "terminal_non_attacker_result",
                "winner": winner,
            }
        return None

    board = game.board
    attacker_state = board.get_player(attacker)
    card_ids = [
        int(card_id)
        for level in board.visible
        for card_id in level
        if int(card_id) >= 0
    ]
    card_ids.extend(
        int(card_id)
        for level in board.decks
        for card_id in level
        if int(card_id) >= 0
    )
    # An opponent's reserved cards can never return to the market.  The
    # attacker's own reserved cards remain possible future purchases.
    card_ids.extend(
        int(card_id)
        for card_id in attacker_state.reserved
        if int(card_id) >= 0
    )
    card_points = sum(int(get_card(card_id).points) for card_id in card_ids)
    noble_points = sum(
        int(get_noble(int(noble_id)).points) for noble_id in board.nobles
    )
    current_score = int(attacker_state.points)
    score_ceiling = current_score + card_points + noble_points
    if score_ceiling >= _WIN_SCORE:
        return None
    return {
        "kind": "attacker_score_ceiling_below_win_threshold",
        "current_score": current_score,
        "maximum_additional_card_points": card_points,
        "maximum_additional_noble_points": noble_points,
        "score_ceiling": score_ceiling,
        "win_threshold": _WIN_SCORE,
    }


def _remaining_node_budget(limit: int, used: int) -> int:
    if limit == 0:
        return 0
    return max(0, limit - used)


def _remaining_time_budget(limit: float, started: float) -> float:
    if limit == 0.0:
        return 0.0
    return max(0.0, limit - (time.monotonic() - started))


def search_reveal_verified_mate_depths(
    game: Game,
    *,
    attacker: int,
    min_depth: int,
    max_depth: int,
    max_nodes: int = 0,
    time_limit_seconds: float = 0.0,
    required_root_action: Optional[int] = None,
    preferred_attacker_actions: Optional[list[int]] = None,
    jobs: int = 1,
    cancellation_token: Optional[MateSearchCancellationToken] = None,
    include_proof_dag: bool = False,
    proof_dag_node_limit: int = 100_000,
    proof_dag_edge_limit: int = 500_000,
    proof_dag_format: str = "compact",
    order_hints: Any = None,
    line_transplant: bool = True,
    no_take_probe: bool = False,
    max_memo_states: int = 0,
    algorithm: str = "dfs",
    dfpn_table_bytes: int = 0,
) -> dict[str, Any]:
    """Search consecutive mate depths until a sound stopping condition.

    ``order_hints`` (a ``MateOrderHints``) orders moves at known positions.
    With ``attacker_restrict_top_k > 0`` on the hints, attacker nodes at known
    positions try only their first k hinted actions: a proof found this way
    is still sound, but a failed depth is reported as ``inconclusive`` and
    the sweep continues to the next depth (``exact`` is then False).

    ``max_nodes`` and ``time_limit_seconds`` are cumulative budgets for the
    complete depth range, not fresh budgets for each depth.  A conclusive
    refutation at depth ``N`` advances immediately to ``N + 1``.  A mate,
    an indeterminate search-limit result, or ``max_depth`` stops the sweep.

    Refutation is monotone only towards *smaller* bounds: proving no mate at
    depth ``N`` says nothing about ``N + 1``.  Consequently this function
    never extrapolates permanent no-mate from repeated bounded refutations.
    It may stop before searching only when it can emit a separate, unbounded
    certificate (a terminal non-attacker result or a score ceiling below 15).

    ``algorithm`` selects the native search of every depth (``"dfs"`` or
    ``"dfpn"``, doc/mate_dfpn_cpp_design.md); verdicts are the same.
    ``dfpn_table_bytes`` (0 = a quarter of the memory budget) is the df-pn
    table of every native search.  ``jobs`` is the threads of every native
    depth-first search (doc/mate_usage.md「並列の詰み探索」): 1 is the serial
    search, 0 is ``default_mate_jobs()``; the verdicts do not depend on it.
    """
    if algorithm not in {"dfs", "dfpn"}:
        raise ValueError("algorithm must be 'dfs' or 'dfpn'")
    if attacker not in (0, 1):
        raise ValueError("attacker must be 0 or 1")
    if min_depth < 0 or max_depth < min_depth:
        raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
    if max_nodes < 0 or time_limit_seconds < 0:
        raise ValueError("search limits must be non-negative")
    if jobs < 0:
        raise ValueError("jobs must be non-negative")
    if proof_dag_node_limit < 0 or proof_dag_edge_limit < 0:
        raise ValueError("proof DAG limits must be non-negative")
    if proof_dag_format not in {"v1", "compact"}:
        raise ValueError("proof_dag_format must be 'v1' or 'compact'")

    required_code: Optional[int]
    required_usi: Optional[str]
    if required_root_action is None:
        required_code = None
        required_usi = None
    else:
        if int(game.current_player) != attacker:
            raise ValueError("required_root_action requires attacker to move")
        required_code = int(required_root_action)
        legal_by_code = {
            int(action.pack()): action for action in game.legal_actions
        }
        if required_code not in legal_by_code:
            raise ValueError("required_root_action must be legal in the root state")
        required_usi = action_to_usi(legal_by_code[required_code], game=game)

    preferred_codes = [
        int(action_code) for action_code in (preferred_attacker_actions or [])
    ]
    restricted_probe = bool(
        order_hints is not None
        and int(getattr(order_hints, "attacker_restrict_top_k", 0)) > 0
    ) or bool(no_take_probe)
    if restricted_probe and include_proof_dag:
        raise ValueError("a restricted probe cannot build a proof DAG")
    # 0 = from the free memory, fixed once for every depth; df-pn
    # takes its table out of the same budget (doc/mate_usage.md「探索のメモリ予算」).
    max_memo_states, dfpn_table_bytes = resolve_search_table_caps(
        max_memo_states, dfpn_table_bytes, algorithm=algorithm
    )
    started = time.monotonic()
    permanent_certificate = _permanent_no_mate_certificate(game, attacker)
    if permanent_certificate is not None:
        return {
            "format": MATE_DEPTH_SEARCH_FORMAT,
            "status": "permanent_no_mate",
            "stop_reason": str(permanent_certificate["kind"]),
            "attacker": attacker,
            "min_depth": min_depth,
            "max_depth": max_depth,
            "mate_depth": None,
            "winning_root_action": None,
            "winning_root_action_usi": None,
            "last_completed_depth": None,
            "verified_no_mate_through_depth": max_depth,
            "minimal_within_range": False,
            "permanent_no_mate_proven": True,
            "permanent_no_mate_certificate": permanent_certificate,
            "jobs": _resolved_jobs(jobs),
            "exact": True,
            "required_root_action": required_code,
            "required_root_action_usi": required_usi,
            "attempts": [],
            "stats": {
                "nodes": 0,
                "elapsed_ms": (time.monotonic() - started) * 1000.0,
            },
        }
    attempts: list[dict[str, Any]] = []
    total_nodes = 0
    total_stats: dict[str, int | float] = {}
    last_completed_depth: Optional[int] = None
    last_refuted_depth: Optional[int] = None
    mate_depth: Optional[int] = None
    stop_reason = "max_depth_reached"
    status = "no_mate_within_max_depth"

    for depth in range(min_depth, max_depth + 1):
        remaining_nodes = _remaining_node_budget(max_nodes, total_nodes)
        if max_nodes and remaining_nodes == 0:
            status = "unknown"
            stop_reason = "cumulative node limit exceeded"
            break
        remaining_time = _remaining_time_budget(time_limit_seconds, started)
        if time_limit_seconds and remaining_time <= 0.0:
            status = "unknown"
            stop_reason = "cumulative time limit exceeded"
            break

        native = solve_reveal_verified_mate_cpp(
            game,
            attacker=attacker,
            depth=depth,
            max_nodes=remaining_nodes,
            time_limit_seconds=remaining_time,
            preferred_attacker_actions=preferred_codes,
            include_proof_dag=include_proof_dag,
            proof_dag_node_limit=proof_dag_node_limit,
            proof_dag_edge_limit=proof_dag_edge_limit,
            required_root_action=(
                _NO_REQUIRED_ACTION if required_code is None else required_code
            ),
            proof_dag_format=proof_dag_format,
            exhaustive_attacker_actions=not restricted_probe,
            exact_reveal_search=True,
            cancellation_token=cancellation_token,
            order_hints=order_hints,
            line_transplant=line_transplant,
            no_take_probe=no_take_probe,
            max_memo_states=max_memo_states,
            algorithm=algorithm,
            dfpn_table_bytes=dfpn_table_bytes,
            jobs=jobs,
        )
        native_stats = dict(native["stats"])
        nodes = int(native_stats.get("nodes", 0))
        total_nodes += nodes
        for name, value in native_stats.items():
            if name == "elapsed_ms" or not isinstance(value, (int, float)):
                continue
            total_stats[name] = total_stats.get(name, 0) + value

        unknown_reason = native["unknown_reason"]
        proven = bool(native["proven"])
        attempt_status = (
            "mate" if proven else "unknown" if unknown_reason is not None else "no_mate"
        )
        attempt: dict[str, Any] = {
            "depth": depth,
            "status": attempt_status,
            "proven": proven,
            "reason": str(native["reason"]),
            "unknown_reason": unknown_reason,
            "memoized_states": int(native["memoized_states"]),
            "stats": native_stats,
            "line": [dict(entry) for entry in native["line"]],
        }
        if proven and include_proof_dag:
            attempt["proof_dag"] = dict(native["proof_dag"])
        attempt["jobs"] = int(native["jobs"])
        attempts.append(attempt)

        if proven:
            mate_depth = depth
            last_completed_depth = depth
            status = "mate"
            stop_reason = "mate_proven"
            break
        if restricted_probe:
            # Only the hinted attacker moves were tried: nothing is refuted.
            attempt["status"] = "inconclusive"
            status = "unknown"
            stop_reason = "restricted_probe_exhausted"
            if unknown_reason is not None and (
                "limit" in str(unknown_reason) or "cancel" in str(unknown_reason)
            ):
                stop_reason = str(unknown_reason)
                break
            continue
        if unknown_reason is not None:
            status = "unknown"
            stop_reason = str(unknown_reason)
            break

        # This depth is conclusively refuted.  Only now is N++ sound.
        last_completed_depth = depth
        last_refuted_depth = depth

    elapsed_ms = (time.monotonic() - started) * 1000.0
    total_stats["nodes"] = total_nodes
    total_stats["elapsed_ms"] = elapsed_ms
    winning_root_action = None
    winning_root_action_usi = None
    if mate_depth is not None and attempts and attempts[-1]["line"]:
        winning_root_action = int(attempts[-1]["line"][0]["action_code"])
        legal_by_code = {
            int(action.pack()): action for action in game.legal_actions
        }
        winning_action = legal_by_code.get(winning_root_action)
        if winning_action is not None:
            winning_root_action_usi = action_to_usi(winning_action, game=game)
    return {
        "format": MATE_DEPTH_SEARCH_FORMAT,
        "status": status,
        "stop_reason": stop_reason,
        "attacker": attacker,
        "min_depth": min_depth,
        "max_depth": max_depth,
        "mate_depth": mate_depth,
        "winning_root_action": winning_root_action,
        "winning_root_action_usi": winning_root_action_usi,
        "last_completed_depth": last_completed_depth,
        "verified_no_mate_through_depth": last_refuted_depth,
        "minimal_within_range": mate_depth is not None,
        "permanent_no_mate_proven": False,
        "permanent_no_mate_certificate": None,
        "jobs": _resolved_jobs(jobs),
        "exact": not restricted_probe,
        "restricted_probe": restricted_probe,
        "required_root_action": required_code,
        "required_root_action_usi": required_usi,
        "attempts": attempts,
        "stats": total_stats,
    }


def search_reveal_verified_mate_anytime(
    game: Game,
    *,
    attacker: int,
    min_depth: int,
    max_depth: int,
    max_nodes: int = 0,
    time_limit_seconds: float = 0.0,
    required_root_action: Optional[int] = None,
    preferred_attacker_actions: Optional[list[int]] = None,
    jobs: int = 1,
    cancellation_token: Optional[MateSearchCancellationToken] = None,
    order_hints: Any = None,
    line_transplant: bool = True,
    no_take_probe: bool = False,
    max_memo_states: int = 0,
) -> dict[str, Any]:
    """Search progressively deeper for a sound mate without proving minimality.

    Each depth receives a share of the remaining budget.  Failure to prove a
    depth is treated as inconclusive and the next depth is attempted.  This is
    the latency-oriented mode for an AI: only a positive mate certificate is
    actionable, while a negative result never becomes a no-mate claim.
    """
    if attacker not in (0, 1):
        raise ValueError("attacker must be 0 or 1")
    if min_depth < 0 or max_depth < min_depth:
        raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
    if max_nodes < 0 or time_limit_seconds < 0 or jobs < 0:
        raise ValueError("search limits and jobs must be non-negative")
    required_code = None if required_root_action is None else int(required_root_action)
    legal_by_code = {int(action.pack()): action for action in game.legal_actions}
    if required_code is not None:
        if int(game.current_player) != attacker:
            raise ValueError("required_root_action requires attacker to move")
        if required_code not in legal_by_code:
            raise ValueError("required_root_action must be legal in the root state")
    max_memo_states = resolve_max_memo_states(max_memo_states)

    started = time.monotonic()
    global_deadline = (
        started + time_limit_seconds if time_limit_seconds > 0.0 else None
    )
    certificate = _permanent_no_mate_certificate(game, attacker)
    if certificate is not None:
        return {
            "format": MATE_ANYTIME_SEARCH_FORMAT,
            "status": "permanent_no_mate",
            "stop_reason": str(certificate["kind"]),
            "mate_depth": None,
            "winning_root_action": None,
            "winning_root_action_usi": None,
            "minimal_depth_proven": False,
            "permanent_no_mate_proven": True,
            "permanent_no_mate_certificate": certificate,
            "attempts": [],
            "stats": {"nodes": 0, "elapsed_ms": 0.0},
        }

    preferred_codes = [
        int(code) for code in (preferred_attacker_actions or [])
    ]
    attempts: list[dict[str, Any]] = []
    total_nodes = 0
    mate_depth: Optional[int] = None
    winning_action: Optional[int] = None
    stop_reason = "max_depth_reached_without_proof"

    for offset, depth in enumerate(range(min_depth, max_depth + 1)):
        attempts_left = max_depth - min_depth + 1 - offset
        remaining_nodes = 0 if max_nodes == 0 else max(0, max_nodes - total_nodes)
        if max_nodes and remaining_nodes == 0:
            stop_reason = "cumulative node limit exceeded"
            break
        remaining_time = (
            0.0
            if global_deadline is None
            else max(0.0, global_deadline - time.monotonic())
        )
        if global_deadline is not None and remaining_time <= 0.0:
            stop_reason = "cumulative time limit exceeded"
            break
        if cancellation_token is not None and cancellation_token.is_cancelled:
            stop_reason = "search cancelled"
            break

        attempt_nodes = (
            0
            if max_nodes == 0
            else max(1, remaining_nodes // attempts_left)
        )
        attempt_time = (
            0.0
            if global_deadline is None
            else remaining_time / attempts_left
        )
        raw = dict(
            solve_reveal_verified_mate_cpp(
                game,
                attacker=attacker,
                depth=depth,
                max_nodes=attempt_nodes,
                time_limit_seconds=attempt_time,
                preferred_attacker_actions=preferred_codes,
                required_root_action=(
                    _NO_REQUIRED_ACTION if required_code is None else required_code
                ),
                exhaustive_attacker_actions=False,
                exact_reveal_search=False,
                cancellation_token=cancellation_token,
                order_hints=order_hints,
                line_transplant=line_transplant,
                no_take_probe=no_take_probe,
                max_memo_states=max_memo_states,
                jobs=jobs,
            )
        )
        native_status = _native_status(raw)
        nodes = int(raw["stats"].get("nodes", 0))
        total_nodes += nodes
        attempt = {
            "depth": depth,
            "status": "mate" if native_status == "mate" else "inconclusive",
            "native_status": native_status,
            "reason": str(raw["reason"]),
            "unknown_reason": raw["unknown_reason"],
            "stats": dict(raw["stats"]),
            "line": [dict(entry) for entry in raw.get("line", [])],
        }
        attempt["jobs"] = int(raw["jobs"])
        attempts.append(attempt)
        if native_status == "mate":
            mate_depth = depth
            stop_reason = "mate_proven"
            if attempt["line"]:
                winning_action = int(attempt["line"][0]["action_code"])
            break
        if raw["unknown_reason"] == "search cancelled":
            stop_reason = "search cancelled"
            break

    winning_usi = None
    if winning_action in legal_by_code:
        winning_usi = action_to_usi(legal_by_code[winning_action], game=game)
    return {
        "format": MATE_ANYTIME_SEARCH_FORMAT,
        "status": "mate" if mate_depth is not None else "no_mate_found",
        "stop_reason": stop_reason,
        "attacker": attacker,
        "min_depth": min_depth,
        "max_depth": max_depth,
        "mate_depth": mate_depth,
        "winning_root_action": winning_action,
        "winning_root_action_usi": winning_usi,
        "minimal_depth_proven": False,
        "verified_no_mate_through_depth": None,
        "permanent_no_mate_proven": False,
        "permanent_no_mate_certificate": None,
        "jobs": _resolved_jobs(jobs),
        "attempts": attempts,
        "stats": {
            "nodes": total_nodes,
            "elapsed_ms": (time.monotonic() - started) * 1000.0,
        },
    }


__all__ = [
    "MATE_ANYTIME_SEARCH_FORMAT",
    "MATE_DEPTH_SEARCH_FORMAT",
    "search_reveal_verified_mate_anytime",
    "search_reveal_verified_mate_depths",
]
