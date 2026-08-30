"""Safe iterative deepening for reveal-verified mate searches."""

from __future__ import annotations

from concurrent.futures import CancelledError, ThreadPoolExecutor, as_completed
import os
import time
from typing import Any, Optional

from ._csplendor import (
    Game,
    MateSearchCancellationToken,
    get_card,
    get_noble,
    solve_reveal_verified_root_split_cpp,
    solve_reveal_verified_mate_cpp,
)
from .api.usi_kifu import action_to_usi


MATE_DEPTH_SEARCH_FORMAT = "csplendor_mate_depth_search_v1"
MATE_ANYTIME_SEARCH_FORMAT = "csplendor_mate_anytime_search_v1"
_WIN_SCORE = 15
_NO_REQUIRED_ACTION = (1 << 64) - 1


def _effective_jobs(jobs: int) -> int:
    if jobs == 0:
        return max(1, os.cpu_count() or 1)
    return max(1, jobs)


def _empty_native_stats() -> dict[str, int | float]:
    return {
        "nodes": 0,
        "memo_hits": 0,
        "persistent_memo_hits": 0,
        "iterative_order_hits": 0,
        "terminal_nodes": 0,
        "legal_moves": 0,
        "reveal_branches": 0,
        "final_round_reveal_collapses": 0,
        "final_round_score_prunes": 0,
        "final_round_direct_resolutions": 0,
        "oracle_purchase_actions": 0,
        "oracle_reserve_actions": 0,
        "deck_reserve_candidates": 0,
        "deck_reserve_branches": 0,
        "elapsed_ms": 0.0,
    }


def _unknown_native(depth: int, reason: str) -> dict[str, Any]:
    return {
        "proven": False,
        "attacker": -1,
        "depth": depth,
        "reason": reason,
        "unknown_reason": reason,
        "memoized_states": 0,
        "stats": _empty_native_stats(),
        "line": [],
        "proof_dag": {},
    }


def _native_status(raw: dict[str, Any]) -> str:
    if bool(raw["proven"]):
        return "mate"
    if raw["unknown_reason"] is not None:
        return "unknown"
    return "no_mate"


def _solve_parallel_branch(
    edge: dict[str, Any],
    *,
    attacker: int,
    max_nodes: int,
    deadline: Optional[float],
    preferred_attacker_actions: list[int],
    cancellation_token: MateSearchCancellationToken,
    exhaustive_attacker_actions: bool,
) -> dict[str, Any]:
    if cancellation_token.is_cancelled:
        return _unknown_native(int(edge["child_depth"]), "search cancelled")
    remaining_time = 0.0
    if deadline is not None:
        remaining_time = deadline - time.monotonic()
        if remaining_time <= 0.0:
            return _unknown_native(
                int(edge["child_depth"]), "time limit exceeded"
            )
    return dict(
        solve_reveal_verified_mate_cpp(
            edge["child_game"],
            attacker=attacker,
            depth=int(edge["child_depth"]),
            max_nodes=max_nodes,
            time_limit_seconds=remaining_time,
            preferred_attacker_actions=preferred_attacker_actions,
            include_proof_dag=False,
            exhaustive_attacker_actions=exhaustive_attacker_actions,
            exact_reveal_search=True,
            cancellation_token=cancellation_token,
        )
    )


def _solve_positive_portfolio(
    game: Game,
    *,
    attacker: int,
    depth: int,
    max_nodes: int,
    deadline: Optional[float],
    required_root_action: Optional[int],
    preferred_attacker_actions: list[int],
    cancellation_token: MateSearchCancellationToken,
) -> dict[str, Any]:
    if cancellation_token.is_cancelled:
        return _unknown_native(depth, "search cancelled")
    remaining_time = 0.0
    if deadline is not None:
        remaining_time = deadline - time.monotonic()
        if remaining_time <= 0.0:
            return _unknown_native(depth, "time limit exceeded")
    return dict(
        solve_reveal_verified_mate_cpp(
            game,
            attacker=attacker,
            depth=depth,
            max_nodes=max_nodes,
            time_limit_seconds=remaining_time,
            preferred_attacker_actions=preferred_attacker_actions,
            required_root_action=(
                _NO_REQUIRED_ACTION
                if required_root_action is None
                else required_root_action
            ),
            exhaustive_attacker_actions=False,
            exact_reveal_search=False,
            cancellation_token=cancellation_token,
        )
    )


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


