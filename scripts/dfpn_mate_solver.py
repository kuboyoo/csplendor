#!/usr/bin/env python3
from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, ProcessPoolExecutor, wait
import gc
import json
import multiprocessing as mp
import os
import sys
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs

from scripts.mate_solver import (
    INVALID_INPUT,
    MATE,
    NO_MATE,
    UNKNOWN,
    MateSolver,
    SearchResult,
    SearchStats,
    SolverOptions,
    SolverState,
    _Outcome,
    _parse_moves,
    _state_from_payload,
    _state_to_payload,
    apply_usi_moves,
    load_game_from_json,
    load_game_from_usi_file,
    load_game_from_usi_text,
)


INF = 10**12
_DFPN_DEFAULT_PRUNING = {
    "lazy_reveal": True,
    "attacker_dependency": True,
    "defender_relevance": True,
    "return_pattern": True,
    "upper_bound": True,
    "immediate_terminal": True,
    "defender_threat_filter": False,
    "max_actions_per_node": 0,
    "target_candidate_limit": 5,
}

_WORKER_STATE: Optional[SolverState] = None
_WORKER_ATTACKER = 0
_WORKER_MAX_DEPTH = 0
_WORKER_OPTIONS: Dict[str, Any] = {}
_WORKER_TASK_INDEX = -1
_WORKER_PROGRESS_ARRAY = None
_WORKER_PROGRESS_STRIDE = 0
_WORKER_PROGRESS_SLOT = -1
_CARD_INFO_CACHE: Dict[int, Tuple[int, int, int, Tuple[int, int, int, int, int]]] = {}
_NOBLE_REQ_CACHE: Dict[int, Tuple[int, int, int, int, int]] = {}


@dataclass
class DFPNStats(SearchStats):
    expansions: int = 0
    tt_hits: int = 0
    root_proof_number: int = 1
    root_disproof_number: int = 1
    dangerous_reveals: int = 0
    safe_reveal_collapses: int = 0
    threat_pruned_reveals: int = 0
    dangerous_reveal_collapses: int = 0
    lazy_reveal_branches: int = 0
    lazy_reveal_refinements: int = 0
    lazy_reveal_pruned: int = 0
    lazy_action_pruned: int = 0
    lazy_action_refinements: int = 0
    action_pruned: int = 0
    return_pattern_pruned: int = 0
    upper_bound_prunes: int = 0
    immediate_terminal_prunes: int = 0


class ProgressReporter:
    def __init__(self, enabled: bool, interval: float, stream=None):
        self.enabled = bool(enabled)
        self.interval = max(0.1, float(interval))
        self.stream = stream or sys.stderr
        self._last_emit = 0.0
        self._last_len = 0
        self._active_line = False

    def maybe_emit(
        self,
        stats: DFPNStats,
        start_time: float,
        *,
        root: Optional["_DFPNNode"] = None,
        force: bool = False,
        extra: str = "",
    ) -> None:
        if not self.enabled:
            return
        now = time.monotonic()
        if not force and now - self._last_emit < self.interval:
            return
        self._last_emit = now
        elapsed = now - start_time if start_time else 0.0
        proof = int(root.proof) if root is not None else int(stats.root_proof_number)
        disproof = int(root.disproof) if root is not None else int(stats.root_disproof_number)
        rate = stats.nodes / elapsed if elapsed > 0 else 0.0
        parts = [
            f"elapsed={elapsed:.1f}s",
            f"nodes={stats.nodes}",
            f"nps={rate:.0f}",
            f"depth={stats.max_depth_reached}",
            f"pn={proof}",
            f"dn={disproof}",
            f"memo={stats.memo_hits}",
            f"reveal={stats.reveal_branches}",
            f"lazy={stats.lazy_reveal_branches}/{stats.lazy_reveal_refinements}",
            f"pruned={stats.threat_pruned_reveals + stats.lazy_action_pruned}",
        ]
        if extra:
            parts.append(extra)
        line = "[dfpn] " + " ".join(parts)
        padding = " " * max(0, self._last_len - len(line))
        print("\r" + line + padding, end="", file=self.stream, flush=True)
        self._last_len = len(line)
        self._active_line = True

    def finish_line(self) -> None:
        if not self.enabled or not self._active_line:
            return
        print(file=self.stream, flush=True)
        self._active_line = False
        self._last_len = 0


@dataclass
class _DFPNNode:
    kind: str
    node_type: str
    depth: int
    state: Optional[SolverState] = None
    action: Optional[cs.Action] = None
    outcome: Optional[_Outcome] = None
    proof: int = 1
    disproof: int = 1
    expanded: bool = False
    terminal: bool = False
    terminal_winner: Optional[int] = None
    reason: Optional[str] = None
    reveal_level: Optional[int] = None
    reveal_candidates: Tuple[int, ...] = ()
    lazy_reveal_materialized: bool = False
    omitted_actions: Tuple[cs.Action, ...] = ()
    lazy_actions_materialized: bool = True
    children: List["_DFPNNode"] = field(default_factory=list)


class SearchLimitExceeded(Exception):
    pass


