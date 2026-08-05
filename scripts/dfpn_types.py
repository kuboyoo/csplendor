"""Value types shared by DFPN search, proof and reporting components."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import csplendor as cs
from scripts.mate_solver import SearchStats, SolverState, _Outcome


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
    final_round_prunes: int = 0


@dataclass
class RevealVerifiedStats(SearchStats):
    candidate_nodes: int = 0
    candidate_elapsed_ms: float = 0.0
    verification_nodes: int = 0
    verification_elapsed_ms: float = 0.0
    final_round_reveal_collapses: int = 0
    final_round_score_prunes: int = 0
    final_round_direct_resolutions: int = 0
    oracle_purchase_actions: int = 0
    oracle_reserve_actions: int = 0
    deck_reserve_candidates: int = 0
    deck_reserve_branches: int = 0


@dataclass
class DFPNNode:
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
    children: List["DFPNNode"] = field(default_factory=list)


class SearchLimitExceeded(Exception):
    """Raised when the node or elapsed-time boundary is reached."""


# The historical private name remains available to compatibility imports.
_DFPNNode = DFPNNode


__all__ = [
    "DFPNNode",
    "DFPNStats",
    "RevealVerifiedStats",
    "SearchLimitExceeded",
    "_DFPNNode",
]
