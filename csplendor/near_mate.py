"""Probabilistic ("near") mate search built on the reveal-verified solver.

``near_mate_probability_cpp`` evaluates one depth: the probability (a sound
lower bound) that the attacker forces a win within that many of its own
moves, against best defence, with each deck reveal uniform over the unseen
cards of its tier (equivalence classes weighted by their card counts). A value
of 1.0 is an exact mate.

Exact probabilities need the whole tree, so ``search_near_mate`` asks
threshold questions instead: with the window ``(T - eps, T)`` the solver
prunes like a proof search (a defender reply or a set of reveals that pushes
the mean below ``T`` closes the node, a move reaching ``T`` closes an attacker
node). Descending thresholds give a bracket ``lower <= p < upper`` per depth;
the lower bound is monotone in depth. The bracketing itself is the C++
``near_mate_search`` (src/near_mate_probe.h), shared with the WASM build;
this module only shapes its result.
"""

from __future__ import annotations

from typing import Any, Optional

from ._csplendor import NearMateSearchConfig, near_mate_search
from .api.usi_kifu import action_to_usi
from .mate_depth import MateSearchCancellationToken

NEAR_MATE_FORMAT = "csplendor_near_mate_v1"


def search_near_mate(
    game: Any,
    *,
    attacker: int,
    min_depth: int,
    max_depth: int,
    thresholds: tuple[float, ...] = (1.0, 0.9, 0.75, 0.5),
    max_nodes: int = 0,
    time_limit_seconds: float = 0.0,
    no_take_probe: bool = False,
    line_transplant: bool = True,
    max_memo_states: int = 0,
    order_hints: Any = None,
    cancellation_token: Optional[MateSearchCancellationToken] = None,
) -> dict[str, Any]:
    """Bracket the forced-win probability per depth with threshold queries.

    For each depth the thresholds are tried from the highest down until one
    is proven (``p >= T``): that is the depth's lower bound and the search
    stops there (a higher depth can only raise it). Refuted thresholds give
    the upper bound ``p < T``. The result's ``lower``/``upper`` summarise the
    best depth, ``best_action`` and ``losing_reveals`` come from the proven
    query. A query cut by the limits leaves its threshold undecided.
    """
    config = NearMateSearchConfig()
    config.attacker = attacker
    config.min_depth = min_depth
    config.max_depth = max_depth
    config.thresholds = [float(t) for t in thresholds]
    config.max_nodes = max_nodes
    config.time_limit_seconds = time_limit_seconds
    config.no_take_probe = no_take_probe
    config.line_transplant = line_transplant
    config.max_memo_states = max_memo_states
    raw = near_mate_search(
        game, config, order_hints=order_hints, cancellation_token=cancellation_token
    )

    best_action = int(raw.best_action) if raw.has_best_action else None
    best_usi = None
    if best_action is not None and int(game.current_player) == attacker:
        legal_by_code = {int(action.pack()): action for action in game.legal_actions}
        action = legal_by_code.get(best_action)
        if action is not None:
            best_usi = action_to_usi(action, game=game)
    reveals = [
        {"card": None if r.card < 0 else int(r.card), "weight": int(r.weight), "value": float(r.value)}
        for r in raw.best_action_reveals
    ]
    return {
        "format": NEAR_MATE_FORMAT,
        "attacker": attacker,
        "min_depth": min_depth,
        "max_depth": max_depth,
        "thresholds": list(raw.thresholds),
        "lower": float(raw.lower),
        "upper": float(raw.upper),
        "lower_depth": None if raw.lower_depth < 0 else int(raw.lower_depth),
        "upper_depth": None if raw.upper_depth < 0 else int(raw.upper_depth),
        "bounds_by_depth": {
            int(b.depth): {"lower": float(b.lower), "upper": float(b.upper), "decided": bool(b.decided)}
            for b in raw.bounds
        },
        "is_mate": raw.lower >= 1.0,
        "best_action": best_action,
        "best_action_usi": best_usi,
        "root_actions": [
            {"action_code": int(a.action_code), "value": float(a.value),
             "lower": float(a.lower), "upper": float(a.upper)}
            for a in raw.root_actions
        ],
        "losing_reveals": [item for item in reveals if item["value"] < 1.0],
        "undecided": bool(raw.undecided),
        "stop_reason": str(raw.stop_reason),
        "attempts": [
            {
                "depth": int(a.depth),
                "threshold": float(a.threshold),
                "verdict": str(a.verdict),
                "nodes": int(a.nodes),
                "elapsed_ms": float(a.elapsed_ms),
                "unknown_reason": a.unknown_reason or None,
            }
            for a in raw.attempts
        ],
        "stats": {"nodes": int(raw.nodes), "elapsed_ms": float(raw.elapsed_ms)},
    }