class DFPNMateSolver:
    """Depth-first proof-number mate solver.

    The result is from the attacker's perspective:
    - OR nodes: attacker choices, including attacker noble choices.
    - AND nodes: defender choices and all hidden-card reveal possibilities.
    """

    def __init__(self, attacker: int, max_depth: int, options: Optional[SolverOptions] = None):
        if attacker not in (0, 1):
            raise ValueError("attacker must be 0 or 1")
        if max_depth < 0:
            raise ValueError("max_depth must be non-negative")
        self.attacker = attacker
        self.max_depth = max_depth
        self.options = options or SolverOptions()
        self.stats = DFPNStats()
        self._start_time = 0.0
        self._state_table: Dict[Tuple[Any, int], _DFPNNode] = {}
        self._helper = MateSolver(attacker=attacker, max_depth=max_depth, options=self.options)
        self._prune_inactive_subtrees = False
        self.use_lazy_reveal_pruning = bool(_DFPN_DEFAULT_PRUNING["lazy_reveal"])
        self.use_attacker_dependency_pruning = bool(_DFPN_DEFAULT_PRUNING["attacker_dependency"])
        self.use_defender_relevance_pruning = bool(_DFPN_DEFAULT_PRUNING["defender_relevance"])
        self.use_threat_reveal_pruning = True
        self.use_equivalence_hash = True
        self.use_tactical_action_pruning = True
        self.use_return_pattern_pruning = bool(_DFPN_DEFAULT_PRUNING["return_pattern"])
        self.use_upper_bound_pruning = bool(_DFPN_DEFAULT_PRUNING["upper_bound"])
        self.use_immediate_terminal_pruning = bool(_DFPN_DEFAULT_PRUNING["immediate_terminal"])
        self.use_defender_threat_filter = bool(_DFPN_DEFAULT_PRUNING["defender_threat_filter"])
        self.max_actions_per_node = int(_DFPN_DEFAULT_PRUNING["max_actions_per_node"])
        self.target_candidate_limit = int(_DFPN_DEFAULT_PRUNING["target_candidate_limit"])
        self.max_tt_entries: Optional[int] = None
        self.parallel_tt_limit = 10000
        self.progress = ProgressReporter(False, 1.0)
        self._last_worker_progress = 0.0
        self.parallel_start_method = "spawn"

    def solve(self, state: SolverState) -> SearchResult:
        self.stats = DFPNStats()
        self._state_table = {}
        self._start_time = time.monotonic()
        self._helper.stats = self.stats
        self._helper._start_time = self._start_time

        root = self._state_node(state, self.max_depth)
        self.progress.maybe_emit(self.stats, self._start_time, root=root, force=True, extra="status=start")
        try:
            if self._effective_jobs() > 1 and not root.terminal:
                return self._solve_root_parallel(root)

            self._dfpn(root, INF, INF)
            self.stats.root_proof_number = int(root.proof)
            self.stats.root_disproof_number = int(root.disproof)
            if root.proof == 0:
                proof = self._extract_tree(root, want_proof=True) if self.options.include_proof else None
                result = SearchResult(MATE, self.max_depth, proof, None, self._finish_stats())
                self._clear_search_memory(root)
                return result
            if root.disproof == 0:
                refutation = self._extract_tree(root, want_proof=False) if self.options.include_proof else None
                result = SearchResult(NO_MATE, None, None, refutation, self._finish_stats())
                self._clear_search_memory(root)
                return result

            self.stats.unknown_reason = "dfpn frontier exhausted without proof/disproof"
            result = SearchResult(UNKNOWN, None, None, None, self._finish_stats())
            self._clear_search_memory(root)
            return result
        except SearchLimitExceeded as exc:
            self.stats.root_proof_number = int(root.proof)
            self.stats.root_disproof_number = int(root.disproof)
            self.stats.unknown_reason = str(exc)
            result = SearchResult(UNKNOWN, None, None, None, self._finish_stats())
            self._clear_search_memory(root)
            return result
        except Exception:
            self.progress.finish_line()
            self._clear_search_memory(root)
            raise

    def _finish_stats(self) -> DFPNStats:
        self.stats.elapsed_ms = (time.monotonic() - self._start_time) * 1000.0
        self.progress.finish_line()
        return self.stats

    def _effective_jobs(self) -> int:
        if self.options.jobs == 0:
            return max(1, os.cpu_count() or 1)
        return max(1, self.options.jobs)

    def _solve_root_parallel(self, root: _DFPNNode) -> SearchResult:
        self._expand(root)
        self._update(root)
        if not root.children or root.proof == 0 or root.disproof == 0:
            return self._result_from_root(root)

        assert root.state is not None
        groups: Dict[int, Dict[str, Any]] = {}
        tasks: List[Dict[str, Any]] = []
        for index, child in enumerate(root.children):
            child_tasks = self._root_tasks_from_child(index, child)
            groups[index] = {
                "kind": "single" if len(child_tasks) == 1 and child_tasks[0]["kind"] == "noble" else "action",
                "pending": len(child_tasks),
                "total": len(child_tasks),
                "proof_values": [],
                "disproof_values": [],
                "proof_children": [],
                "refutation_children": [],
                "done": False,
                "proof": INF,
                "disproof": INF,
                "proof_tree": None,
                "refutation": None,
            }
            tasks.extend(child_tasks)
        root.children.clear()
        for task_index, task in enumerate(tasks):
            task["index"] = task_index
        if not tasks:
            return self._result_from_root(root)

        jobs = min(self._effective_jobs(), len(tasks))
        state_payload = _state_to_payload(root.state)
        root_type = root.node_type
        proof_found = False
        refutation_found = False
        solved_proof: Optional[Dict[str, Any]] = None
        solved_refutation: Optional[Dict[str, Any]] = None
        unknown_reason: Optional[str] = None

        for branch in self._run_parallel_tasks_dynamic(tasks, state_payload, jobs):
            self._merge_worker_stats(branch.get("stats", {}))
            group_index = int(branch.get("group_index", branch.get("index", -1)))
            group = groups[group_index]
            proof = int(branch.get("proof_number", INF))
            disproof = int(branch.get("disproof_number", INF))

            if branch.get("unknown_reason"):
                unknown_reason = str(branch["unknown_reason"])
                break

            group["pending"] -= 1
            group["proof_values"].append(proof)
            group["disproof_values"].append(disproof)
            if proof == 0 and branch.get("proof_tree") is not None:
                group["proof_children"].append(branch["proof_tree"])
            if disproof == 0 and branch.get("refutation") is not None:
                group["refutation_children"].append(branch["refutation"])

            self._update_parallel_group(group, branch)

            completed_groups = [g for g in groups.values() if g["done"]]
            if root_type == "OR":
                proving = next((g for g in completed_groups if g["proof"] == 0), None)
                if proving is not None:
                    proof_found = True
                    solved_proof = proving.get("proof_tree")
                    root.proof = 0
                    root.disproof = self._sat_sum(g["disproof"] for g in completed_groups)
                    break
                if len(completed_groups) == len(groups) and all(g["disproof"] == 0 for g in completed_groups):
                    refutation_found = True
                    root.proof = min(g["proof"] for g in completed_groups)
                    root.disproof = 0
                    break
            else:
                disproving = next((g for g in completed_groups if g["disproof"] == 0), None)
                if disproving is not None:
                    refutation_found = True
                    solved_refutation = disproving.get("refutation")
                    root.proof = self._sat_sum(g["proof"] for g in completed_groups)
                    root.disproof = 0
                    break
                if len(completed_groups) == len(groups) and all(g["proof"] == 0 for g in completed_groups):
                    proof_found = True
                    root.proof = 0
                    root.disproof = min(g["disproof"] for g in completed_groups)
                    break

        if root_type == "OR":
            done_groups = [g for g in groups.values() if g["done"]]
            root.proof = 0 if proof_found else (min((g["proof"] for g in done_groups), default=INF))
            root.disproof = self._sat_sum(g["disproof"] for g in done_groups)
        else:
            done_groups = [g for g in groups.values() if g["done"]]
            root.proof = self._sat_sum(g["proof"] for g in done_groups)
            root.disproof = 0 if refutation_found else (min((g["disproof"] for g in done_groups), default=INF))

        self.stats.root_proof_number = int(root.proof)
        self.stats.root_disproof_number = int(root.disproof)

        if proof_found:
            proof_tree = self._root_tree(root, [solved_proof] if solved_proof else []) if self.options.include_proof else None
            result = SearchResult(MATE, self.max_depth, proof_tree, None, self._finish_stats())
        elif refutation_found:
            refutation = self._root_tree(root, [solved_refutation] if solved_refutation else []) if self.options.include_proof else None
            result = SearchResult(NO_MATE, None, None, refutation, self._finish_stats())
        elif unknown_reason is not None:
            self.stats.unknown_reason = unknown_reason
            result = SearchResult(UNKNOWN, None, None, None, self._finish_stats())
        elif root.proof == 0:
            proof_tree = self._root_tree(root, []) if self.options.include_proof else None
            result = SearchResult(MATE, self.max_depth, proof_tree, None, self._finish_stats())
        elif root.disproof == 0:
            refutation = self._root_tree(root, []) if self.options.include_proof else None
            result = SearchResult(NO_MATE, None, None, refutation, self._finish_stats())
        else:
            self.stats.unknown_reason = "parallel dfpn frontier exhausted without proof/disproof"
            result = SearchResult(UNKNOWN, None, None, None, self._finish_stats())

        self._clear_search_memory(root)
        return result

    def _root_task_from_child(self, index: int, child: _DFPNNode) -> Dict[str, Any]:
        if child.kind not in {"action", "noble"} or child.action is None:
            raise RuntimeError("root parallel search can only split action or noble nodes")
        return {
            "index": index,
            "group_index": index,
            "kind": child.kind,
            "depth": child.depth,
            "action_code": int(child.action.pack()),
            "actor_is_attacker": child.reason == "attacker_action",
        }

    def _root_tasks_from_child(self, index: int, child: _DFPNNode) -> List[Dict[str, Any]]:
        if child.kind == "noble":
            return [self._root_task_from_child(index, child)]
        if child.kind != "action" or child.state is None or child.action is None:
            raise RuntimeError("root parallel search can only split action or noble nodes")
        if self.use_lazy_reveal_pruning and self._can_use_lazy_reveal(child.state, child.action):
            return [self._root_task_from_child(index, child)]

        actor_is_attacker = child.reason == "attacker_action"
        outcomes = self._transition_outcomes(child.state, child.action)
        tasks: List[Dict[str, Any]] = []
        for outcome_index, outcome in enumerate(outcomes):
            if len(outcome.children) != 1:
                tasks.append(
                    {
                        "index": len(tasks),
                        "group_index": index,
                        "outcome_index": outcome_index,
                        "kind": "outcome",
                        "depth": child.depth,
                        "action_code": int(child.action.pack()),
                        "actor_is_attacker": actor_is_attacker,
                        "reveal_level": outcome.reveal_level,
                        "reveal_card": outcome.reveal_card,
                    }
                )
                continue

            next_depth = child.depth - 1 if actor_is_attacker else child.depth
            for child_index, child_state in enumerate(outcome.children):
                if (
                    child_state.game.is_game_over()
                    or bool(child_state.game.board.waiting_noble)
                    or int(child_state.game.board.current_player) == self.attacker
                ):
                    tasks.append(
                        {
                            "index": len(tasks),
                            "group_index": index,
                            "outcome_index": outcome_index,
                            "child_index": child_index,
                            "kind": "state_after_root",
                            "depth": next_depth,
                            "action_code": int(child.action.pack()),
                            "actor_is_attacker": actor_is_attacker,
                            "reveal_level": outcome.reveal_level,
                            "reveal_card": outcome.reveal_card,
                        }
                    )
                    continue

                defender_actions = self._ordered_actions(
                    child_state,
                    self._helper._legal_actions(child_state),
                    next_depth,
                )
                if not defender_actions:
                    tasks.append(
                        {
                            "index": len(tasks),
                            "group_index": index,
                            "outcome_index": outcome_index,
                            "child_index": child_index,
                            "kind": "state_after_root",
                            "depth": next_depth,
                            "action_code": int(child.action.pack()),
                            "actor_is_attacker": actor_is_attacker,
                            "reveal_level": outcome.reveal_level,
                            "reveal_card": outcome.reveal_card,
                        }
                    )
                    continue

                for defender_action in defender_actions:
                    defender_outcomes = self._transition_outcomes(child_state, defender_action)
                    for defender_outcome_index, defender_outcome in enumerate(defender_outcomes):
                        tasks.append(
                            {
                                "index": len(tasks),
                                "group_index": index,
                                "outcome_index": outcome_index,
                                "child_index": child_index,
                                "defender_outcome_index": defender_outcome_index,
                                "kind": "defender_outcome",
                                "depth": next_depth,
                                "action_code": int(child.action.pack()),
                                "actor_is_attacker": actor_is_attacker,
                                "reveal_level": outcome.reveal_level,
                                "reveal_card": outcome.reveal_card,
                                "defender_action_code": int(defender_action.pack()),
                                "defender_reveal_level": defender_outcome.reveal_level,
                                "defender_reveal_card": defender_outcome.reveal_card,
                            }
                        )
        return tasks

    def _update_parallel_group(self, group: Dict[str, Any], branch: Dict[str, Any]) -> None:
        if group["kind"] == "single":
            group["done"] = True
            group["proof"] = int(branch.get("proof_number", INF))
            group["disproof"] = int(branch.get("disproof_number", INF))
            group["proof_tree"] = branch.get("proof_tree")
            group["refutation"] = branch.get("refutation")
            return

        # Root action nodes are AND nodes over their reveal/noble outcomes.
        if int(branch.get("disproof_number", INF)) == 0:
            group["done"] = True
            group["proof"] = self._sat_sum(group["proof_values"])
            group["disproof"] = 0
            group["refutation"] = branch.get("refutation")
            return

        if group["pending"] > 0:
            return

        group["done"] = True
        group["proof"] = self._sat_sum(group["proof_values"])
        group["disproof"] = min(group["disproof_values"]) if group["disproof_values"] else INF
        if group["proof"] == 0 and group["proof_children"]:
            group["proof_tree"] = {"kind": "action", "children": group["proof_children"]}
        if group["disproof"] == 0 and group["refutation_children"]:
            group["refutation"] = {"kind": "action", "children": group["refutation_children"]}

    def _worker_options_payload(self, batch_size: int) -> Dict[str, Any]:
        payload = asdict(self.options)
        elapsed = time.monotonic() - self._start_time
        if self.options.time_limit:
            payload["time_limit"] = max(0.001, self.options.time_limit - elapsed)
        if self.options.max_nodes:
            remaining = max(1, self.options.max_nodes - self.stats.nodes)
            payload["max_nodes"] = max(1, remaining // max(1, batch_size))
        payload["jobs"] = 1
        payload["include_proof"] = False
        payload["use_memo"] = bool(self.options.use_memo and self.parallel_tt_limit != 0)
        payload["_dfpn_threat_reveal_pruning"] = self.use_threat_reveal_pruning
        payload["_dfpn_lazy_reveal_pruning"] = self.use_lazy_reveal_pruning
        payload["_dfpn_attacker_dependency_pruning"] = self.use_attacker_dependency_pruning
        payload["_dfpn_defender_relevance_pruning"] = self.use_defender_relevance_pruning
        payload["_dfpn_equivalence_hash"] = self.use_equivalence_hash
        payload["_dfpn_return_pattern_pruning"] = self.use_return_pattern_pruning
        payload["_dfpn_upper_bound_pruning"] = self.use_upper_bound_pruning
        payload["_dfpn_immediate_terminal_pruning"] = self.use_immediate_terminal_pruning
        payload["_dfpn_defender_threat_filter"] = self.use_defender_threat_filter
        payload["_dfpn_max_actions_per_node"] = self.max_actions_per_node
        payload["_dfpn_target_candidate_limit"] = self.target_candidate_limit
        payload["_dfpn_max_tt_entries"] = max(0, int(self.parallel_tt_limit))
        return payload

    def _run_parallel_tasks_dynamic(
        self,
        tasks: Sequence[Dict[str, Any]],
        state_payload: Dict[str, Any],
        jobs: int,
    ):
        next_index = 0
        active: Dict[Any, Dict[str, Any]] = {}
        progress_stride = 14
        ctx = mp.get_context(self.parallel_start_method) if self.parallel_start_method else mp.get_context()
        progress_array = ctx.Array("q", jobs * progress_stride, lock=False)

        def clear_progress_slot(slot: int) -> None:
            offset = int(slot) * progress_stride
            for idx in range(progress_stride):
                progress_array[offset + idx] = 0
        def emit_parallel_progress(force: bool = False) -> None:
            extra = f"done={next_index - len(active)}/{len(tasks)} active={len(active)}"
            display = DFPNStats(**asdict(self.stats))
            if progress_array is not None:
                for slot in range(jobs):
                    offset = slot * progress_stride
                    display.nodes += int(progress_array[offset + 0])
                    display.expansions += int(progress_array[offset + 1])
                    display.memo_hits += int(progress_array[offset + 2])
                    display.terminal_nodes += int(progress_array[offset + 3])
                    display.reveal_branches += int(progress_array[offset + 4])
                    display.legal_moves += int(progress_array[offset + 5])
                    display.max_depth_reached = max(display.max_depth_reached, int(progress_array[offset + 6]))
                display.dangerous_reveals += int(progress_array[offset + 7])
                display.safe_reveal_collapses += int(progress_array[offset + 8])
                display.threat_pruned_reveals += int(progress_array[offset + 9])
                if progress_stride > 10:
                    display.action_pruned += int(progress_array[offset + 10])
                    display.return_pattern_pruned += int(progress_array[offset + 11])
                    display.upper_bound_prunes += int(progress_array[offset + 12])
                    display.immediate_terminal_prunes += int(progress_array[offset + 13])
            self.progress.maybe_emit(display, self._start_time, root=None, force=force, extra=extra)

        pool_kwargs: Dict[str, Any] = {
            "max_workers": jobs,
            "initializer": _parallel_worker_init,
            "initargs": (
                state_payload,
                self.attacker,
                self.max_depth,
                self._worker_options_payload(jobs),
                progress_array,
                progress_stride,
            ),
        }
        if self.parallel_start_method:
            pool_kwargs["mp_context"] = ctx
        executor = ProcessPoolExecutor(**pool_kwargs)
        try:
            while next_index < len(tasks) and len(active) < jobs:
                self._check_limits()
                task = tasks[next_index]
                task["progress_slot"] = len(active)
                active[executor.submit(_parallel_root_worker, task)] = task
                next_index += 1

            while active:
                done, _ = wait(
                    active.keys(),
                    timeout=self.progress.interval if self.progress.enabled else None,
                    return_when=FIRST_COMPLETED,
                )
                if not done:
                    emit_parallel_progress(force=True)
                    continue
                for future in done:
                    task = active.pop(future, None)
                    completed_slot = int(task.get("progress_slot", 0)) if task is not None else 0
                    branch = future.result()
                    clear_progress_slot(completed_slot)
                    emit_parallel_progress(force=True)
                    yield branch

                    if next_index < len(tasks):
                        self._check_limits()
                        next_task = tasks[next_index]
                        next_task["progress_slot"] = completed_slot
                        active[executor.submit(_parallel_root_worker, next_task)] = next_task
                        next_index += 1

                gc.collect()
        finally:
            emit_parallel_progress(force=True)
            for future in active:
                future.cancel()
            try:
                executor.shutdown(wait=False, cancel_futures=True)
            except TypeError:
                executor.shutdown(wait=False)

    def _merge_worker_stats(self, stats: Dict[str, Any]) -> None:
        self.stats.nodes += int(stats.get("nodes", 0))
        self.stats.memo_hits += int(stats.get("memo_hits", 0))
        self.stats.terminal_nodes += int(stats.get("terminal_nodes", 0))
        self.stats.reveal_branches += int(stats.get("reveal_branches", 0))
        self.stats.legal_moves += int(stats.get("legal_moves", 0))
        self.stats.expansions += int(stats.get("expansions", 0))
        self.stats.tt_hits += int(stats.get("tt_hits", 0))
        self.stats.dangerous_reveals += int(stats.get("dangerous_reveals", 0))
        self.stats.safe_reveal_collapses += int(stats.get("safe_reveal_collapses", 0))
        self.stats.threat_pruned_reveals += int(stats.get("threat_pruned_reveals", 0))
        self.stats.dangerous_reveal_collapses += int(stats.get("dangerous_reveal_collapses", 0))
        self.stats.lazy_reveal_branches += int(stats.get("lazy_reveal_branches", 0))
        self.stats.lazy_reveal_refinements += int(stats.get("lazy_reveal_refinements", 0))
        self.stats.lazy_reveal_pruned += int(stats.get("lazy_reveal_pruned", 0))
        self.stats.lazy_action_pruned += int(stats.get("lazy_action_pruned", 0))
        self.stats.lazy_action_refinements += int(stats.get("lazy_action_refinements", 0))
        self.stats.action_pruned += int(stats.get("action_pruned", 0))
        self.stats.return_pattern_pruned += int(stats.get("return_pattern_pruned", 0))
        self.stats.upper_bound_prunes += int(stats.get("upper_bound_prunes", 0))
        self.stats.immediate_terminal_prunes += int(stats.get("immediate_terminal_prunes", 0))
        self.stats.max_depth_reached = max(
            self.stats.max_depth_reached,
            int(stats.get("max_depth_reached", 0)),
        )

    def _root_tree(self, root: _DFPNNode, children: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
        tree = self._node_summary(root)
        tree["children"] = [child for child in children if child is not None]
        return tree

    def _result_from_root(self, root: _DFPNNode) -> SearchResult:
        self.stats.root_proof_number = int(root.proof)
        self.stats.root_disproof_number = int(root.disproof)
        if root.proof == 0:
            proof = self._extract_tree(root, want_proof=True) if self.options.include_proof else None
            result = SearchResult(MATE, self.max_depth, proof, None, self._finish_stats())
        elif root.disproof == 0:
            refutation = self._extract_tree(root, want_proof=False) if self.options.include_proof else None
            result = SearchResult(NO_MATE, None, None, refutation, self._finish_stats())
        else:
            self.stats.unknown_reason = "dfpn frontier exhausted without proof/disproof"
            result = SearchResult(UNKNOWN, None, None, None, self._finish_stats())
        self._clear_search_memory(root)
        return result

    def _check_limits(self) -> None:
        if self.options.max_nodes and self.stats.nodes >= self.options.max_nodes:
            raise SearchLimitExceeded("node limit exceeded")
        if (
            self.options.time_limit
            and self._start_time
            and (time.monotonic() - self._start_time) >= self.options.time_limit
        ):
            raise SearchLimitExceeded("time limit exceeded")

    def _worker_maybe_emit_progress(self) -> None:
        if _WORKER_PROGRESS_ARRAY is None or _WORKER_PROGRESS_SLOT < 0 or _WORKER_PROGRESS_STRIDE <= 0:
            return
        offset = _WORKER_PROGRESS_SLOT * _WORKER_PROGRESS_STRIDE
        try:
            _WORKER_PROGRESS_ARRAY[offset + 0] = int(self.stats.nodes)
            _WORKER_PROGRESS_ARRAY[offset + 1] = int(self.stats.expansions)
            _WORKER_PROGRESS_ARRAY[offset + 2] = int(self.stats.memo_hits)
            _WORKER_PROGRESS_ARRAY[offset + 3] = int(self.stats.terminal_nodes)
            _WORKER_PROGRESS_ARRAY[offset + 4] = int(self.stats.reveal_branches)
            _WORKER_PROGRESS_ARRAY[offset + 5] = int(self.stats.legal_moves)
            _WORKER_PROGRESS_ARRAY[offset + 6] = int(self.stats.max_depth_reached)
            _WORKER_PROGRESS_ARRAY[offset + 7] = int(self.stats.dangerous_reveals)
            _WORKER_PROGRESS_ARRAY[offset + 8] = int(self.stats.safe_reveal_collapses)
            _WORKER_PROGRESS_ARRAY[offset + 9] = int(self.stats.threat_pruned_reveals)
            if _WORKER_PROGRESS_STRIDE > 10:
                _WORKER_PROGRESS_ARRAY[offset + 10] = int(self.stats.action_pruned)
                _WORKER_PROGRESS_ARRAY[offset + 11] = int(self.stats.return_pattern_pruned)
                _WORKER_PROGRESS_ARRAY[offset + 12] = int(self.stats.upper_bound_prunes)
                _WORKER_PROGRESS_ARRAY[offset + 13] = int(self.stats.immediate_terminal_prunes)
        except Exception:
            return

    def _state_table_key(self, state: SolverState) -> Tuple[Any, ...]:
        if not self.use_equivalence_hash:
            return self._helper._canonical_key(state)
        return self._equivalence_key(state)

    def _equivalence_key(self, state: SolverState) -> Tuple[Any, ...]:
        board = state.game.board
        players = []
        for player_idx in range(2):
            player = board.get_player(player_idx)
            players.append(
                (
                    int(player.points),
                    tuple(int(v) for v in player.gems),
                    tuple(int(v) for v in player.bonuses),
                    tuple(int(v) for v in player.reserved),
                    tuple(bool(v) for v in player.reserved_is_hidden),
                    int(player.reserved_count),
                    int(player.purchased_count),
                    tuple(sorted(int(v) for v in player.purchased_cards)),
                    tuple(sorted(int(v) for v in player.acquired_nobles)),
                )
            )

        return (
            "threat-v1",
            int(board.turn),
            int(board.current_player),
            bool(board.final_round),
            bool(board.waiting_noble),
            int(board.winner),
            tuple(int(v) for v in board.bank),
            tuple(
                tuple(self._card_equivalence_key(state, int(card_id), visible=True) for card_id in row)
                for row in board.visible
            ),
            tuple(sorted(int(noble_id) for noble_id in board.nobles)),
            tuple(players),
            tuple(self._unseen_equivalence_key(state, level) for level in range(3)),
            bool(state.game.simple_payment_mode),
            bool(state.game.blank_refill_mode),
        )

    def _unseen_equivalence_key(self, state: SolverState, level: int) -> Tuple[Any, ...]:
        counts: Dict[Tuple[Any, ...], int] = {}
        for card_id in state.unseen_by_level[level]:
            key = self._card_equivalence_key(state, int(card_id), visible=False)
            counts[key] = counts.get(key, 0) + 1
        return tuple(
            (key, count)
            for key, count in sorted(counts.items(), key=lambda item: repr(item[0]))
        )

    def _card_equivalence_key(
        self,
        state: SolverState,
        card_id: int,
        visible: bool,
    ) -> Tuple[Any, ...]:
        if card_id < 0:
            return ("empty",)
        if self._is_immediate_winning_reveal(state, card_id):
            return ("immediate-win", int(card_id))

        level, points, bonus, cost = self._card_info(card_id)
        player_terms = []
        for player_idx in range(2):
            player = state.game.board.get_player(player_idx)
            bonuses = self._fixed_ints(player.bonuses, 5)
            net_cost = tuple(
                max(0, cost[color] - bonuses[color])
                for color in range(5)
            )
            gems = self._fixed_ints(player.gems, 6)
            shortage = tuple(
                max(0, net_cost[color] - gems[color])
                for color in range(5)
            )
            gold_needed = sum(shortage)
            bonuses_after = self._fixed_ints(player.bonuses, 5)
            if 0 <= bonus < 5:
                bonuses_after[bonus] += 1
            noble_options = tuple(
                sorted(self._nobles_available_after_purchase(state, player_idx, bonuses_after))
            )
            player_terms.append(
                (
                    int(self._can_afford_card_cost(player, cost)),
                    net_cost,
                    shortage,
                    min(gold_needed, 6),
                    noble_options,
                )
            )

        # Visible safe cards keep slightly more information because they can be
        # bought/reserved directly; hidden safe cards are grouped more aggressively.
        visibility_term = "visible" if visible else "hidden"
        return (
            "safe-card",
            visibility_term,
            level,
            points,
            bonus,
            tuple(player_terms),
        )

    def _dfpn(self, node: _DFPNNode, proof_limit: int, disproof_limit: int) -> None:
        while node.proof < proof_limit and node.disproof < disproof_limit:
            self._check_limits()
            if not node.expanded:
                self._expand(node)
                self._update(node)
                self.progress.maybe_emit(self.stats, self._start_time, root=node)
                self._worker_maybe_emit_progress()
                if not node.children or node.proof == 0 or node.disproof == 0:
                    return
            if not node.children or node.proof == 0 or node.disproof == 0:
                return

            child, second = self._select_most_proving_child(node)
            if node.node_type == "OR":
                child_proof_limit = min(proof_limit, self._sat_add(second, 1))
                child_disproof_limit = self._sat_sub(disproof_limit, node.disproof - child.disproof)
            else:
                child_proof_limit = self._sat_sub(proof_limit, node.proof - child.proof)
                child_disproof_limit = min(disproof_limit, self._sat_add(second, 1))

            self._dfpn(child, max(1, child_proof_limit), max(1, child_disproof_limit))
            self._update(node)
            self.progress.maybe_emit(self.stats, self._start_time, root=node)
            self._worker_maybe_emit_progress()
            if self._prune_inactive_subtrees:
                self._prune_inactive_children(node, keep=child)

    def _state_node(self, state: SolverState, depth: int) -> _DFPNNode:
        key: Optional[Tuple[Any, int]] = None
        if self.options.use_memo:
            key = (self._state_table_key(state), depth)
            if key in self._state_table:
                self.stats.memo_hits += 1
                self.stats.tt_hits += 1
                return self._state_table[key]

        game = state.game
        board = game.board
        node_type = "OR"
        terminal = False
        terminal_winner: Optional[int] = None
        reason: Optional[str] = None
        proof = 1
        disproof = 1

        if game.is_game_over():
            terminal = True
            terminal_winner = int(game.winner)
            if terminal_winner == self.attacker:
                proof, disproof = 0, INF
            else:
                proof, disproof = INF, 0
        elif (
            self.use_immediate_terminal_pruning
            and self._has_immediate_terminal_win(state, int(board.current_player))
        ):
            terminal = True
            terminal_winner = int(board.current_player)
            reason = "immediate_terminal_win"
            self.stats.immediate_terminal_prunes += 1
            if terminal_winner == self.attacker:
                proof, disproof = 0, INF
            else:
                proof, disproof = INF, 0
        elif self.use_upper_bound_pruning and self._attacker_score_upper_bound(state, depth) < 15:
            terminal = True
            reason = "attacker_score_upper_bound_below_15"
            self.stats.upper_bound_prunes += 1
            proof, disproof = INF, 0
        elif bool(board.waiting_noble):
            node_type = "OR" if int(board.current_player) == self.attacker else "AND"
        elif int(board.current_player) == self.attacker:
            node_type = "OR"
            if depth <= 0:
                terminal = True
                reason = "attacker_depth_exhausted"
                proof, disproof = INF, 0
        else:
            node_type = "AND"

        node = _DFPNNode(
            kind="state",
            node_type=node_type,
            depth=depth,
            state=state,
            proof=proof,
            disproof=disproof,
            terminal=terminal,
            terminal_winner=terminal_winner,
            reason=reason,
        )
        if (
            self.options.use_memo
            and key is not None
            and (self.max_tt_entries is None or len(self._state_table) < self.max_tt_entries)
        ):
            self._state_table[key] = node
        return node

    def _action_node(
        self,
        state: SolverState,
        depth: int,
        action: cs.Action,
        actor_is_attacker: bool,
    ) -> _DFPNNode:
        return _DFPNNode(
            kind="action",
            node_type="AND",
            depth=depth,
            state=state,
            action=action,
            reason="attacker_action" if actor_is_attacker else "defender_action",
        )

    def _outcome_node(
        self,
        depth: int,
        action: cs.Action,
        outcome: _Outcome,
        actor_is_attacker: bool,
    ) -> _DFPNNode:
        return _DFPNNode(
            kind="outcome",
            node_type="OR" if actor_is_attacker else "AND",
            depth=depth,
            action=action,
            outcome=outcome,
        )

    def _expand(self, node: _DFPNNode) -> None:
        if node.expanded:
            return
        self._check_limits()
        node.expanded = True
        self.stats.nodes += 1
        self.stats.expansions += 1

        if node.kind == "state":
            self._expand_state(node)
        elif node.kind == "action":
            self._expand_action(node)
        elif node.kind == "lazy_reveal":
            self._expand_lazy_reveal(node)
        elif node.kind == "noble":
            self._expand_noble(node)
        elif node.kind == "outcome":
            self._expand_outcome(node)

    def _expand_state(self, node: _DFPNNode) -> None:
        assert node.state is not None
        state = node.state
        board = state.game.board
        self.stats.max_depth_reached = max(self.stats.max_depth_reached, self.max_depth - node.depth)

        if node.terminal:
            if node.terminal_winner is not None:
                self.stats.terminal_nodes += 1
            return

        if bool(board.waiting_noble):
            choices = [
                action
                for action in state.game.legal_actions
                if int(action.type) == int(cs.ActionType.VISIT_NOBLE)
            ]
            self.stats.legal_moves += len(choices)
            for action in self._ordered_actions(state, choices, node.depth):
                node.children.append(
                    _DFPNNode(
                        kind="noble",
                        node_type="OR",
                        depth=node.depth,
                        state=state,
                        action=action,
                    )
                )
            return

        actions = self._helper._legal_actions(state)
        actor_is_attacker = int(board.current_player) == self.attacker
        ordered, omitted = self._ordered_actions_with_omissions(state, actions, node.depth)
        node.omitted_actions = tuple(omitted)
        node.lazy_actions_materialized = not bool(omitted)
        for action in ordered:
            node.children.append(self._action_node(state, node.depth, action, actor_is_attacker))

    def _expand_action(self, node: _DFPNNode) -> None:
        assert node.state is not None
        assert node.action is not None
        actor_is_attacker = node.reason == "attacker_action"
        lazy = self._lazy_reveal_node(node.state, node.depth, node.action, actor_is_attacker)
        if lazy is not None:
            node.children.append(lazy)
            return
        outcomes = self._transition_outcomes(node.state, node.action)
        for outcome in outcomes:
            node.children.append(self._outcome_node(node.depth, node.action, outcome, actor_is_attacker))

    def _lazy_reveal_node(
        self,
        state: SolverState,
        depth: int,
        action: cs.Action,
        actor_is_attacker: bool,
    ) -> Optional[_DFPNNode]:
        if not self.use_lazy_reveal_pruning:
            return None
        level = self._visible_refill_level(action)
        if level is None:
            return None
        candidates = tuple(sorted(int(card_id) for card_id in state.unseen_by_level[level]))
        if not candidates:
            return None
        return _DFPNNode(
            kind="lazy_reveal",
            node_type="AND",
            depth=depth,
            state=state,
            action=action,
            reason="attacker_action" if actor_is_attacker else "defender_action",
            reveal_level=level,
            reveal_candidates=candidates,
        )

    def _can_use_lazy_reveal(self, state: SolverState, action: cs.Action) -> bool:
        if not self.use_lazy_reveal_pruning:
            return False
        level = self._visible_refill_level(action)
        return level is not None and bool(state.unseen_by_level[level])

    def _expand_lazy_reveal(self, node: _DFPNNode) -> None:
        assert node.state is not None
        assert node.action is not None
        assert node.reveal_level is not None
        if node.lazy_reveal_materialized:
            return
        outcome = self._apply_with_blank_reveal(
            node.state,
            node.action,
            node.reveal_level,
            node.reveal_candidates,
        )
        node.children.append(
            self._outcome_node(
                node.depth,
                node.action,
                outcome,
                node.reason == "attacker_action",
            )
        )
        self.stats.reveal_branches += 1
        self.stats.lazy_reveal_branches += 1
        self.stats.lazy_reveal_pruned += max(0, len(node.reveal_candidates) - 1)

    def _apply_with_blank_reveal(
        self,
        state: SolverState,
        action: cs.Action,
        level: int,
        candidates: Sequence[int],
    ) -> _Outcome:
        representative = self._blank_reveal_representative(state, candidates)
        game = state.game.clone_light()
        game.board.decks = self._helper._deck_order_for(state, level, representative)
        original_blank_mode = bool(game.blank_refill_mode)
        game.blank_refill_mode = True
        try:
            if not game.apply(action, False):
                raise RuntimeError("engine rejected a legal action during blank reveal transition")
        finally:
            game.blank_refill_mode = original_blank_mode

        # The blank child is an abstraction: the slot is unknown, but no concrete
        # card identity is committed until refinement.
        game.board.decks = [
            sorted(int(card_id) for card_id in unseen)
            for unseen in state.unseen_by_level
        ]
        raw_child = SolverState(game=game, unseen_by_level=state.unseen_by_level)
        children = self._helper._finalize_noble_choices(raw_child)
        return _Outcome(level, None, children)

    def _blank_reveal_representative(self, state: SolverState, candidates: Sequence[int]) -> Optional[int]:
        if not candidates:
            return None
        return max((int(card_id) for card_id in candidates), key=lambda card_id: self._safe_reveal_score(state, card_id))

    def _transition_outcomes(self, state: SolverState, action: cs.Action) -> List[_Outcome]:
        level = self._visible_refill_level(action)
        if level is None or not self.use_threat_reveal_pruning:
            return self._helper._transition_outcomes(state, action)

        candidates = sorted(int(card_id) for card_id in state.unseen_by_level[level])
        if not candidates:
            return [self._helper._apply_with_reveal(state, action, level, None)]

        dangerous: List[int] = []
        safe: List[int] = []
        for card_id in candidates:
            if self._is_immediate_winning_reveal(state, card_id):
                dangerous.append(card_id)
            else:
                safe.append(card_id)

        dangerous_representatives = self._dangerous_reveal_representatives(state, dangerous)
        outcomes = [
            self._helper._apply_with_reveal(state, action, level, card_id)
            for card_id in self._helper._ordered_reveals(dangerous_representatives)
        ]
        self.stats.dangerous_reveals += len(dangerous_representatives)

        if safe:
            representative = self._safe_reveal_representative(state, safe)
            outcomes.append(self._helper._apply_with_reveal(state, action, level, representative))
            self.stats.safe_reveal_collapses += 1
            self.stats.threat_pruned_reveals += max(0, len(safe) - 1)

        self.stats.reveal_branches += len(outcomes)
        return outcomes

    def _visible_refill_level(self, action: cs.Action) -> Optional[int]:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            return self._card_info(int(action.card_id))[0] - 1
        if action_type == int(cs.ActionType.PURCHASE) and not bool(action.from_reserved):
            return self._card_info(int(action.card_id))[0] - 1
        return None

    def _is_immediate_winning_reveal(self, state: SolverState, card_id: int) -> bool:
        _, points, bonus, cost = self._card_info(card_id)
        for player_idx in range(2):
            player = state.game.board.get_player(player_idx)
            if not self._can_afford_card_cost(player, cost):
                continue
            bonus_after = self._fixed_ints(player.bonuses, 5)
            if 0 <= bonus < 5:
                bonus_after[bonus] += 1
            noble_points = 3 if self._can_visit_noble_after_purchase(state, player_idx, bonus_after) else 0
            if int(player.points) + points + noble_points >= 15:
                return True
        return False

    def _dangerous_reveal_representatives(self, state: SolverState, card_ids: Sequence[int]) -> List[int]:
        groups: Dict[Tuple[Any, ...], Tuple[int, Tuple[int, int, int, int, int]]] = {}
        for card_id in card_ids:
            card_id = int(card_id)
            key = self._reveal_threat_key(state, card_id)
            score = self._safe_reveal_score(state, card_id)
            current = groups.get(key)
            if current is None or score > current[1]:
                groups[key] = (card_id, score)

        representatives = [card_id for card_id, _ in groups.values()]
        self.stats.dangerous_reveal_collapses += max(0, len(card_ids) - len(representatives))
        return representatives

    def _reveal_threat_key(self, state: SolverState, card_id: int) -> Tuple[Any, ...]:
        _, points, bonus, cost = self._card_info(card_id)
        player_terms = []
        for player_idx in range(2):
            player = state.game.board.get_player(player_idx)
            can_afford = self._can_afford_card_cost(player, cost)
            bonuses_after = self._fixed_ints(player.bonuses, 5)
            if 0 <= bonus < 5:
                bonuses_after[bonus] += 1
            noble_gain = self._can_visit_noble_after_purchase(state, player_idx, bonuses_after)
            immediate_win = can_afford and int(player.points) + points + (3 if noble_gain else 0) >= 15
            player_terms.append((int(immediate_win), int(can_afford), int(noble_gain)))
        return (points, bonus, tuple(player_terms))

    def _safe_reveal_representative(self, state: SolverState, card_ids: Sequence[int]) -> int:
        return max(card_ids, key=lambda card_id: self._safe_reveal_score(state, int(card_id)))

    def _safe_reveal_score(self, state: SolverState, card_id: int) -> Tuple[int, int, int, int, int]:
        _, points, bonus, cost = self._card_info(card_id)
        best_affordable_score = 0
        best_noble_gain = 0
        affordable_count = 0
        for player_idx in range(2):
            player = state.game.board.get_player(player_idx)
            if not self._can_afford_card_cost(player, cost):
                continue
            affordable_count += 1
            bonus_after = [int(v) for v in player.bonuses]
            if 0 <= bonus < 5:
                bonus_after[bonus] += 1
            noble_gain = 3 if self._can_visit_noble_after_purchase(state, player_idx, bonus_after) else 0
            best_noble_gain = max(best_noble_gain, noble_gain)
            best_affordable_score = max(best_affordable_score, int(player.points) + points + noble_gain)
        return (
            best_affordable_score,
            best_noble_gain,
            points,
            affordable_count,
            int(card_id),
        )

    @staticmethod
    def _can_afford_card(player: cs.PlayerState, card: cs.Card) -> bool:
        return DFPNMateSolver._can_afford_card_cost(player, DFPNMateSolver._card_cost(card))

    @staticmethod
    def _can_afford_card_cost(player: cs.PlayerState, cost: Sequence[int]) -> bool:
        gems = DFPNMateSolver._fixed_ints(player.gems, 6)
        bonuses = DFPNMateSolver._fixed_ints(player.bonuses, 5)
        gold_needed = 0
        for color in range(5):
            need = max(0, int(cost[color]) - bonuses[color])
            if need > gems[color]:
                gold_needed += need - gems[color]
        return gold_needed <= gems[5]

    def _can_visit_noble_after_purchase(
        self,
        state: SolverState,
        player_idx: int,
        bonuses_after: Sequence[int],
    ) -> bool:
        return bool(self._nobles_available_after_purchase(state, player_idx, bonuses_after))

    def _nobles_available_after_purchase(
        self,
        state: SolverState,
        player_idx: int,
        bonuses_after: Sequence[int],
    ) -> List[int]:
        acquired = set(int(v) for v in state.game.board.get_player(player_idx).acquired_nobles)
        available: List[int] = []
        for noble_id in state.game.board.nobles:
            noble_id = int(noble_id)
            if noble_id < 0 or noble_id in acquired:
                continue
            requirements = self._noble_requirement(noble_id)
            bonuses = self._fixed_ints(bonuses_after, 5)
            if all(bonuses[color] >= requirements[color] for color in range(5)):
                available.append(noble_id)
        return available

    def _expand_noble(self, node: _DFPNNode) -> None:
        assert node.state is not None
        assert node.action is not None
        child = self._helper._apply_noble_choice(node.state, node.action)
        node.children.append(self._state_node(child, node.depth))

    def _expand_outcome(self, node: _DFPNNode) -> None:
        assert node.outcome is not None
        next_depth = node.depth - 1 if node.node_type == "OR" else node.depth
        for child in node.outcome.children:
            node.children.append(self._state_node(child, next_depth))

    def _ordered_actions(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        depth: int,
    ) -> List[cs.Action]:
        ordered, _ = self._ordered_actions_with_omissions(state, actions, depth)
        return ordered

    def _ordered_actions_with_omissions(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        depth: int,
    ) -> Tuple[List[cs.Action], List[cs.Action]]:
        board = state.game.board
        player_idx = int(board.current_player)
        remaining_turns = self._remaining_player_turns(player_idx, depth)
        targets = self._target_card_scores(state, player_idx, remaining_turns)
        working = list(actions)
        if self.use_return_pattern_pruning:
            working = self._representative_payment_and_return_actions(
                state,
                working,
                player_idx,
                remaining_turns,
                targets,
            )
        if self.use_defender_threat_filter and player_idx != self.attacker:
            working = self._filter_defender_threat_responses(
                state,
                working,
                player_idx,
                remaining_turns,
                targets,
            )

        omitted: List[cs.Action] = []
        if self.use_attacker_dependency_pruning and player_idx == self.attacker:
            working, omitted = self._filter_attacker_dependency_actions(
                state,
                working,
                player_idx,
                remaining_turns,
                targets,
            )
        elif self.use_defender_relevance_pruning and player_idx != self.attacker:
            working, omitted = self._filter_defender_relevance_actions(
                state,
                working,
                player_idx,
                depth,
            )

        ordered = sorted(
            working,
            key=lambda action: (
                -self._move_order_score(state, action, player_idx, remaining_turns, targets),
                self._helper._action_order_key(action),
            ),
        )
        if self.max_actions_per_node and len(ordered) > self.max_actions_per_node:
            self.stats.action_pruned += len(ordered) - self.max_actions_per_node
            ordered = ordered[: self.max_actions_per_node]
        ordered_omitted = sorted(
            omitted,
            key=lambda action: (
                -self._move_order_score(state, action, player_idx, remaining_turns, targets),
                self._helper._action_order_key(action),
            ),
        )
        return ordered, ordered_omitted

    def _filter_attacker_dependency_actions(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> Tuple[List[cs.Action], List[cs.Action]]:
        if remaining_turns <= 1:
            return list(actions), []

        target_colors = self._target_dependency_colors(state, player_idx, targets)
        defender_threats = self._immediate_scoring_threat_cards(state, 1 - player_idx)
        kept: List[cs.Action] = []
        omitted: List[cs.Action] = []
        for action in actions:
            if self._attacker_action_in_dependency_cone(
                state,
                action,
                player_idx,
                remaining_turns,
                targets,
                target_colors,
                defender_threats,
            ):
                kept.append(action)
            else:
                omitted.append(action)

        min_keep = min(len(actions), max(6, len([a for a in actions if int(a.type) == int(cs.ActionType.PURCHASE)])))
        if len(kept) < min_keep:
            ordered = sorted(
                actions,
                key=lambda action: (
                    -self._move_order_score(state, action, player_idx, remaining_turns, targets),
                    self._helper._action_order_key(action),
                ),
            )
            promoted = set(int(action.pack()) for action in ordered[:min_keep])
            kept_codes = promoted | {int(action.pack()) for action in kept}
            kept = [action for action in actions if int(action.pack()) in kept_codes]
            omitted = [action for action in actions if int(action.pack()) not in kept_codes]

        self.stats.lazy_action_pruned += len(omitted)
        return kept, omitted

    def _filter_defender_relevance_actions(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        player_idx: int,
        depth: int,
    ) -> Tuple[List[cs.Action], List[cs.Action]]:
        attacker_turns = self._remaining_player_turns(self.attacker, depth)
        attacker_targets = self._target_card_scores(state, self.attacker, attacker_turns)
        attacker_target_ids = set(attacker_targets)
        attacker_target_colors = self._target_dependency_colors(state, self.attacker, attacker_targets)

        kept: List[cs.Action] = []
        omitted: List[cs.Action] = []
        for action in actions:
            if self._defender_action_is_race_relevant(
                state,
                action,
                player_idx,
                attacker_target_ids,
                attacker_target_colors,
            ):
                kept.append(action)
            else:
                omitted.append(action)

        if not kept and actions:
            ordered = sorted(
                actions,
                key=lambda action: (
                    -self._move_order_score(
                        state,
                        action,
                        player_idx,
                        self._remaining_player_turns(player_idx, depth),
                        self._target_card_scores(
                            state,
                            player_idx,
                            self._remaining_player_turns(player_idx, depth),
                        ),
                    ),
                    self._helper._action_order_key(action),
                ),
            )
            kept = ordered[:1]
            kept_codes = {int(action.pack()) for action in kept}
            omitted = [action for action in actions if int(action.pack()) not in kept_codes]

        self.stats.lazy_action_pruned += len(omitted)
        return kept, omitted

    def _defender_action_is_race_relevant(
        self,
        state: SolverState,
        action: cs.Action,
        player_idx: int,
        attacker_target_ids: set,
        attacker_target_colors: set,
    ) -> bool:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.PURCHASE):
            if self._action_reaches_15(state, action, player_idx):
                return True
            if not bool(action.from_reserved) and int(action.card_id) in attacker_target_ids:
                return True
            return False
        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            return int(action.card_id) in attacker_target_ids
        if action_type in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME)):
            return self._defender_take_exhausts_attacker_target_color(
                state,
                action,
                attacker_target_colors,
            )
        if action_type == int(cs.ActionType.RESERVE_DECK):
            return False
        return True

    def _defender_take_exhausts_attacker_target_color(
        self,
        state: SolverState,
        action: cs.Action,
        attacker_target_colors: set,
    ) -> bool:
        take = self._fixed_ints(action.take, 6)
        bank = self._fixed_ints(state.game.board.bank, 6)
        for color in range(5):
            if color in attacker_target_colors and take[color] > 0 and bank[color] <= take[color]:
                return True
        return False

    def _target_dependency_colors(
        self,
        state: SolverState,
        player_idx: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> set:
        player = state.game.board.get_player(player_idx)
        colors = set()
        ranked_targets = sorted(
            targets.items(),
            key=lambda item: (item[1][0], -item[1][1], -item[1][2], item[0]),
            reverse=True,
        )
        for card_id, (expected, _, _) in ranked_targets[:4]:
            if expected <= 0:
                continue
            _, lack = self._card_payment_gap(player, card_id)
            colors.update(color for color, amount in enumerate(lack) if amount > 0)
        for noble_id in state.game.board.nobles:
            noble_id = int(noble_id)
            if noble_id < 0:
                continue
            requirement = self._noble_requirement(noble_id)
            bonuses = self._fixed_ints(player.bonuses, 5)
            deficit = [
                color
                for color in range(5)
                if requirement[color] > bonuses[color]
            ]
            if 0 < len(deficit) <= 2:
                colors.update(deficit)
        return colors

    def _attacker_action_in_dependency_cone(
        self,
        state: SolverState,
        action: cs.Action,
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
        target_colors: set,
        defender_threats: set,
    ) -> bool:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.PURCHASE):
            return True
        if action_type == int(cs.ActionType.VISIT_NOBLE):
            return True
        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            card_id = int(action.card_id)
            if card_id in defender_threats:
                return True
            _, points, bonus, _ = self._card_info(card_id)
            if points > 0:
                return True
            return bonus in target_colors
        if action_type == int(cs.ActionType.RESERVE_DECK):
            return bool(defender_threats)
        if action_type in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME)):
            take = self._fixed_ints(action.take, 6)
            if any(take[color] > 0 and color in target_colors for color in range(5)):
                return self._token_progress_score(state, player_idx, action, targets) > 0
            return False
        return True

    def _remaining_player_turns(self, player_idx: int, depth: int) -> int:
        if player_idx == self.attacker:
            return max(0, int(depth))
        return max(1, int(depth))

    def _has_immediate_terminal_win(self, state: SolverState, player_idx: int) -> bool:
        game = state.game
        if int(game.board.current_player) != player_idx:
            return False
        terminal_action_types = {
            int(cs.ActionType.PURCHASE),
            int(cs.ActionType.VISIT_NOBLE),
        }
        for action in game.legal_actions:
            if int(action.type) not in terminal_action_types:
                continue
            next_game = game.clone_light()
            if not next_game.apply(action, False):
                continue
            if next_game.is_game_over() and int(next_game.winner) == player_idx:
                return True
            if not bool(next_game.board.waiting_noble):
                continue
            for noble_action in next_game.legal_actions:
                if int(noble_action.type) != int(cs.ActionType.VISIT_NOBLE):
                    continue
                noble_game = next_game.clone_light()
                if noble_game.apply(noble_action, False) and noble_game.is_game_over():
                    if int(noble_game.winner) == player_idx:
                        return True
        return False

    def _attacker_score_upper_bound(self, state: SolverState, depth: int) -> int:
        board = state.game.board
        attacker = board.get_player(self.attacker)
        base_score = int(attacker.points)
        turns = self._remaining_player_turns(int(board.current_player), depth)
        if turns <= 0:
            return base_score

        card_scores = [
            self._card_info(card_id)[1]
            for card_id in self._attacker_upper_bound_card_ids(state)
            if self._card_can_reach_with_relaxed_resources(attacker, card_id, turns)
        ]
        card_scores.sort(reverse=True)
        best_card_points = sum(card_scores[:turns])

        current_bonuses = self._fixed_ints(attacker.bonuses, 5)
        reachable_nobles = 0
        for noble_id in board.nobles:
            noble_id = int(noble_id)
            if noble_id < 0:
                continue
            requirement = self._noble_requirement(noble_id)
            bonus_deficit = sum(
                max(0, requirement[color] - current_bonuses[color])
                for color in range(5)
            )
            if bonus_deficit <= turns:
                reachable_nobles += 1
        best_noble_points = 3 * min(turns, reachable_nobles)
        return base_score + best_card_points + best_noble_points

    def _attacker_upper_bound_card_ids(self, state: SolverState) -> List[int]:
        board = state.game.board
        attacker = board.get_player(self.attacker)
        card_ids = {
            int(card_id)
            for row in board.visible
            for card_id in row
            if int(card_id) >= 0
        }
        card_ids.update(int(card_id) for card_id in attacker.reserved if int(card_id) >= 0)
        for level in state.unseen_by_level:
            card_ids.update(int(card_id) for card_id in level)
        return sorted(card_ids)

    def _card_can_reach_with_relaxed_resources(
        self,
        player: cs.PlayerState,
        card: Any,
        turns: int,
    ) -> bool:
        gems = self._fixed_ints(player.gems, 6)
        bonuses = self._fixed_ints(player.bonuses, 5)
        cost = self._card_cost(card)
        total_missing = 0
        for color in range(5):
            # Relaxation: previous purchases may add at most one bonus per turn,
            # and all token gains may be used on this card. This can only
            # overestimate reachability.
            need = max(0, cost[color] - bonuses[color] - turns)
            total_missing += max(0, need - gems[color])
        relaxed_tokens = gems[5] + turns * 4
        return total_missing <= relaxed_tokens

    def _representative_payment_and_return_actions(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> List[cs.Action]:
        groups: Dict[Tuple[Any, ...], List[cs.Action]] = {}
        for action in actions:
            groups.setdefault(self._representative_action_key(action), []).append(action)

        representatives: List[cs.Action] = []
        for grouped in groups.values():
            if len(grouped) == 1:
                representatives.append(grouped[0])
                continue
            best = max(
                grouped,
                key=lambda action: self._representative_action_score(
                    state,
                    action,
                    player_idx,
                    remaining_turns,
                    targets,
                ),
            )
            representatives.append(best)
            self.stats.return_pattern_pruned += len(grouped) - 1
        return self._collapse_equivalent_take_actions(representatives)

    def _representative_action_key(self, action: cs.Action) -> Tuple[Any, ...]:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.PURCHASE):
            return ("buy", int(action.card_id), bool(action.from_reserved))
        if action_type in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME)):
            return ("take", action_type, tuple(self._fixed_ints(action.take, 6)))
        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            return ("reserve-visible", int(action.card_id))
        if action_type == int(cs.ActionType.RESERVE_DECK):
            return ("reserve-deck", int(action.deck_level))
        if action_type == int(cs.ActionType.VISIT_NOBLE):
            return ("noble", int(action.noble_choice))
        return ("other", action_type, int(action.pack()))

    def _collapse_equivalent_take_actions(self, actions: Sequence[cs.Action]) -> List[cs.Action]:
        groups: Dict[Tuple[int, ...], List[cs.Action]] = {}
        passthrough: List[cs.Action] = []
        take_types = {
            int(cs.ActionType.TAKE_DIFFERENT),
            int(cs.ActionType.TAKE_SAME),
        }
        for action in actions:
            if int(action.type) in take_types:
                groups.setdefault(self._take_net_delta(action), []).append(action)
            else:
                passthrough.append(action)

        collapsed = list(passthrough)
        for grouped in groups.values():
            if len(grouped) == 1:
                collapsed.append(grouped[0])
                continue
            best = max(grouped, key=self._take_equivalence_score)
            collapsed.append(best)
            self.stats.return_pattern_pruned += len(grouped) - 1
        return collapsed

    def _take_net_delta(self, action: cs.Action) -> Tuple[int, ...]:
        take = self._fixed_ints(action.take, 6)
        returns = self._fixed_ints(action.return_gems, 6)
        return tuple(take[color] - returns[color] for color in range(6))

    def _take_equivalence_score(self, action: cs.Action) -> Tuple[int, int, int]:
        returns = self._fixed_ints(action.return_gems, 6)
        return (
            -returns[5],
            -sum(returns[:5]),
            -int(self._helper._action_order_key(action)[0]),
        )

    def _representative_action_score(
        self,
        state: SolverState,
        action: cs.Action,
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> int:
        base = self._move_order_score(state, action, player_idx, remaining_turns, targets)
        action_type = int(action.type)
        if action_type == int(cs.ActionType.PURCHASE):
            gold_used = sum(self._fixed_ints(action.gold_as, 5))
            return base - gold_used * 2000
        if action_type in (
            int(cs.ActionType.TAKE_DIFFERENT),
            int(cs.ActionType.TAKE_SAME),
            int(cs.ActionType.RESERVE_VISIBLE),
            int(cs.ActionType.RESERVE_DECK),
        ):
            returns = self._fixed_ints(action.return_gems, 6)
            return base - returns[5] * 2000 - sum(returns[:5]) * 200
        return base

    def _filter_defender_threat_responses(
        self,
        state: SolverState,
        actions: Sequence[cs.Action],
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> List[cs.Action]:
        attacker_threats = self._immediate_scoring_threat_cards(state, self.attacker)
        if not attacker_threats:
            return list(actions)

        kept: List[cs.Action] = []
        for action in actions:
            action_type = int(action.type)
            if action_type == int(cs.ActionType.PURCHASE):
                if self._action_reaches_15(state, action, player_idx):
                    kept.append(action)
                continue
            if action_type == int(cs.ActionType.RESERVE_VISIBLE) and int(action.card_id) in attacker_threats:
                kept.append(action)
                continue
            if action_type in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME)):
                if self._token_denies_attacker_threat(state, action, attacker_threats):
                    kept.append(action)
                continue

        if kept:
            self.stats.action_pruned += max(0, len(actions) - len(kept))
            return kept
        return list(actions)

    def _immediate_scoring_threat_cards(self, state: SolverState, player_idx: int) -> set:
        threats = set()
        player = state.game.board.get_player(player_idx)
        visible = [
            int(card_id)
            for row in state.game.board.visible
            for card_id in row
            if int(card_id) >= 0
        ]
        reserved = [int(card_id) for card_id in player.reserved if int(card_id) >= 0]
        for card_id in visible + reserved:
            if not self._can_afford_card_cost(player, self._card_cost(card_id)):
                continue
            expected, _, _ = self._card_expected_score(state, player_idx, card_id)
            if int(player.points) + expected >= 15:
                threats.add(card_id)
        return threats

    def _action_reaches_15(self, state: SolverState, action: cs.Action, player_idx: int) -> bool:
        if int(action.type) != int(cs.ActionType.PURCHASE):
            return False
        player = state.game.board.get_player(player_idx)
        expected, _, _ = self._card_expected_score(state, player_idx, int(action.card_id))
        return int(player.points) + expected >= 15

    def _token_denies_attacker_threat(
        self,
        state: SolverState,
        action: cs.Action,
        threat_cards: set,
    ) -> bool:
        take = self._fixed_ints(action.take, 6)
        if not any(take[:5]):
            return False
        attacker = state.game.board.get_player(self.attacker)
        for card_id in threat_cards:
            _, lack = self._card_payment_gap(attacker, int(card_id))
            for color in range(5):
                if lack[color] > 0 and take[color] > 0:
                    return True
        return False

    def _move_order_score(
        self,
        state: SolverState,
        action: cs.Action,
        player_idx: int,
        remaining_turns: int,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> int:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.VISIT_NOBLE):
            return 900000

        if action_type == int(cs.ActionType.PURCHASE):
            card_id = int(action.card_id)
            expected, turns, gap = self._card_expected_score(state, player_idx, card_id)
            horizon_bonus = 50000 if turns <= max(1, remaining_turns) else 0
            return 1000000 + horizon_bonus + expected * 10000 - turns * 500 - gap * 100

        if action_type == int(cs.ActionType.RESERVE_VISIBLE):
            card_id = int(action.card_id)
            expected, turns, gap = targets.get(
                card_id,
                self._card_expected_score(state, player_idx, card_id),
            )
            return 450000 + expected * 7000 - turns * 700 - gap * 150

        if action_type in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME)):
            progress_score = self._token_progress_score(state, player_idx, action, targets)
            return 300000 + progress_score

        if action_type == int(cs.ActionType.RESERVE_DECK):
            return 120000

        return 0

    def _target_card_scores(
        self,
        state: SolverState,
        player_idx: int,
        remaining_turns: int,
    ) -> Dict[int, Tuple[int, int, int]]:
        candidates = self._candidate_target_cards(state, player_idx)
        scored: Dict[int, Tuple[int, int, int]] = {}
        fallback: List[Tuple[int, int, int, int]] = []
        for card_id in candidates:
            expected, turns, gap = self._card_expected_score(state, player_idx, card_id)
            fallback.append((expected, -turns, -gap, card_id))
            if turns <= max(1, remaining_turns):
                scored[card_id] = (expected, turns, gap)

        if scored or not fallback:
            return scored

        # If nothing is realistically in horizon, still guide token moves toward
        # the best near-target instead of falling back to type-only ordering.
        fallback.sort(reverse=True)
        for expected, neg_turns, neg_gap, card_id in fallback[:3]:
            scored[card_id] = (expected, -neg_turns, -neg_gap)
        return scored

    def _candidate_target_cards(self, state: SolverState, player_idx: int) -> List[int]:
        board = state.game.board
        player = board.get_player(player_idx)
        reserved = {
            int(card_id)
            for card_id in player.reserved
            if int(card_id) >= 0
        }
        cards = {
            int(card_id): int(card_id) in reserved
            for row in board.visible
            for card_id in row
            if int(card_id) >= 0
        }
        for card_id in reserved:
            cards[card_id] = True

        limit = max(0, int(self.target_candidate_limit))
        if limit <= 0 or len(cards) <= limit:
            return sorted(cards)

        gems = self._fixed_ints(player.gems, 6)
        bonuses = self._fixed_ints(player.bonuses, 5)
        ranked: List[Tuple[int, int, int, int]] = []
        for card_id, is_reserved in cards.items():
            _, points, bonus, cost = self._card_info(card_id)
            missing = 0
            missing_colors = 0
            for color in range(5):
                lack = max(0, cost[color] - bonuses[color] - gems[color])
                missing += lack
                if lack > 0:
                    missing_colors += 1
            gap = max(0, missing - gems[5])
            # This only steers dependency pruning and token ordering. Prefer
            # scoring cards and reserved cards, then near-term bonus progress.
            score = (
                points * 1000
                + (200 if is_reserved else 0)
                + (100 if 0 <= bonus < 5 else 0)
                - gap * 80
                - missing_colors * 10
            )
            ranked.append((score, -gap, points, card_id))
        ranked.sort(reverse=True)
        return sorted(card_id for _, _, _, card_id in ranked[:limit])

    def _card_expected_score(
        self,
        state: SolverState,
        player_idx: int,
        card_id: int,
    ) -> Tuple[int, int, int]:
        player = state.game.board.get_player(player_idx)
        _, points, bonus, _ = self._card_info(card_id)
        gap, _ = self._card_payment_gap(player, card_id)
        token_turns = (gap + 2) // 3
        turns_to_purchase = token_turns + 1
        bonuses_after = self._fixed_ints(player.bonuses, 5)
        if 0 <= bonus < 5:
            bonuses_after[bonus] += 1
        noble_gain = 3 if self._can_visit_noble_after_purchase(state, player_idx, bonuses_after) else 0
        expected = points + noble_gain
        return expected, turns_to_purchase, gap

    def _token_progress_score(
        self,
        state: SolverState,
        player_idx: int,
        action: cs.Action,
        targets: Dict[int, Tuple[int, int, int]],
    ) -> int:
        if not targets:
            return 0

        player = state.game.board.get_player(player_idx)
        before_gems = self._fixed_ints(player.gems, 6)
        take = self._fixed_ints(action.take, 6)
        returns = self._fixed_ints(action.return_gems, 6)
        after_gems = [
            max(0, before_gems[i] + take[i] - returns[i])
            for i in range(6)
        ]

        best = 0
        for card_id, (expected, turns, _) in targets.items():
            gap_before, lack_before = self._card_payment_gap(player, card_id, before_gems)
            gap_after, lack_after = self._card_payment_gap(player, card_id, after_gems)
            progress = max(0, gap_before - gap_after)
            color_match = sum(
                min(max(0, int(action.take[color]) - int(action.return_gems[color])), lack_before[color])
                for color in range(5)
            )
            if progress <= 0 and color_match <= 0:
                continue
            score = (
                expected * 10000
                + progress * 5000
                + color_match * 1000
                - turns * 300
                - sum(lack_after) * 100
            )
            best = max(best, score)
        return best

    @staticmethod
    def _card_payment_gap(
        player: cs.PlayerState,
        card: Any,
        gems_override: Optional[Sequence[int]] = None,
    ) -> Tuple[int, Tuple[int, int, int, int, int]]:
        gems = DFPNMateSolver._fixed_ints(gems_override if gems_override is not None else player.gems, 6)
        bonuses = DFPNMateSolver._fixed_ints(player.bonuses, 5)
        cost = DFPNMateSolver._card_cost(card)
        lack = tuple(
            max(0, cost[color] - bonuses[color] - gems[color])
            for color in range(5)
        )
        gap = max(0, sum(lack) - gems[5])
        return gap, lack

    @staticmethod
    def _fixed_ints(values: Sequence[Any], length: int, default: int = 0) -> List[int]:
        try:
            return [int(values[idx]) for idx in range(length)]
        except Exception:
            out: List[int] = []
            for idx in range(length):
                try:
                    out.append(int(values[idx]))
                except Exception:
                    out.append(default)
            return out

    @staticmethod
    def _card_info(card_or_id: Any) -> Tuple[int, int, int, Tuple[int, int, int, int, int]]:
        card_id = int(card_or_id if isinstance(card_or_id, int) else card_or_id.id)
        info = _CARD_INFO_CACHE.get(card_id)
        if info is None:
            card = cs.get_card(card_id)
            info = (
                int(card.level),
                int(card.points),
                int(card.bonus),
                tuple(int(card.cost[idx]) for idx in range(5)),
            )
            _CARD_INFO_CACHE[card_id] = info
        return info

    @staticmethod
    def _card_cost(card_or_id: Any) -> Tuple[int, int, int, int, int]:
        return DFPNMateSolver._card_info(card_or_id)[3]

    @staticmethod
    def _noble_requirement(noble_id: int) -> Tuple[int, int, int, int, int]:
        noble_id = int(noble_id)
        requirement = _NOBLE_REQ_CACHE.get(noble_id)
        if requirement is None:
            noble = cs.get_noble(noble_id)
            requirement = tuple(int(noble.requirement[idx]) for idx in range(5))
            _NOBLE_REQ_CACHE[noble_id] = requirement
        return requirement

    def _update(self, node: _DFPNNode) -> None:
        if node.terminal:
            return
        if node.kind == "lazy_reveal" and self._lazy_reveal_needs_refinement(node):
            self._materialize_lazy_reveal(node)
        if node.kind == "state" and self._state_actions_need_refinement(node):
            self._materialize_state_actions(node)
        if not node.children:
            if node.node_type == "OR":
                node.proof, node.disproof = INF, 0
            else:
                node.proof, node.disproof = 0, INF
            return

        if node.node_type == "OR":
            node.proof = min(child.proof for child in node.children)
            node.disproof = self._sat_sum(child.disproof for child in node.children)
        else:
            node.proof = self._sat_sum(child.proof for child in node.children)
            node.disproof = min(child.disproof for child in node.children)

    def _state_actions_need_refinement(self, node: _DFPNNode) -> bool:
        if node.kind != "state" or node.lazy_actions_materialized or not node.omitted_actions:
            return False
        if not node.children:
            return True
        if node.node_type == "OR":
            return all(child.disproof == 0 for child in node.children)
        return all(child.proof == 0 for child in node.children)

    def _materialize_state_actions(self, node: _DFPNNode) -> None:
        assert node.state is not None
        actor_is_attacker = int(node.state.game.board.current_player) == self.attacker
        for action in node.omitted_actions:
            node.children.append(self._action_node(node.state, node.depth, action, actor_is_attacker))
        node.omitted_actions = ()
        node.lazy_actions_materialized = True
        self.stats.lazy_action_refinements += 1

    def _lazy_reveal_needs_refinement(self, node: _DFPNNode) -> bool:
        if node.kind != "lazy_reveal" or node.lazy_reveal_materialized:
            return False
        if node.reason == "defender_action" and node.depth <= 0:
            return False
        if not node.children:
            return False
        return any(child.proof == 0 or child.disproof == 0 for child in node.children)

    def _materialize_lazy_reveal(self, node: _DFPNNode) -> None:
        assert node.state is not None
        assert node.action is not None
        assert node.reveal_level is not None
        actor_is_attacker = node.reason == "attacker_action"
        outcomes = self._transition_outcomes(node.state, node.action)
        node.children.clear()
        for outcome in outcomes:
            node.children.append(
                self._outcome_node(node.depth, node.action, outcome, actor_is_attacker)
            )
        node.lazy_reveal_materialized = True
        self.stats.lazy_reveal_refinements += 1

    def _select_most_proving_child(self, node: _DFPNNode) -> Tuple[_DFPNNode, int]:
        if node.node_type == "OR":
            ordered = sorted(node.children, key=lambda child: (child.proof, child.disproof))
            second = ordered[1].proof if len(ordered) > 1 else INF
            return ordered[0], second
        ordered = sorted(node.children, key=lambda child: (child.disproof, child.proof))
        second = ordered[1].disproof if len(ordered) > 1 else INF
        return ordered[0], second

    def _extract_tree(self, node: _DFPNNode, want_proof: bool) -> Dict[str, Any]:
        data = self._node_summary(node)
        if not node.children:
            return data

        selected: List[_DFPNNode]
        if node.node_type == "OR":
            if want_proof:
                selected = [self._first_matching(node.children, proof_zero=True)]
            else:
                selected = [child for child in node.children if child.disproof == 0]
        else:
            if want_proof:
                selected = [child for child in node.children if child.proof == 0]
            else:
                selected = [self._first_matching(node.children, proof_zero=False)]

        data["children"] = [
            self._extract_tree(child, want_proof)
            for child in selected
            if child is not None
        ]
        return data

    def _node_summary(self, node: _DFPNNode) -> Dict[str, Any]:
        data: Dict[str, Any] = {
            "kind": node.kind,
            "node_type": node.node_type,
            "depth_remaining": node.depth,
            "proof_number": int(node.proof),
            "disproof_number": int(node.disproof),
        }
        if node.state is not None:
            data.update(self._helper._state_summary(node.state, node.depth))
        if node.action is not None:
            game = node.state.game if node.state is not None else None
            data["action"] = self._helper._action_summary(node.action, game)
        if node.outcome is not None:
            data["reveal_level"] = (
                None if node.outcome.reveal_level is None else node.outcome.reveal_level + 1
            )
            data["reveal_card"] = node.outcome.reveal_card
            data["noble_choice_count"] = len(node.outcome.children)
        if node.kind == "lazy_reveal":
            data["reveal_level"] = None if node.reveal_level is None else node.reveal_level + 1
            data["reveal_candidate_count"] = len(node.reveal_candidates)
            data["lazy_reveal_materialized"] = bool(node.lazy_reveal_materialized)
        if node.kind == "state" and node.omitted_actions:
            data["omitted_action_count"] = len(node.omitted_actions)
            data["lazy_actions_materialized"] = bool(node.lazy_actions_materialized)
        if node.terminal_winner is not None:
            data["terminal_winner"] = node.terminal_winner
        if node.reason is not None:
            data["reason"] = node.reason
        return data

    def _clear_search_memory(self, root: Optional[_DFPNNode] = None) -> None:
        if root is not None:
            self._release_tree(root)
        self._state_table.clear()
        gc.collect()

    def _prune_inactive_children(self, node: _DFPNNode, keep: _DFPNNode) -> None:
        if keep not in node.children:
            return
        for child in node.children:
            if child is keep:
                continue
            self._release_descendants(child)

    def _release_descendants(self, node: _DFPNNode) -> None:
        if not node.children:
            return
        stack = list(node.children)
        node.children.clear()
        if not node.terminal and node.proof != 0 and node.disproof != 0:
            node.expanded = False
        for child in stack:
            self._release_tree(child)

    @staticmethod
    def _release_tree(root: _DFPNNode) -> None:
        stack = [root]
        seen = set()
        while stack:
            node = stack.pop()
            ident = id(node)
            if ident in seen:
                continue
            seen.add(ident)
            stack.extend(node.children)
            node.children.clear()
            node.state = None
            node.action = None
            node.outcome = None

    @staticmethod
    def _first_matching(children: Sequence[_DFPNNode], proof_zero: bool) -> Optional[_DFPNNode]:
        for child in children:
            if proof_zero and child.proof == 0:
                return child
            if not proof_zero and child.disproof == 0:
                return child
        return children[0] if children else None

    @staticmethod
    def _sat_sum(values) -> int:
        total = 0
        for value in values:
            total += int(value)
            if total >= INF:
                return INF
        return total

    @staticmethod
    def _sat_add(a: int, b: int) -> int:
        return min(INF, int(a) + int(b))

    @staticmethod
    def _sat_sub(a: int, b: int) -> int:
        if a >= INF:
            return INF
        return max(1, int(a) - int(b))


