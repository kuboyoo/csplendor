"""Value types of the reveal-verified mate search driver."""

from __future__ import annotations

from dataclasses import dataclass

from scripts.mate_solver import SearchStats


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


class SearchLimitExceeded(Exception):
    """Raised when the node or elapsed-time boundary is reached."""


__all__ = ["RevealVerifiedStats", "SearchLimitExceeded"]
