"""Deadline-aware, reusable mate search session for game-playing agents."""

from __future__ import annotations

import threading
import time
from typing import Any, Optional

from ._csplendor import (
    Game,
    MateSearchCancellationToken,
    NativeMateSearchSession,
)
from .api.usi_kifu import action_to_usi
from .mate_depth import (
    MATE_DEPTH_SEARCH_FORMAT,
    _effective_jobs,
    _native_status,
    _permanent_no_mate_certificate,
    search_reveal_verified_mate_anytime,
    search_reveal_verified_mate_depths,
)

MATE_SEARCH_SESSION_FORMAT = "csplendor_mate_search_session_v1"


class MateSearchSession:
    """Reusable exact mate search for an AI player.

    The native exact transposition table survives calls, so a later position
    from the same game can reuse states visited on the preceding turn.  With
    multiple jobs, a short warm-cache probe runs before root-parallel search.
    Search is serialized per session, while :meth:`cancel` is thread-safe and
    may be called by a clock or an AI controller.
    """

    def __init__(
        self,
        attacker: int,
        *,
        jobs: int = 1,
        parallel_min_branches: int = 2,
        warm_start_nodes: int = 50_000,
        warm_start_time_seconds: float = 0.05,
        max_cache_states: int = 2_000_000,
    ) -> None:
        if attacker not in (0, 1):
            raise ValueError("attacker must be 0 or 1")
        if jobs < 0:
            raise ValueError("jobs must be non-negative")
        if parallel_min_branches < 2:
            raise ValueError("parallel_min_branches must be at least 2")
        if (
            warm_start_nodes < 0
            or warm_start_time_seconds < 0
            or max_cache_states < 0
        ):
            raise ValueError("warm-start limits must be non-negative")
        if jobs != 1 and warm_start_nodes == 0 and warm_start_time_seconds == 0:
            raise ValueError("parallel warm start needs a node or time limit")
        self.attacker = attacker
        self.jobs = jobs
        self.parallel_min_branches = parallel_min_branches
        self.warm_start_nodes = warm_start_nodes
        self.warm_start_time_seconds = warm_start_time_seconds
        self.max_cache_states = max_cache_states
        self._native = NativeMateSearchSession(attacker)
        self._cancellation_token = MateSearchCancellationToken()
        self._search_lock = threading.Lock()
        self._preferred_root_actions: list[int] = []
        self._simple_payment_mode: Optional[bool] = None

    @property
    def memoized_states(self) -> int:
        return int(self._native.memoized_states)

    def cancel(self) -> None:
        """Cooperatively stop the active search."""
        self._cancellation_token.request_cancel()

    def clear(self) -> None:
        """Drop knowledge from the previous game and reset cancellation."""
        with self._search_lock:
            self._native.clear()
            self._preferred_root_actions = []
            self._simple_payment_mode = None
            self._cancellation_token.reset()

    def search(
        self,
        game: Game,
        *,
        min_depth: int,
        max_depth: int,
        max_nodes: int = 0,
        time_limit_seconds: float = 0.0,
    ) -> dict[str, Any]:
        """Find the shallowest proven mate available before the hard limits."""
        if min_depth < 0 or max_depth < min_depth:
            raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
        if max_nodes < 0 or time_limit_seconds < 0:
            raise ValueError("search limits must be non-negative")

        with self._search_lock:
            self._cancellation_token.reset()
            simple_payment_mode = bool(game.simple_payment_mode)
            if (
                self._simple_payment_mode is not None
                and self._simple_payment_mode != simple_payment_mode
            ):
                self._native.clear()
                self._preferred_root_actions = []
            self._simple_payment_mode = simple_payment_mode
            return self._search_locked(
                game,
                min_depth=min_depth,
                max_depth=max_depth,
                max_nodes=max_nodes,
                time_limit_seconds=time_limit_seconds,
            )

    def search_anytime(
        self,
        game: Game,
        *,
        min_depth: int,
        max_depth: int,
        max_nodes: int = 0,
        time_limit_seconds: float = 0.0,
    ) -> dict[str, Any]:
        """Find any verified mate in time, without waiting for minimality."""
        if min_depth < 0 or max_depth < min_depth:
            raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
        if max_nodes < 0 or time_limit_seconds < 0:
            raise ValueError("search limits must be non-negative")
        with self._search_lock:
            self._cancellation_token.reset()
            simple_payment_mode = bool(game.simple_payment_mode)
            if (
                self._simple_payment_mode is not None
                and self._simple_payment_mode != simple_payment_mode
            ):
                self._native.clear()
                self._preferred_root_actions = []
            self._simple_payment_mode = simple_payment_mode
            return self._search_anytime_locked(
                game,
                min_depth=min_depth,
                max_depth=max_depth,
                max_nodes=max_nodes,
                time_limit_seconds=time_limit_seconds,
            )

    def _search_anytime_locked(
        self,
        game: Game,
        *,
        min_depth: int,
        max_depth: int,
        max_nodes: int,
        time_limit_seconds: float,
    ) -> dict[str, Any]:
        started = time.monotonic()
        deadline = (
            started + time_limit_seconds if time_limit_seconds > 0.0 else None
        )
        cache_before = self.memoized_states
        attempts: list[dict[str, Any]] = []
        total_nodes = 0
        reused_memo_hits = 0
        iterative_order_hits = 0
        mate_depth: Optional[int] = None
        winning_action: Optional[int] = None
        stop_reason = "max_depth_reached_without_proof"

        for offset, depth in enumerate(range(min_depth, max_depth + 1)):
            attempts_left = max_depth - min_depth + 1 - offset
            remaining_nodes = 0 if max_nodes == 0 else max_nodes - total_nodes
            remaining_time = (
                0.0
                if deadline is None
                else max(0.0, deadline - time.monotonic())
            )
            if (max_nodes and remaining_nodes <= 0) or (
                deadline is not None and remaining_time <= 0.0
            ):
                stop_reason = "cumulative search limit exceeded"
                break

            depth_node_share = (
                0
                if max_nodes == 0
                else max(1, remaining_nodes // attempts_left)
            )
            depth_time_share = (
                0.0
                if deadline is None
                else remaining_time / attempts_left
            )
            warm_nodes = self.warm_start_nodes
            if max_nodes:
                warm_nodes = min(
                    max(1, warm_nodes),
                    max(1, depth_node_share // 4),
                )
            warm_time = self.warm_start_time_seconds
            if deadline is not None:
                warm_time = min(warm_time, depth_time_share / 4.0)
            # A one-node lookup is enough to recover a previously completed
            # root even when background cache growth is disabled.
            if warm_nodes == 0 and warm_time == 0.0:
                warm_nodes = 1

            probe = dict(
                self._native.search(
                    game,
                    depth=depth,
                    max_nodes=warm_nodes,
                    time_limit_seconds=warm_time,
                    preferred_attacker_actions=self._preferred_root_actions,
                    cancellation_token=self._cancellation_token,
                    max_cache_states=self.max_cache_states,
                )
            )
            probe_nodes = int(probe["stats"].get("nodes", 0))
            probe_hits = int(
                probe["stats"].get("persistent_memo_hits", 0)
            )
            total_nodes += probe_nodes
            reused_memo_hits += probe_hits
            iterative_order_hits += int(
                probe["stats"].get("iterative_order_hits", 0)
            )
            probe_status = _native_status(probe)
            if probe_status == "mate":
                raw = probe
                phase = "warm_cache"
            elif probe_status == "no_mate":
                attempts.append(
                    {
                        "depth": depth,
                        "status": "inconclusive",
                        "native_status": "no_mate",
                        "phase": "warm_cache_refutation",
                        "reason": str(probe["reason"]),
                        "unknown_reason": None,
                        "stats": dict(probe["stats"]),
                        "line": [dict(entry) for entry in probe.get("line", [])],
                    }
                )
                continue
            else:
                remaining_nodes = (
                    0 if max_nodes == 0 else max(0, max_nodes - total_nodes)
                )
                remaining_time = (
                    0.0
                    if deadline is None
                    else max(0.0, deadline - time.monotonic())
                )
                if (max_nodes and remaining_nodes <= 0) or (
                    deadline is not None and remaining_time <= 0.0
                ):
                    stop_reason = "cumulative search limit exceeded"
                    break
                depth_nodes = (
                    0
                    if max_nodes == 0
                    else max(1, remaining_nodes // attempts_left)
                )
                depth_time = (
                    0.0
                    if deadline is None
                    else remaining_time / attempts_left
                )
                searched = search_reveal_verified_mate_anytime(
                    game,
                    attacker=self.attacker,
                    min_depth=depth,
                    max_depth=depth,
                    max_nodes=depth_nodes,
                    time_limit_seconds=depth_time,
                    preferred_attacker_actions=self._preferred_root_actions,
                    jobs=self.jobs,
                    parallel_min_branches=self.parallel_min_branches,
                    cancellation_token=self._cancellation_token,
                )
                if not searched["attempts"]:
                    stop_reason = str(searched["stop_reason"])
                    break
                raw = dict(searched["attempts"][-1])
                phase = "parallel_positive_proof"
                total_nodes += int(raw["stats"].get("nodes", 0))

            native_status = (
                "mate"
                if bool(raw.get("proven", raw.get("status") == "mate"))
                else str(raw.get("native_status", "unknown"))
            )
            attempt = {
                "depth": depth,
                "status": "mate" if native_status == "mate" else "inconclusive",
                "native_status": native_status,
                "phase": phase,
                "reason": str(raw["reason"]),
                "unknown_reason": raw.get("unknown_reason"),
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
                    self._preferred_root_actions = [winning_action]
                break
            if raw.get("unknown_reason") == "search cancelled":
                stop_reason = "search cancelled"
                break

        winning_usi = None
        if winning_action is not None:
            legal = {
                int(action.pack()): action for action in game.legal_actions
            }
            if winning_action in legal:
                winning_usi = action_to_usi(legal[winning_action], game=game)
        return {
            "format": MATE_SEARCH_SESSION_FORMAT,
            "mode": "anytime",
            "status": "mate" if mate_depth is not None else "no_mate_found",
            "stop_reason": stop_reason,
            "attacker": self.attacker,
            "min_depth": min_depth,
            "max_depth": max_depth,
            "mate_depth": mate_depth,
            "winning_root_action": winning_action,
            "winning_root_action_usi": winning_usi,
            "minimal_depth_proven": False,
            "verified_no_mate_through_depth": None,
            "jobs": _effective_jobs(self.jobs),
            "attempts": attempts,
            "stats": {
                "nodes": total_nodes,
                "elapsed_ms": (time.monotonic() - started) * 1000.0,
                "memoized_states_before": cache_before,
                "memoized_states_after": self.memoized_states,
                "reused_memo_hits": reused_memo_hits,
                "iterative_order_hits": iterative_order_hits,
            },
        }

    def _search_locked(
        self,
        game: Game,
        *,
        min_depth: int,
        max_depth: int,
        max_nodes: int,
        time_limit_seconds: float,
    ) -> dict[str, Any]:
        started = time.monotonic()
        deadline = (
            started + time_limit_seconds if time_limit_seconds > 0.0 else None
        )
        cache_before = self.memoized_states
        certificate = _permanent_no_mate_certificate(game, self.attacker)
        if certificate is not None:
            return {
                "format": MATE_SEARCH_SESSION_FORMAT,
                "depth_search_format": MATE_DEPTH_SEARCH_FORMAT,
                "status": "permanent_no_mate",
                "stop_reason": str(certificate["kind"]),
                "mate_depth": None,
                "winning_root_action": None,
                "winning_root_action_usi": None,
                "verified_no_mate_through_depth": max_depth,
                "permanent_no_mate_proven": True,
                "permanent_no_mate_certificate": certificate,
                "attempts": [],
                "stats": {
                    "nodes": 0,
                    "elapsed_ms": (time.monotonic() - started) * 1000.0,
                    "memoized_states_before": cache_before,
                    "memoized_states_after": self.memoized_states,
                    "reused_memo_hits": 0,
                    "iterative_order_hits": 0,
                },
            }

        attempts: list[dict[str, Any]] = []
        total_nodes = 0
        reused_memo_hits = 0
        iterative_order_hits = 0
        last_refuted_depth: Optional[int] = None
        mate_depth: Optional[int] = None
        winning_action: Optional[int] = None
        status = "no_mate_within_max_depth"
        stop_reason = "max_depth_reached"

        for depth in range(min_depth, max_depth + 1):
            remaining_nodes = 0 if max_nodes == 0 else max_nodes - total_nodes
            remaining_time = (
                0.0
                if deadline is None
                else max(0.0, deadline - time.monotonic())
            )
            if max_nodes and remaining_nodes <= 0:
                status = "unknown"
                stop_reason = "cumulative node limit exceeded"
                break
            if deadline is not None and remaining_time <= 0.0:
                status = "unknown"
                stop_reason = "cumulative time limit exceeded"
                break
            if self._cancellation_token.is_cancelled:
                status = "unknown"
                stop_reason = "search cancelled"
                break

            parallel = _effective_jobs(self.jobs) > 1
            warm_nodes = remaining_nodes
            warm_time = remaining_time
            if parallel:
                if max_nodes:
                    warm_nodes = min(
                        remaining_nodes,
                        max(1, self.warm_start_nodes),
                    )
                else:
                    warm_nodes = self.warm_start_nodes
                if deadline is not None:
                    warm_time = min(remaining_time, self.warm_start_time_seconds)
                else:
                    warm_time = self.warm_start_time_seconds

            warm_raw = dict(
                self._native.search(
                    game,
                    depth=depth,
                    max_nodes=max(0, warm_nodes),
                    time_limit_seconds=max(0.0, warm_time),
                    preferred_attacker_actions=self._preferred_root_actions,
                    cancellation_token=self._cancellation_token,
                    max_cache_states=self.max_cache_states,
                )
            )
            warm_stats = dict(warm_raw["stats"])
            warm_used = int(warm_stats.get("nodes", 0))
            total_nodes += warm_used
            reused_memo_hits += int(
                warm_stats.get("persistent_memo_hits", 0)
            )
            iterative_order_hits += int(
                warm_stats.get("iterative_order_hits", 0)
            )
            raw = warm_raw
            phase = "warm_cache" if parallel else "persistent_exact"

            if parallel and _native_status(warm_raw) == "unknown":
                unknown_reason = str(warm_raw.get("unknown_reason") or "")
                if unknown_reason not in {
                    "search cancelled",
                    "cumulative time limit exceeded",
                }:
                    remaining_nodes = (
                        0 if max_nodes == 0 else max(0, max_nodes - total_nodes)
                    )
                    remaining_time = (
                        0.0
                        if deadline is None
                        else max(0.0, deadline - time.monotonic())
                    )
                    if (not max_nodes or remaining_nodes > 0) and (
                        deadline is None or remaining_time > 0.0
                    ):
                        cold = search_reveal_verified_mate_depths(
                            game,
                            attacker=self.attacker,
                            min_depth=depth,
                            max_depth=depth,
                            max_nodes=remaining_nodes,
                            time_limit_seconds=remaining_time,
                            preferred_attacker_actions=self._preferred_root_actions,
                            jobs=self.jobs,
                            parallel_min_branches=self.parallel_min_branches,
                            cancellation_token=self._cancellation_token,
                        )
                        if cold["attempts"]:
                            raw = dict(cold["attempts"][-1])
                            phase = "parallel_exact"
                            total_nodes += int(raw["stats"].get("nodes", 0))

            raw_status = _native_status(raw)
            attempt = {
                "depth": depth,
                "status": raw_status,
                "phase": phase,
                "reason": str(raw["reason"]),
                "unknown_reason": raw["unknown_reason"],
                "stats": dict(raw["stats"]),
                "line": [dict(entry) for entry in raw.get("line", [])],
            }
            if "parallel" in raw:
                attempt["parallel"] = dict(raw["parallel"])
            attempts.append(attempt)

            if raw_status == "mate":
                mate_depth = depth
                status = "mate"
                stop_reason = "mate_proven"
                if attempt["line"]:
                    winning_action = int(attempt["line"][0]["action_code"])
                    self._preferred_root_actions = [winning_action]
                break
            if raw_status == "unknown":
                status = "unknown"
                stop_reason = str(raw["unknown_reason"] or raw["reason"])
                break
            last_refuted_depth = depth

        winning_usi = None
        if winning_action is not None:
            legal = {
                int(action.pack()): action for action in game.legal_actions
            }
            if winning_action in legal:
                winning_usi = action_to_usi(legal[winning_action], game=game)
        return {
            "format": MATE_SEARCH_SESSION_FORMAT,
            "depth_search_format": MATE_DEPTH_SEARCH_FORMAT,
            "status": status,
            "stop_reason": stop_reason,
            "attacker": self.attacker,
            "min_depth": min_depth,
            "max_depth": max_depth,
            "mate_depth": mate_depth,
            "winning_root_action": winning_action,
            "winning_root_action_usi": winning_usi,
            "verified_no_mate_through_depth": last_refuted_depth,
            "permanent_no_mate_proven": False,
            "permanent_no_mate_certificate": None,
            "jobs": _effective_jobs(self.jobs),
            "attempts": attempts,
            "stats": {
                "nodes": total_nodes,
                "elapsed_ms": (time.monotonic() - started) * 1000.0,
                "memoized_states_before": cache_before,
                "memoized_states_after": self.memoized_states,
                "reused_memo_hits": reused_memo_hits,
                "iterative_order_hits": iterative_order_hits,
            },
        }


__all__ = ["MATE_SEARCH_SESSION_FORMAT", "MateSearchSession"]