def _parallel_worker_init(
    state_payload: Dict[str, Any],
    attacker: int,
    max_depth: int,
    options_payload: Dict[str, Any],
    progress_array=None,
    progress_stride: int = 0,
) -> None:
    global _WORKER_STATE, _WORKER_ATTACKER, _WORKER_MAX_DEPTH, _WORKER_OPTIONS
    global _WORKER_PROGRESS_ARRAY, _WORKER_PROGRESS_STRIDE
    _WORKER_STATE = _state_from_payload(state_payload)
    _WORKER_ATTACKER = int(attacker)
    _WORKER_MAX_DEPTH = int(max_depth)
    _WORKER_OPTIONS = dict(options_payload)
    _WORKER_PROGRESS_ARRAY = progress_array
    _WORKER_PROGRESS_STRIDE = int(progress_stride)


def _parallel_root_worker(task: Dict[str, Any]) -> Dict[str, Any]:
    global _WORKER_TASK_INDEX, _WORKER_PROGRESS_SLOT
    if _WORKER_STATE is None:
        raise RuntimeError("parallel worker was not initialized")
    _WORKER_TASK_INDEX = int(task["index"])
    _WORKER_PROGRESS_SLOT = int(task.get("progress_slot", -1))

    option_payload = dict(_WORKER_OPTIONS)
    use_threat_reveal_pruning = bool(option_payload.pop("_dfpn_threat_reveal_pruning", True))
    use_lazy_reveal_pruning = bool(option_payload.pop("_dfpn_lazy_reveal_pruning", True))
    use_attacker_dependency_pruning = bool(option_payload.pop("_dfpn_attacker_dependency_pruning", True))
    use_defender_relevance_pruning = bool(option_payload.pop("_dfpn_defender_relevance_pruning", True))
    use_equivalence_hash = bool(option_payload.pop("_dfpn_equivalence_hash", True))
    use_return_pattern_pruning = bool(option_payload.pop("_dfpn_return_pattern_pruning", True))
    use_upper_bound_pruning = bool(option_payload.pop("_dfpn_upper_bound_pruning", True))
    use_immediate_terminal_pruning = bool(option_payload.pop("_dfpn_immediate_terminal_pruning", True))
    use_defender_threat_filter = bool(option_payload.pop("_dfpn_defender_threat_filter", False))
    max_actions_per_node = int(option_payload.pop("_dfpn_max_actions_per_node", 0))
    target_candidate_limit = int(option_payload.pop("_dfpn_target_candidate_limit", 5))
    max_tt_entries = int(option_payload.pop("_dfpn_max_tt_entries", 0))
    options = SolverOptions(**option_payload)
    solver = DFPNMateSolver(_WORKER_ATTACKER, _WORKER_MAX_DEPTH, options)
    solver.stats = DFPNStats()
    solver._state_table = {}
    solver._start_time = time.monotonic()
    solver._helper.stats = solver.stats
    solver._helper._start_time = solver._start_time
    solver._prune_inactive_subtrees = True
    solver.use_threat_reveal_pruning = use_threat_reveal_pruning
    solver.use_lazy_reveal_pruning = use_lazy_reveal_pruning
    solver.use_attacker_dependency_pruning = use_attacker_dependency_pruning
    solver.use_defender_relevance_pruning = use_defender_relevance_pruning
    solver.use_equivalence_hash = use_equivalence_hash
    solver.use_return_pattern_pruning = use_return_pattern_pruning
    solver.use_upper_bound_pruning = use_upper_bound_pruning
    solver.use_immediate_terminal_pruning = use_immediate_terminal_pruning
    solver.use_defender_threat_filter = use_defender_threat_filter
    solver.max_actions_per_node = max(0, max_actions_per_node)
    solver.target_candidate_limit = max(0, target_candidate_limit)
    solver.max_tt_entries = max_tt_entries if max_tt_entries > 0 else None
    solver._last_worker_progress = 0.0

    action = cs.Action.unpack(int(task["action_code"]))
    depth = int(task["depth"])

    if task["kind"] == "action":
        node = solver._action_node(
            _WORKER_STATE,
            depth,
            action,
            actor_is_attacker=bool(task["actor_is_attacker"]),
        )
    elif task["kind"] == "state_after_root":
        root_outcome = solver._helper._apply_with_reveal(
            _WORKER_STATE,
            action,
            None if task.get("reveal_level") is None else int(task["reveal_level"]),
            None if task.get("reveal_card") is None else int(task["reveal_card"]),
        )
        child_state = root_outcome.children[int(task.get("child_index", 0))]
        node = solver._state_node(child_state, depth)
    elif task["kind"] == "defender_outcome":
        root_outcome = solver._helper._apply_with_reveal(
            _WORKER_STATE,
            action,
            None if task.get("reveal_level") is None else int(task["reveal_level"]),
            None if task.get("reveal_card") is None else int(task["reveal_card"]),
        )
        child_state = root_outcome.children[int(task.get("child_index", 0))]
        defender_action = cs.Action.unpack(int(task["defender_action_code"]))
        defender_outcome = solver._helper._apply_with_reveal(
            child_state,
            defender_action,
            None if task.get("defender_reveal_level") is None else int(task["defender_reveal_level"]),
            None if task.get("defender_reveal_card") is None else int(task["defender_reveal_card"]),
        )
        node = solver._outcome_node(
            depth,
            defender_action,
            defender_outcome,
            actor_is_attacker=False,
        )
    elif task["kind"] == "outcome":
        reveal_level = task.get("reveal_level")
        reveal_card = task.get("reveal_card")
        outcome = solver._helper._apply_with_reveal(
            _WORKER_STATE,
            action,
            None if reveal_level is None else int(reveal_level),
            None if reveal_card is None else int(reveal_card),
        )
        node = solver._outcome_node(
            depth,
            action,
            outcome,
            actor_is_attacker=bool(task["actor_is_attacker"]),
        )
    elif task["kind"] == "noble":
        node = _DFPNNode(
            kind="noble",
            node_type="OR",
            depth=depth,
            state=_WORKER_STATE,
            action=action,
        )
    else:
        raise ValueError(f"unknown parallel task kind: {task['kind']}")

    proof_tree = None
    refutation = None
    unknown_reason = None
    try:
        solver._dfpn(node, INF, INF)
        if options.include_proof and node.proof == 0:
            proof_tree = solver._extract_tree(node, want_proof=True)
        if options.include_proof and node.disproof == 0:
            refutation = solver._extract_tree(node, want_proof=False)
    except SearchLimitExceeded as exc:
        unknown_reason = str(exc)

    solver.stats.root_proof_number = int(node.proof)
    solver.stats.root_disproof_number = int(node.disproof)
    payload = {
        "index": int(task["index"]),
        "group_index": int(task.get("group_index", task["index"])),
        "pid": os.getpid(),
        "proof_number": int(node.proof),
        "disproof_number": int(node.disproof),
        "proof_tree": proof_tree,
        "refutation": refutation,
        "stats": asdict(solver.stats),
        "unknown_reason": unknown_reason,
    }
    solver._clear_search_memory(node)
    return payload


