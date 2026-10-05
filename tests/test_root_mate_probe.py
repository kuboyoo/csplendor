"""root_mate_probe (C++) matches the Python root mate probe it replaces.

The reference below is dlsplendor's ``RootMateSearch.probe`` with a fresh
``MateSearchSession(jobs=1)`` per call.  With node limits only (no time
limit) both are deterministic, so every reported field must agree.
"""

from __future__ import annotations

import random
import time
from typing import Any, Dict, Optional

import pytest

import csplendor as cs
from scripts.puzzle_engine_adapter import RandomPurchasePlayer


def _reference_probe(
    game: cs.Game,
    config: cs.RootMateProbeConfig,
    previous_value: Optional[float] = None,
) -> Dict[str, Any]:
    out: Dict[str, Any] = {
        "attempted": False, "proven": False, "value_proven": False,
        "depth": -1, "action_code": -1, "action_id": -1, "nodes": 0,
        "stop_reason": "", "value_triggered": False,
    }
    if not config.enabled:
        return out
    attacker = int(game.current_player)
    points = int(game.board.players[attacker].points)
    trigger = config.trigger_value
    value_triggered = (
        points < config.min_points
        and trigger is not None
        and previous_value is not None
        and previous_value >= trigger
    )
    if points < config.min_points and not value_triggered:
        return out
    out["value_triggered"] = value_triggered
    opponent = game.board.players[1 - attacker]
    if any(int(c) >= 0 and bool(h) for c, h in zip(opponent.reserved, opponent.reserved_is_hidden)):
        out["stop_reason"] = "opponent_hidden_reserve"
        return out
    endgame = value_triggered or (config.endgame_points > 0 and points >= config.endgame_points)
    max_nodes = config.endgame_max_nodes if endgame and config.endgame_max_nodes > 0 else config.max_nodes
    session = cs.MateSearchSession(
        attacker=attacker,
        jobs=1,
        max_cache_states=config.max_cache_states,
        warm_start_nodes=config.warm_start_nodes,
        warm_start_time_seconds=0.0,
    )
    result = session.search_anytime(
        game, min_depth=config.min_depth, max_depth=config.max_depth,
        max_nodes=max_nodes, time_limit_seconds=0.0,
    )
    out["attempted"] = True
    out["depth"] = -1 if result["mate_depth"] is None else int(result["mate_depth"])
    out["nodes"] = int(result["stats"]["nodes"])
    out["stop_reason"] = str(result["stop_reason"])
    if result["status"] != "mate" and endgame:
        exact = cs.search_reveal_verified_mate_depths(
            game, attacker=attacker, min_depth=1, max_depth=config.endgame_exact_depth,
            max_nodes=max_nodes, time_limit_seconds=0.0, max_memo_states=max(1, max_nodes),
        )
        out["nodes"] += int(exact["stats"]["nodes"])
        if exact["status"] == "mate":
            result = exact
            out["depth"] = int(exact["mate_depth"])
            out["stop_reason"] = "exact_endgame_mate"
    if result["status"] != "mate":
        return out
    out["value_proven"] = True
    packed = result["winning_root_action"]
    if packed is None:
        out["stop_reason"] = "mate_proven_without_root_action"
        return out
    action = next(a for a in game.legal_actions if int(a.pack()) == int(packed))
    out["proven"] = True
    out["action_code"] = int(packed)
    out["action_id"] = int(cs.ActionEncoderV4.encode(action, game))
    return out


def _native(game, config, previous_value=None, time_limit_seconds=None) -> Dict[str, Any]:
    r = cs.root_mate_probe(game, config, previous_value=previous_value,
                           time_limit_seconds=time_limit_seconds)
    return {name: getattr(r, name) for name in (
        "attempted", "proven", "value_proven", "depth", "action_code",
        "action_id", "nodes", "stop_reason", "value_triggered")}


def _node_only_config(**overrides) -> cs.RootMateProbeConfig:
    config = cs.RootMateProbeConfig()
    config.time_limit_ms = 0.0
    config.endgame_time_limit_ms = 0.0
    config.warm_start_time_ms = 0.0
    for name, value in overrides.items():
        setattr(config, name, value)
    return config


def _endgame(seed: int, target: int) -> Optional[cs.Game]:
    """A random-purchase game stopped when the mover reaches ``target`` points."""
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    player = RandomPurchasePlayer(rng)
    while not game.is_game_over() and game.turn < 300:
        board = game.board
        mover = board.players[int(game.current_player)]
        if (not board.waiting_return and not board.waiting_noble
                and target <= int(mover.points) <= 14):
            return game
        game.apply(player.select_action(game))
    return None


POSITIONS = [
    (seed, target)
    for seed in range(1, 41)
    for target in (8, 10, 12, 14)
]