def _solve_depth_parallel(
    game: Game,
    *,
    attacker: int,
    depth: int,
    max_nodes: int,
    deadline: Optional[float],
    required_root_action: Optional[int],
    preferred_attacker_actions: list[int],
    jobs: int,
    parallel_min_branches: int,
    cancellation_token: Optional[MateSearchCancellationToken],
    exhaustive_attacker_actions: bool = True,
    conclusive_refutations: bool = True,
) -> Optional[dict[str, Any]]:
    """Solve one exact depth by splitting root actions/reveals across threads."""
    split_started = time.monotonic()
    split = dict(
        solve_reveal_verified_root_split_cpp(
            game,
            attacker=attacker,
            depth=depth,
            required_root_action=(
                _NO_REQUIRED_ACTION
                if required_root_action is None
                else required_root_action
            ),
            exhaustive_attacker_actions=exhaustive_attacker_actions,
            cancellation_token=cancellation_token,
        )
    )
    edges = [dict(edge) for edge in split["edges"]]
    split_stats = [dict(split["stats"])]
    prefix_edges: list[tuple[dict[str, Any], int]] = []
    preferred_for_split = list(preferred_attacker_actions)
    if (
        bool(split["complete"])
        and len(edges) == 1
        and len(edges) < max(2, parallel_min_branches)
    ):
        prefix_action_count = len(
            {int(edge["action_code"]) for edge in edges}
        )
        prefix_edge = edges[0]
        if int(split["player"]) == attacker and preferred_for_split:
            preferred_for_split = preferred_for_split[1:]
        child_split = dict(
            solve_reveal_verified_root_split_cpp(
                prefix_edge["child_game"],
                attacker=attacker,
                depth=int(prefix_edge["child_depth"]),
                exhaustive_attacker_actions=exhaustive_attacker_actions,
                cancellation_token=cancellation_token,
            )
        )
        child_edges = [dict(edge) for edge in child_split["edges"]]
        if bool(child_split["complete"]) and len(child_edges) >= max(
            2, parallel_min_branches
        ):
            prefix_edges.append((prefix_edge, prefix_action_count))
            split = child_split
            split_stats.append(dict(child_split["stats"]))
            edges = child_edges

    if (
        not bool(split["complete"])
        or len(edges) < max(2, parallel_min_branches)
        or _effective_jobs(jobs) <= 1
    ):
        return None

    preferred_root = (
        preferred_attacker_actions[0] if preferred_attacker_actions else None
    )
    edges.sort(
        key=lambda edge: (
            int(edge["action_code"]) != preferred_root,
            int(edge["action_code"]),
            -1 if edge["reveal_card"] is None else int(edge["reveal_card"]),
        )
    )
    action_edges: dict[int, list[dict[str, Any]]] = {}
    for edge in edges:
        action_edges.setdefault(int(edge["action_code"]), []).append(edge)

    root_player = int(split["player"])
    child_preferred = list(preferred_for_split)
    if root_player == attacker and child_preferred:
        child_preferred = child_preferred[1:]

    use_positive_portfolio = not conclusive_refutations
    budget_units = len(edges) + int(use_positive_portfolio)
    portfolio_limit = 0
    branch_limits = [0] * len(edges)
    if max_nodes:
        branch_budget = max_nodes
        if use_positive_portfolio:
            # The blank/oracle portfolio is usually the fastest positive
            # prover.  Giving it one equal-sized edge share made a 16-way run
            # weaker than the serial search under a finite node budget.
            portfolio_limit = max(1, max_nodes // 2)
            branch_budget = max_nodes - portfolio_limit
        quotient, remainder = divmod(branch_budget, len(edges))
        branch_limits = [
            quotient + int(index < remainder) for index in range(len(edges))
        ]

    group_tokens = {
        action_code: MateSearchCancellationToken(cancellation_token)
        for action_code in action_edges
    }
    groups: dict[int, dict[str, Any]] = {
        action_code: {
            "remaining": len(group_edges),
            "has_unknown": False,
            "status": None,
            "selected": None,
            "first": None,
        }
        for action_code, group_edges in action_edges.items()
    }
    completed_raw: list[dict[str, Any]] = []
    global_status: Optional[str] = None
    selected: Optional[tuple[dict[str, Any], dict[str, Any]]] = None
    direct_selected_raw: Optional[dict[str, Any]] = None

    max_workers = min(_effective_jobs(jobs), budget_units)
    executor = ThreadPoolExecutor(
        max_workers=max_workers,
        thread_name_prefix="csplendor-mate",
    )
    futures: dict[Any, tuple[str, Optional[dict[str, Any]], Optional[int]]] = {}
    portfolio_token = MateSearchCancellationToken(cancellation_token)
    try:
        if use_positive_portfolio:
            if max_nodes and portfolio_limit == 0:
                portfolio_future = executor.submit(
                    lambda: _unknown_native(depth, "parallel node share exhausted")
                )
            else:
                portfolio_future = executor.submit(
                    _solve_positive_portfolio,
                    game,
                    attacker=attacker,
                    depth=depth,
                    max_nodes=portfolio_limit,
                    deadline=deadline,
                    required_root_action=required_root_action,
                    preferred_attacker_actions=preferred_attacker_actions,
                    cancellation_token=portfolio_token,
                )
            futures[portfolio_future] = ("portfolio", None, None)
        for index, edge in enumerate(edges):
            action_code = int(edge["action_code"])
            if max_nodes and branch_limits[index] == 0:
                raw = _unknown_native(depth, "parallel node share exhausted")
                future = executor.submit(lambda value=raw: value)
            else:
                future = executor.submit(
                    _solve_parallel_branch,
                    edge,
                    attacker=attacker,
                    max_nodes=branch_limits[index],
                    deadline=deadline,
                    preferred_attacker_actions=child_preferred,
                    cancellation_token=group_tokens[action_code],
                    exhaustive_attacker_actions=exhaustive_attacker_actions,
                )
            futures[future] = ("branch", edge, action_code)

        for future in as_completed(futures):
            kind, edge, action_code = futures[future]
            try:
                raw = dict(future.result())
            except CancelledError:
                continue
            completed_raw.append(raw)
            if kind == "portfolio":
                if _native_status(raw) == "mate":
                    global_status = "mate"
                    direct_selected_raw = raw
                    portfolio_token.request_cancel()
                    for token in group_tokens.values():
                        token.request_cancel()
                    for other in futures:
                        if other is not future:
                            other.cancel()
                continue
            assert edge is not None and action_code is not None
            group = groups[action_code]
            if group["status"] is not None:
                continue
            group["remaining"] = int(group["remaining"]) - 1
            status = _native_status(raw)
            if group["first"] is None:
                group["first"] = (edge, raw)
            if status == "no_mate":
                group["status"] = (
                    "no_mate" if conclusive_refutations else "unknown"
                )
                group["selected"] = (edge, raw)
                group_tokens[action_code].request_cancel()
                for other, (other_kind, _, other_action) in futures.items():
                    if (
                        other_kind == "branch"
                        and other_action == action_code
                        and other is not future
                    ):
                        other.cancel()
            elif status == "unknown":
                group["has_unknown"] = True
                if group["selected"] is None:
                    group["selected"] = (edge, raw)

            if group["status"] is None and group["remaining"] == 0:
                group["status"] = (
                    "unknown" if bool(group["has_unknown"]) else "mate"
                )
                if group["selected"] is None:
                    group["selected"] = group["first"]

            if global_status is None:
                resolved_groups = [
                    item for item in groups.values() if item["status"] is not None
                ]
                if root_player == attacker:
                    winning = next(
                        (
                            item
                            for item in resolved_groups
                            if item["status"] == "mate"
                        ),
                        None,
                    )
                    if winning is not None:
                        global_status = "mate"
                        selected = winning["selected"]
                    elif len(resolved_groups) == len(groups):
                        unknown = next(
                            (
                                item
                                for item in resolved_groups
                                if item["status"] == "unknown"
                            ),
                            None,
                        )
                        global_status = "unknown" if unknown else "no_mate"
                        chosen = unknown or resolved_groups[0]
                        selected = chosen["selected"]
                else:
                    refuted = next(
                        (
                            item
                            for item in resolved_groups
                            if item["status"] == "no_mate"
                        ),
                        None,
                    )
                    if refuted is not None:
                        global_status = "no_mate"
                        selected = refuted["selected"]
                    elif len(resolved_groups) == len(groups):
                        unknown = next(
                            (
                                item
                                for item in resolved_groups
                                if item["status"] == "unknown"
                            ),
                            None,
                        )
                        global_status = "unknown" if unknown else "mate"
                        chosen = unknown or resolved_groups[0]
                        selected = chosen["selected"]

                if global_status in {"mate", "no_mate"}:
                    portfolio_token.request_cancel()
                    for token in group_tokens.values():
                        token.request_cancel()
                    for other in futures:
                        if other is not future:
                            other.cancel()
    finally:
        executor.shutdown(wait=True, cancel_futures=True)

    if global_status is None:
        global_status = "unknown"
    aggregate = _empty_native_stats()
    for source in split_stats + [
        dict(raw["stats"]) for raw in completed_raw
    ]:
        for name, value in source.items():
            if name == "elapsed_ms" or name not in aggregate:
                continue
            if isinstance(value, (int, float)):
                aggregate[name] = aggregate[name] + value
    aggregate["elapsed_ms"] = (time.monotonic() - split_started) * 1000.0
    aggregate["parallel_jobs"] = max_workers
    aggregate["parallel_branches"] = len(edges)
    aggregate["parallel_action_groups"] = len(action_edges)

    line: list[dict[str, Any]] = []
    selected_raw: Optional[dict[str, Any]] = None
    if direct_selected_raw is not None:
        selected_raw = direct_selected_raw
        line.extend(dict(entry) for entry in direct_selected_raw.get("line", []))
    elif selected is not None:
        selected_edge, selected_raw = selected
        for prefix_edge, prefix_action_count in prefix_edges:
            line.append(
                {
                    "action_code": int(prefix_edge["action_code"]),
                    "reveal_card": prefix_edge["reveal_card"],
                    "action_count": prefix_action_count,
                }
            )
        line.append(
            {
                "action_code": int(selected_edge["action_code"]),
                "reveal_card": selected_edge["reveal_card"],
                "action_count": len(action_edges),
            }
        )
        line.extend(dict(entry) for entry in selected_raw.get("line", []))

    unknown_reason = None
    if global_status == "unknown":
        unknown_reason = (
            selected_raw.get("unknown_reason")
            if selected_raw is not None
            else "parallel root search was indeterminate"
        )
        unknown_reason = str(unknown_reason or "parallel root search was indeterminate")
    return {
        "proven": global_status == "mate",
        "attacker": attacker,
        "depth": depth,
        "reason": (
            "all_reveals_verified"
            if global_status == "mate"
            else unknown_reason
            if global_status == "unknown"
            else "candidate_mate_not_verified"
        ),
        "unknown_reason": unknown_reason,
        "memoized_states": sum(
            int(raw.get("memoized_states", 0)) for raw in completed_raw
        ),
        "stats": aggregate,
        "line": line,
        "proof_dag": {},
        "parallel": {
            "jobs": max_workers,
            "branches": len(edges),
            "action_groups": len(action_edges),
            "exact": True,
            "exhaustive": exhaustive_attacker_actions,
            "conclusive_refutations": conclusive_refutations,
            "split_ply": 1 + len(prefix_edges),
            "positive_portfolio": use_positive_portfolio,
        },
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
    parallel_min_branches: int = 2,
    cancellation_token: Optional[MateSearchCancellationToken] = None,
    include_proof_dag: bool = False,
    proof_dag_node_limit: int = 100_000,
    proof_dag_edge_limit: int = 500_000,
    proof_dag_format: str = "compact",
) -> dict[str, Any]:
    """Search consecutive mate depths until a sound stopping condition.

    ``max_nodes`` and ``time_limit_seconds`` are cumulative budgets for the
    complete depth range, not fresh budgets for each depth.  A conclusive
    refutation at depth ``N`` advances immediately to ``N + 1``.  A mate,
    an indeterminate search-limit result, or ``max_depth`` stops the sweep.

    Refutation is monotone only towards *smaller* bounds: proving no mate at
    depth ``N`` says nothing about ``N + 1``.  Consequently this function
    never extrapolates permanent no-mate from repeated bounded refutations.
    It may stop before searching only when it can emit a separate, unbounded
    certificate (a terminal non-attacker result or a score ceiling below 15).
    """
    if attacker not in (0, 1):
        raise ValueError("attacker must be 0 or 1")
    if min_depth < 0 or max_depth < min_depth:
        raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
    if max_nodes < 0 or time_limit_seconds < 0:
        raise ValueError("search limits must be non-negative")
    if jobs < 0:
        raise ValueError("jobs must be non-negative")
    if parallel_min_branches < 2:
        raise ValueError("parallel_min_branches must be at least 2")
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
    started = time.monotonic()
    deadline = (
        started + time_limit_seconds if time_limit_seconds > 0.0 else None
    )
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
            "jobs": _effective_jobs(jobs),
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

        native = None
        if _effective_jobs(jobs) > 1 and not include_proof_dag:
            native = _solve_depth_parallel(
                game,
                attacker=attacker,
                depth=depth,
                max_nodes=remaining_nodes,
                deadline=deadline,
                required_root_action=required_code,
                preferred_attacker_actions=preferred_codes,
                jobs=jobs,
                parallel_min_branches=parallel_min_branches,
                cancellation_token=cancellation_token,
            )
        if native is None:
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
                exhaustive_attacker_actions=True,
                exact_reveal_search=True,
                cancellation_token=cancellation_token,
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
        if "parallel" in native:
            attempt["parallel"] = dict(native["parallel"])
        attempts.append(attempt)

        if proven:
            mate_depth = depth
            last_completed_depth = depth
            status = "mate"
            stop_reason = "mate_proven"
            break
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
        "jobs": _effective_jobs(jobs),
        "exact": True,
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
    parallel_min_branches: int = 2,
    cancellation_token: Optional[MateSearchCancellationToken] = None,
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
    if parallel_min_branches < 2:
        raise ValueError("parallel_min_branches must be at least 2")
    required_code = None if required_root_action is None else int(required_root_action)
    legal_by_code = {int(action.pack()): action for action in game.legal_actions}
    if required_code is not None:
        if int(game.current_player) != attacker:
            raise ValueError("required_root_action requires attacker to move")
        if required_code not in legal_by_code:
            raise ValueError("required_root_action must be legal in the root state")

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
        attempt_deadline = (
            None if global_deadline is None else time.monotonic() + attempt_time
        )
        raw = None
        if _effective_jobs(jobs) > 1:
            raw = _solve_depth_parallel(
                game,
                attacker=attacker,
                depth=depth,
                max_nodes=attempt_nodes,
                deadline=attempt_deadline,
                required_root_action=required_code,
                preferred_attacker_actions=preferred_codes,
                jobs=jobs,
                parallel_min_branches=parallel_min_branches,
                cancellation_token=cancellation_token,
                exhaustive_attacker_actions=False,
                conclusive_refutations=False,
            )
        if raw is None:
            raw = dict(
                solve_reveal_verified_mate_cpp(
                    game,
                    attacker=attacker,
                    depth=depth,
                    max_nodes=attempt_nodes,
                    time_limit_seconds=attempt_time,
                    preferred_attacker_actions=preferred_codes,
                    required_root_action=(
                        _NO_REQUIRED_ACTION
                        if required_code is None
                        else required_code
                    ),
                    exhaustive_attacker_actions=False,
                    exact_reveal_search=False,
                    cancellation_token=cancellation_token,
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
        if "parallel" in raw:
            attempt["parallel"] = dict(raw["parallel"])
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
        "jobs": _effective_jobs(jobs),
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