def solve_game_dfpn(
    game: cs.Game,
    attacker: int,
    max_depth: int,
    options: Optional[SolverOptions] = None,
    use_lazy_reveal_pruning: bool = True,
    use_attacker_dependency_pruning: bool = True,
    use_defender_relevance_pruning: bool = True,
    use_threat_reveal_pruning: bool = True,
    use_equivalence_hash: bool = True,
    use_return_pattern_pruning: bool = True,
    use_upper_bound_pruning: bool = True,
    use_immediate_terminal_pruning: bool = True,
    use_defender_threat_filter: bool = False,
    max_actions_per_node: int = 0,
    target_candidate_limit: int = 5,
    parallel_tt_limit: int = 10000,
    show_progress: bool = False,
    progress_interval: float = 1.0,
    parallel_start_method: str = "spawn",
) -> SearchResult:
    solver = DFPNMateSolver(attacker=attacker, max_depth=max_depth, options=options)
    solver.use_lazy_reveal_pruning = use_lazy_reveal_pruning
    solver.use_attacker_dependency_pruning = use_attacker_dependency_pruning
    solver.use_defender_relevance_pruning = use_defender_relevance_pruning
    solver.use_threat_reveal_pruning = use_threat_reveal_pruning
    solver.use_equivalence_hash = use_equivalence_hash
    solver.use_return_pattern_pruning = use_return_pattern_pruning
    solver.use_upper_bound_pruning = use_upper_bound_pruning
    solver.use_immediate_terminal_pruning = use_immediate_terminal_pruning
    solver.use_defender_threat_filter = use_defender_threat_filter
    solver.max_actions_per_node = max(0, int(max_actions_per_node))
    solver.target_candidate_limit = max(0, int(target_candidate_limit))
    solver.parallel_tt_limit = max(0, int(parallel_tt_limit))
    solver.progress = ProgressReporter(show_progress, progress_interval)
    solver.parallel_start_method = parallel_start_method
    return solver.solve(SolverState.from_game(game))