def test_default_config_matches_the_documented_values():
    config = cs.RootMateProbeConfig()
    assert config.enabled is True
    assert (config.min_points, config.endgame_points) == (9, 10)
    assert config.trigger_value == pytest.approx(0.6)
    assert (config.max_nodes, config.time_limit_ms) == (20_000, 20.0)
    assert (config.endgame_max_nodes, config.endgame_time_limit_ms) == (2_000_000, 150.0)
    assert (config.min_depth, config.max_depth, config.endgame_exact_depth) == (1, 3, 2)
    assert config.max_cache_states == 50_000
    assert (config.warm_start_nodes, config.warm_start_time_ms) == (50_000, 50.0)
    config.trigger_value = None
    assert config.trigger_value is None


@pytest.mark.parametrize("max_nodes", [20_000, 300])
def test_matches_python_reference_on_endgames(max_nodes):
    config = _node_only_config(max_nodes=max_nodes, endgame_max_nodes=max_nodes * 10)
    compared = proven = 0
    for seed, target in POSITIONS:
        game = _endgame(seed, target)
        if game is None:
            continue
        for previous_value in (None, 0.7):
            assert _native(game, config, previous_value) == _reference_probe(
                game, config, previous_value), (seed, target, previous_value)
            compared += 1
        proven += _native(game, config)["proven"]
    assert compared >= 200
    assert proven >= 10  # the sample exercises the mate path, not only refusals


def test_trigger_value_none_uses_points_only():
    game = _endgame(3, 8)
    assert game is not None and int(game.board.players[game.current_player].points) < 9
    config = _node_only_config(trigger_value=None)
    assert _native(game, config, 0.99)["attempted"] is False
    assert _native(game, _node_only_config(), 0.99)["value_triggered"] is True


def test_disabled_and_hidden_reserve_are_not_searched():
    game = _endgame(5, 12)
    assert game is not None
    assert _native(game, _node_only_config(enabled=False))["attempted"] is False
    opponent_index = 1 - int(game.current_player)
    opponent = game.board.get_player(opponent_index)
    hidden_card = next(int(deck[-1]) for deck in game.board.decks if len(deck))
    opponent.reserved = [hidden_card, -1, -1]
    opponent.reserved_is_hidden = [True, False, False]
    game.board.set_player(opponent_index, opponent)
    result = _native(game, _node_only_config())
    assert result["attempted"] is False
    assert result["stop_reason"] == "opponent_hidden_reserve"


@pytest.mark.parametrize("limit_ms", [5, 30])
def test_whole_probe_respects_one_shared_deadline(limit_ms):
    config = cs.RootMateProbeConfig()
    worst = 0.0
    for seed in range(1, 16):
        game = _endgame(seed, 10)
        if game is None:
            continue
        started = time.monotonic()
        result = cs.root_mate_probe(game, config, time_limit_seconds=limit_ms / 1000.0)
        wall_ms = (time.monotonic() - started) * 1000.0
        assert result.elapsed_ms <= limit_ms + 2.0
        worst = max(worst, wall_ms)
    assert worst <= limit_ms + 15.0  # binding and clone overhead only


def test_exact_endgame_sweep_matches_reference():
    # The anytime sweep runs out of its 50 nodes; the exact sweep proves it.
    game = _endgame(139, 10)
    config = _node_only_config(max_nodes=50, endgame_max_nodes=2_000)
    native = _native(game, config)
    assert native["stop_reason"] == "exact_endgame_mate" and native["proven"]
    assert native == _reference_probe(game, config)


def _hopeless_endgame() -> cs.Game:
    """No card or noble is left for the attacker: it can never reach 15."""
    game = _endgame(5, 12)
    board = game.board
    board.visible = [[-1] * 4, [-1] * 4, [-1] * 4]
    board.decks = [[], [], []]
    board.nobles = []
    attacker = int(game.current_player)
    player = board.get_player(attacker)
    player.reserved = [-1, -1, -1]
    player.reserved_is_hidden = [False] * 3
    board.set_player(attacker, player)
    opponent = board.get_player(1 - attacker)
    opponent.reserved_is_hidden = [False] * 3
    board.set_player(1 - attacker, opponent)
    return game


@pytest.mark.parametrize(
    "max_nodes, stop_reason",
    [
        (1, "cumulative search limit exceeded"),
        (4, "attacker_score_ceiling_below_win_threshold"),
    ],
)
def test_budget_and_certificate_stops_match_reference(max_nodes, stop_reason):
    game = _hopeless_endgame()
    config = _node_only_config(max_nodes=max_nodes, endgame_max_nodes=max_nodes)
    native = _native(game, config)
    assert native["stop_reason"] == stop_reason
    assert native == _reference_probe(game, config)
