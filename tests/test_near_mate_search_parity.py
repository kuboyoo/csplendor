"""search_near_mate (now over the C++ near_mate_search) matches the Python
implementation it replaced, copied below verbatim as the reference.

Node limits only, so both are deterministic and every field except the
elapsed times must agree.
"""

from __future__ import annotations

import random
import time
from typing import Any, Optional

import pytest

import csplendor as cs
from csplendor import near_mate_probability_cpp
from csplendor.api.usi_kifu import action_to_usi
from csplendor.mate_depth import MateSearchCancellationToken
from csplendor.near_mate import NEAR_MATE_FORMAT
from tests.test_reveal_verified_solver import (
    _five_move_mate_fixture,
    _six_move_mate_fixture,
)


def _reference_search_near_mate(
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
    if attacker not in (0, 1):
        raise ValueError("attacker must be 0 or 1")
    if min_depth < 0 or max_depth < min_depth:
        raise ValueError("depth range must satisfy 0 <= min_depth <= max_depth")
    ladder = sorted({float(t) for t in thresholds}, reverse=True)
    if not ladder or ladder[0] > 1.0 or ladder[-1] <= 0.0:
        raise ValueError("thresholds must lie in (0, 1]")
    started = time.monotonic()
    attempts: list[dict[str, Any]] = []
    total_nodes = 0
    lower = 0.0
    lower_depth: Optional[int] = None
    best_action = None
    reveals: list[dict[str, Any]] = []
    root_actions: list[dict[str, Any]] = []
    # per depth: refuted upper bound and whether every threshold above the
    # proven one was decided (an undecided query leaves a gap)
    bounds_by_depth: dict[int, dict[str, Any]] = {}
    stop_reason = "max_depth_reached"
    undecided = False

    def budget_left() -> tuple[int, float, bool]:
        remaining_nodes = 0 if max_nodes == 0 else max(0, max_nodes - total_nodes)
        remaining_time = 0.0
        if time_limit_seconds > 0.0:
            remaining_time = time_limit_seconds - (time.monotonic() - started)
        exhausted = (max_nodes and remaining_nodes == 0) or (time_limit_seconds > 0.0 and remaining_time <= 0.0)
        return remaining_nodes, remaining_time, bool(exhausted)

    for depth in range(min_depth, max_depth + 1):
        depth_upper = 1.0
        depth_decided = True
        for threshold in ladder:
            if threshold <= lower:
                break  # already proven at a shallower depth
            remaining_nodes, remaining_time, exhausted = budget_left()
            if exhausted:
                stop_reason = "cumulative limit exceeded"
                depth_decided = False
                break
            raw = dict(
                near_mate_probability_cpp(
                    game,
                    attacker=attacker,
                    depth=depth,
                    max_nodes=remaining_nodes,
                    time_limit_seconds=remaining_time,
                    root_exact=False,
                    no_take_probe=no_take_probe,
                    line_transplant=line_transplant,
                    max_memo_states=max_memo_states,
                    order_hints=order_hints,
                    cancellation_token=cancellation_token,
                    alpha=max(0.0, threshold - 1e-6),
                    beta=threshold,
                )
            )
            nodes = int(raw["stats"].get("nodes", 0))
            total_nodes += nodes
            complete = bool(raw["complete"])
            proven = complete and float(raw["value"]) >= threshold
            attempts.append(
                {
                    "depth": depth,
                    "threshold": threshold,
                    "verdict": "proven" if proven else "refuted" if complete else "undecided",
                    "nodes": nodes,
                    "elapsed_ms": float(raw["stats"].get("elapsed_ms", 0.0)),
                    "unknown_reason": raw["unknown_reason"],
                }
            )
            if proven:
                lower = threshold
                lower_depth = depth
                best_action = raw["best_action"]
                reveals = [dict(item) for item in raw["best_action_reveals"]]
                root_actions = [dict(item) for item in raw["root_actions"]]
                break
            if complete:
                depth_upper = min(depth_upper, threshold)
            else:
                undecided = True
                depth_decided = False
                stop_reason = str(raw["unknown_reason"] or "search limit exceeded")
                break
        bounds_by_depth[depth] = {"lower": lower, "upper": depth_upper, "decided": depth_decided}
        if lower >= ladder[0]:
            stop_reason = "mate_proven"
            break
        if not depth_decided:
            break
    # The upper bound belongs to the deepest depth whose ladder was decided
    # (a deeper search can only raise the probability).
    upper = 1.0
    upper_depth: Optional[int] = None
    for depth in sorted(bounds_by_depth):
        entry = bounds_by_depth[depth]
        if entry["decided"] or entry["upper"] < 1.0:
            upper = entry["upper"] if entry["decided"] else max(entry["upper"], lower)
            upper_depth = depth
    if lower >= 1.0:
        upper, upper_depth = 1.0, lower_depth
    elif upper <= lower:
        upper = lower

    best_usi = None
    if best_action is not None and int(game.current_player) == attacker:
        legal_by_code = {int(action.pack()): action for action in game.legal_actions}
        action = legal_by_code.get(int(best_action))
        if action is not None:
            best_usi = action_to_usi(action, game=game)
    losing = [item for item in reveals if float(item["value"]) < 1.0]
    return {
        "format": NEAR_MATE_FORMAT,
        "attacker": attacker,
        "min_depth": min_depth,
        "max_depth": max_depth,
        "thresholds": ladder,
        "lower": float(lower),
        "upper": float(upper),
        "lower_depth": lower_depth,
        "upper_depth": upper_depth,
        "bounds_by_depth": bounds_by_depth,
        "is_mate": lower >= 1.0,
        "best_action": best_action,
        "best_action_usi": best_usi,
        "root_actions": root_actions,
        "losing_reveals": losing,
        "undecided": undecided,
        "stop_reason": stop_reason,
        "attempts": attempts,
        "stats": {"nodes": total_nodes, "elapsed_ms": (time.monotonic() - started) * 1000.0},
    }


def _without_times(result: dict) -> dict:
    result = dict(result)
    result["stats"] = {k: v for k, v in result["stats"].items() if k != "elapsed_ms"}
    result["attempts"] = [
        {k: v for k, v in attempt.items() if k != "elapsed_ms"} for attempt in result["attempts"]
    ]
    return result


def _late_position(seed: int) -> Optional[cs.Game]:
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    game.simple_payment_mode = False
    while not game.is_game_over():
        if max(int(p.points) for p in game.board.players) >= 11:
            return game
        actions = game.legal_actions
        game.apply(actions[rng.randrange(len(actions))], False)
    return None


def _cases():
    yield "six", _six_move_mate_fixture(), 0, 3, 6, 0
    yield "six-shallow", _six_move_mate_fixture(), 0, 3, 5, 0
    yield "five", _five_move_mate_fixture(), 0, 4, 5, 0
    # A cumulative node budget that runs out between two queries.
    yield "late49-400", _late_position(49), int(_late_position(49).current_player), 1, 3, 400
    for seed in range(1, 30):
        game = _late_position(seed)
        if game is not None:
            for max_nodes in (3_000, 60_000):
                yield f"late{seed}-{max_nodes}", game, int(game.current_player), 1, 3, max_nodes


@pytest.mark.parametrize("name, game, attacker, min_depth, max_depth, max_nodes",
                         list(_cases()), ids=lambda v: v if isinstance(v, str) else None)
def test_matches_the_python_implementation(name, game, attacker, min_depth, max_depth, max_nodes):
    kwargs = dict(attacker=attacker, min_depth=min_depth, max_depth=max_depth, max_nodes=max_nodes)
    assert _without_times(cs.search_near_mate(game, **kwargs)) == _without_times(
        _reference_search_near_mate(game, **kwargs)
    )


def test_the_native_result_carries_the_v4_id_of_the_best_action():
    game = _six_move_mate_fixture()
    config = cs.NearMateSearchConfig()
    config.attacker, config.min_depth, config.max_depth = 0, 6, 6
    result = cs.near_mate_search(game, config)
    assert result.lower == 1.0 and result.stop_reason == "mate_proven"
    action = cs.Action.unpack(result.best_action)
    assert result.best_action_id == cs.ActionEncoderV4.encode(action, game)


@pytest.mark.parametrize("thresholds", [(), (1.2,), (0.0,)])
def test_invalid_thresholds_are_rejected(thresholds):
    with pytest.raises(ValueError, match="thresholds"):
        cs.search_near_mate(_six_move_mate_fixture(), attacker=0, min_depth=1,
                            max_depth=1, thresholds=thresholds)
