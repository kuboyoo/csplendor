"""Group-by-group generation of the defender's replies in the mate search.

doc/mate_usage.md, 「守り側の応手の遅延生成」: a defender node generates its
purchases, visible reserves, token takes and deck reserves one group at a
time, in the order the full list visits them, so a refutation found among the
purchases never generates the takes. Verdict, principal line (including each
entry's full ``action_count``) and node counts equal the eager generation's.
"""

from __future__ import annotations

import pytest

import csplendor as cs
from tests.test_mate_last_reply_cut import L2_TWO_A, _position, _solve

BLUE = 1


def _take_then_buy_position():
    """P0 (13 points) reaches 15 only by taking a blue token and then buying
    the 2-point card (the reserve slots hold unaffordable level-3 cards, so
    the card cannot be reserved for gold); P1 buys it at once and refutes."""
    return _position(
        mover=0,
        p0=dict(points=13, gems=[0, 4, 0, 0, 0, 0], reserved=[70, 82, 89]),
        p1=dict(points=0, gems=[0, 5, 0, 0, 0, 0]),
        visible=[[-1] * 4, [L2_TWO_A, -1, -1, -1], [-1] * 4],
        decks=[[], [], []],
    )


def _defender_node(game, line):
    child = game.clone()
    first = line[0]
    assert first["reveal_card"] is None
    child.apply(cs.Action.unpack(first["action_code"]), False)
    assert child.current_player == 1
    return child


@pytest.mark.parametrize("exact", [True, False])
def test_refuted_defender_entry_reports_the_full_reply_count(exact):
    game = _take_then_buy_position()
    assert cs.get_card(L2_TWO_A).cost[BLUE] == 5
    result = _solve(game, 0, 2, exact=exact)
    assert not result["proven"] and result["unknown_reason"] is None
    line = result["line"]
    assert len(line) >= 2
    defender = _defender_node(game, line)
    full_count = len(defender.legal_actions)
    assert cs.Action.unpack(line[1]["action_code"]).card_id == L2_TWO_A
    # The refutation was found among the purchases, so the takes were never
    # generated, yet the line reports the size of the full reply list.
    assert line[1]["action_count"] == full_count
    assert full_count > 1
    root_moves = len(game.legal_actions)
    assert result["stats"]["legal_moves"] < root_moves + full_count


def test_order_hints_fall_back_to_the_eager_generation():
    import numpy as np

    game = _take_then_buy_position()
    plain = _solve(game, 0, 2)
    hints = cs.MateOrderHints(np.asarray([], dtype=np.uint64),
                              np.asarray([0], dtype=np.int64),
                              np.asarray([], dtype=np.uint64))
    hinted = _solve(game, 0, 2, order_hints=hints)
    assert hinted["proven"] == plain["proven"]
    assert hinted["stats"]["nodes"] == plain["stats"]["nodes"]
    assert [(e["action_code"], e["action_count"]) for e in hinted["line"]] == [
        (e["action_code"], e["action_count"]) for e in plain["line"]]
    # The eager path generates every reply of the refuting defender node.
    assert hinted["stats"]["legal_moves"] > plain["stats"]["legal_moves"]
