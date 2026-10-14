"""exact_mate_search (C++) matches search_reveal_verified_mate_depths(jobs=1).

Node limits only, so both are deterministic.
"""

from __future__ import annotations

from typing import Any, Dict

import pytest

import csplendor as cs
from tests.test_near_mate_search_parity import _late_position
from tests.test_reveal_verified_solver import (
    _five_move_mate_fixture,
    _six_move_mate_fixture,
)


def _native(game, attacker, min_depth, max_depth, max_nodes, max_memo_states=0) -> Dict[str, Any]:
    config = cs.ExactMateSearchConfig()
    config.attacker = attacker
    config.min_depth = min_depth
    config.max_depth = max_depth
    config.max_nodes = max_nodes
    config.max_memo_states = max_memo_states
    r = cs.exact_mate_search(game, config)
    return {
        "status": r.status,
        "stop_reason": r.stop_reason,
        "mate_depth": None if r.mate_depth < 0 else r.mate_depth,
        "verified_no_mate_through_depth": (
            None if r.verified_no_mate_through_depth < 0 else r.verified_no_mate_through_depth
        ),
        "winning_root_action": r.winning_root_action if r.has_winning_root_action else None,
        "line": [
            (e.action_code, None if e.reveal_card < 0 else e.reveal_card, e.action_count)
            for e in r.line
        ],
        "nodes": r.nodes,
    }


def _reference(game, attacker, min_depth, max_depth, max_nodes, max_memo_states=0) -> Dict[str, Any]:
    r = cs.search_reveal_verified_mate_depths(
        game, attacker=attacker, min_depth=min_depth, max_depth=max_depth,
        max_nodes=max_nodes, max_memo_states=max_memo_states,
    )
    line = r["attempts"][-1]["line"] if r["status"] == "mate" else []
    return {
        "status": r["status"],
        "stop_reason": r["stop_reason"],
        "mate_depth": r["mate_depth"],
        "verified_no_mate_through_depth": r["verified_no_mate_through_depth"],
        "winning_root_action": r["winning_root_action"],
        "line": [(e["action_code"], e["reveal_card"], e["action_count"]) for e in line],
        "nodes": int(r["stats"]["nodes"]),
    }


def _hopeless(game: cs.Game) -> cs.Game:
    board = game.board
    board.visible = [[-1] * 4, [-1] * 4, [-1] * 4]
    board.decks = [[], [], []]
    board.nobles = []
    attacker = int(game.current_player)
    player = board.get_player(attacker)
    player.reserved = [-1, -1, -1]
    player.reserved_is_hidden = [False] * 3
    board.set_player(attacker, player)
    return game


def _cases():
    yield "six", _six_move_mate_fixture(), 0, 4, 6, 0
    yield "five", _five_move_mate_fixture(), 0, 3, 5, 0
    yield "five-cut", _five_move_mate_fixture(), 0, 3, 5, 2_000
    yield "hopeless", _hopeless(_late_position(5)), int(_late_position(5).current_player), 1, 2, 0
    for seed in range(1, 20):
        game = _late_position(seed)
        if game is not None:
            for max_nodes in (5_000, 200_000):
                yield f"late{seed}-{max_nodes}", game, int(game.current_player), 1, 2, max_nodes


@pytest.mark.parametrize("name, game, attacker, min_depth, max_depth, max_nodes",
                         list(_cases()), ids=lambda v: v if isinstance(v, str) else None)
def test_matches_the_python_depth_sweep(name, game, attacker, min_depth, max_depth, max_nodes):
    args = (game, attacker, min_depth, max_depth, max_nodes)
    assert _native(*args) == _reference(*args)


def test_memo_limit_is_forwarded():
    args = (_five_move_mate_fixture(), 0, 5, 5, 0, 50_000)
    assert _native(*args) == _reference(*args)


def test_winning_root_action_has_its_v4_id():
    game = _six_move_mate_fixture()
    config = cs.ExactMateSearchConfig()
    config.attacker, config.min_depth, config.max_depth = 0, 6, 6
    result = cs.exact_mate_search(game, config)
    assert result.status == "mate"
    action = cs.Action.unpack(result.winning_root_action)
    assert result.winning_root_action_id == cs.ActionEncoderV4.encode(action, game)