_solve_game_dfpn_impl = solve_game_dfpn


def solve_game_dfpn(*args: Any, **kwargs: Any) -> SearchResult:
    use_lazy_reveal_pruning = kwargs.pop(
        "use_lazy_reveal_pruning", _DFPN_DEFAULT_PRUNING["lazy_reveal"]
    )
    use_attacker_dependency_pruning = kwargs.pop(
        "use_attacker_dependency_pruning", _DFPN_DEFAULT_PRUNING["attacker_dependency"]
    )
    use_defender_relevance_pruning = kwargs.pop(
        "use_defender_relevance_pruning", _DFPN_DEFAULT_PRUNING["defender_relevance"]
    )
    use_return_pattern_pruning = kwargs.pop(
        "use_return_pattern_pruning", _DFPN_DEFAULT_PRUNING["return_pattern"]
    )
    use_upper_bound_pruning = kwargs.pop(
        "use_upper_bound_pruning", _DFPN_DEFAULT_PRUNING["upper_bound"]
    )
    use_immediate_terminal_pruning = kwargs.pop(
        "use_immediate_terminal_pruning", _DFPN_DEFAULT_PRUNING["immediate_terminal"]
    )
    use_defender_threat_filter = kwargs.pop(
        "use_defender_threat_filter", _DFPN_DEFAULT_PRUNING["defender_threat_filter"]
    )
    max_actions_per_node = kwargs.pop(
        "max_actions_per_node", _DFPN_DEFAULT_PRUNING["max_actions_per_node"]
    )
    target_candidate_limit = kwargs.pop(
        "target_candidate_limit", _DFPN_DEFAULT_PRUNING["target_candidate_limit"]
    )
    _DFPN_DEFAULT_PRUNING.update(
        {
            "lazy_reveal": bool(use_lazy_reveal_pruning),
            "attacker_dependency": bool(use_attacker_dependency_pruning),
            "defender_relevance": bool(use_defender_relevance_pruning),
            "return_pattern": bool(use_return_pattern_pruning),
            "upper_bound": bool(use_upper_bound_pruning),
            "immediate_terminal": bool(use_immediate_terminal_pruning),
            "defender_threat_filter": bool(use_defender_threat_filter),
            "max_actions_per_node": max(0, int(max_actions_per_node)),
            "target_candidate_limit": max(0, int(target_candidate_limit)),
        }
    )
    return _solve_game_dfpn_impl(
        *args,
        use_lazy_reveal_pruning=bool(use_lazy_reveal_pruning),
        use_attacker_dependency_pruning=bool(use_attacker_dependency_pruning),
        use_defender_relevance_pruning=bool(use_defender_relevance_pruning),
        use_return_pattern_pruning=bool(use_return_pattern_pruning),
        use_upper_bound_pruning=bool(use_upper_bound_pruning),
        use_immediate_terminal_pruning=bool(use_immediate_terminal_pruning),
        use_defender_threat_filter=bool(use_defender_threat_filter),
        max_actions_per_node=max(0, int(max_actions_per_node)),
        target_candidate_limit=max(0, int(target_candidate_limit)),
        **kwargs,
    )


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Search guaranteed Splendor mate with depth-first proof-number search."
    )
    state_group = parser.add_mutually_exclusive_group()
    state_group.add_argument("--state-json", help="JSON file describing an arbitrary state")
    state_group.add_argument("--position", help="USI position command or raw SPN text")
    state_group.add_argument("--position-file", help="file containing a USI position command or raw SPN")
    parser.add_argument("--seed", type=int, default=0, help="initial game seed when state-json is omitted")
    parser.add_argument("--moves", action="append", default=[], help="USI move list, comma-separated or repeated")
    parser.add_argument("--attacker", type=int, default=0, choices=(0, 1))
    parser.add_argument("--max-depth", type=int, required=True)
    parser.add_argument("--node-limit", type=int, default=200000)
    parser.add_argument("--time-limit", type=float, default=10.0)
    parser.add_argument(
        "--simple-payment",
        action="store_true",
        help="generate only canonical purchase payments that preserve gold when possible",
    )
    parser.add_argument(
        "--jobs",
        type=int,
        default=1,
        help="worker processes for root-parallel DFPN search; 0 uses CPU count",
    )
    parser.add_argument("--allow-deck-reserve", action="store_true")
    parser.add_argument(
        "--no-threat-reveal-pruning",
        action="store_true",
        help="disable immediate-win threat based reveal collapsing",
    )
    parser.add_argument(
        "--no-lazy-reveal-pruning",
        action="store_true",
        help="disable delayed blank reveal refinement before concrete reveal branching",
    )
    parser.add_argument(
        "--no-attacker-dependency-pruning",
        action="store_true",
        help="disable lazy pruning of attacker moves outside the score dependency cone",
    )
    parser.add_argument(
        "--no-defender-relevance-pruning",
        action="store_true",
        help="disable lazy deferral of defender moves that do not affect the attacker's race plan",
    )
    parser.add_argument(
        "--no-equivalence-hash",
        action="store_true",
        help="disable threat-equivalence hashing and use exact state keys",
    )
    parser.add_argument(
        "--no-return-pattern-pruning",
        action="store_true",
        help="Disable representative pruning for equivalent payment/return patterns.",
    )
    parser.add_argument(
        "--no-upper-bound-pruning",
        action="store_true",
        help="Disable pruning by the attacker's optimistic score upper bound.",
    )
    parser.add_argument(
        "--no-immediate-terminal-pruning",
        action="store_true",
        help="Disable immediate win/defense terminal checks before expanding children.",
    )
    parser.add_argument(
        "--defender-threat-filter",
        action="store_true",
        help="Only expand defender replies that address immediate attacker threats.",
    )
    parser.add_argument(
        "--max-actions-per-node",
        type=int,
        default=0,
        help="Optional cap after move ordering and pruning; 0 means no cap.",
    )
    parser.add_argument(
        "--target-candidate-limit",
        type=int,
        default=int(_DFPN_DEFAULT_PRUNING["target_candidate_limit"]),
        help="Limit scored target cards for dependency pruning; 0 disables the limit.",
    )
    parser.add_argument(
        "--parallel-tt-limit",
        type=int,
        default=10000,
        help="max transposition-table entries per parallel worker; 0 disables worker memo",
    )
    parser.add_argument(
        "--parallel-start-method",
        choices=("spawn", "fork", "forkserver"),
        default="spawn",
        help="multiprocessing start method for parallel DFPN workers",
    )
    parser.add_argument("--progress", action="store_true", help="print periodic progress to stderr")
    parser.add_argument("--progress-interval", type=float, default=1.0, help="progress output interval in seconds")
    parser.add_argument("--no-memo", action="store_true")
    parser.add_argument("--no-proof", action="store_true")
    parser.add_argument("--pretty", action="store_true")
    args = parser.parse_args(argv)

    try:
        if args.state_json:
            game = load_game_from_json(args.state_json)
        elif args.position:
            game = load_game_from_usi_text(args.position, seed=args.seed)
        elif args.position_file:
            game = load_game_from_usi_file(args.position_file, seed=args.seed)
        else:
            game = cs.Game(seed=args.seed)
        if args.simple_payment:
            game.simple_payment_mode = True
        apply_usi_moves(game, _parse_moves(args.moves))
        options = SolverOptions(
            max_nodes=args.node_limit,
            time_limit=args.time_limit,
            include_proof=not args.no_proof,
            allow_deck_reserve=args.allow_deck_reserve,
            use_memo=not args.no_memo,
            jobs=args.jobs,
        )
        _DFPN_DEFAULT_PRUNING.update(
            {
                "lazy_reveal": not args.no_lazy_reveal_pruning,
                "attacker_dependency": not args.no_attacker_dependency_pruning,
                "defender_relevance": not args.no_defender_relevance_pruning,
                "return_pattern": not args.no_return_pattern_pruning,
                "upper_bound": not args.no_upper_bound_pruning,
                "immediate_terminal": not args.no_immediate_terminal_pruning,
                "defender_threat_filter": args.defender_threat_filter,
                "max_actions_per_node": max(0, int(args.max_actions_per_node)),
                "target_candidate_limit": max(0, int(args.target_candidate_limit)),
            }
        )
        result = solve_game_dfpn(
            game,
            attacker=args.attacker,
            max_depth=args.max_depth,
            options=options,
            use_lazy_reveal_pruning=not args.no_lazy_reveal_pruning,
            use_attacker_dependency_pruning=not args.no_attacker_dependency_pruning,
            use_defender_relevance_pruning=not args.no_defender_relevance_pruning,
            use_threat_reveal_pruning=not args.no_threat_reveal_pruning,
            use_equivalence_hash=not args.no_equivalence_hash,
            use_return_pattern_pruning=not args.no_return_pattern_pruning,
            use_upper_bound_pruning=not args.no_upper_bound_pruning,
            use_immediate_terminal_pruning=not args.no_immediate_terminal_pruning,
            use_defender_threat_filter=args.defender_threat_filter,
            max_actions_per_node=max(0, int(args.max_actions_per_node)),
            target_candidate_limit=max(0, int(args.target_candidate_limit)),
            parallel_tt_limit=args.parallel_tt_limit,
            show_progress=args.progress,
            progress_interval=args.progress_interval,
            parallel_start_method=args.parallel_start_method,
        )
        print(json.dumps(result.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
        return 0 if result.status in (MATE, NO_MATE) else 2
    except Exception as exc:
        error = SearchResult(
            status=INVALID_INPUT,
            depth=None,
            proof_tree=None,
            refutation={"error": str(exc)},
            stats=SearchStats(),
        )
        print(json.dumps(error.to_dict(), indent=2 if args.pretty else None, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
