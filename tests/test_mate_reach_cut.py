"""The attacker's two-move reach cut of the reveal-verified mate search.

doc/mate_usage.md, 「攻め側の2手の到達上界による反証」: an attacker node with two
moves left is refuted without generating moves when even the upper bound of
the attacker's points after two turns stays below 15.
"""

from __future__ import annotations

import pytest

import csplendor as cs
from tests.test_mate_last_reply_cut import (
    L1_ZERO,
    L2_THREE_BLUE,
    L2_TWO_A,
    L2_TWO_B,
    L3_FIVE,
    RICH,
    _near_mate_value,
    _position,
    _solve,
)

TWO_POINT_CARDS = [L1_ZERO, [L2_TWO_A, L2_TWO_B, -1, -1], [-1] * 4]
EMPTY_DECKS = [[], [], []]


def _attacker_to_move(points, *, visible, decks, reserved=()):
    return _position(mover=0, p0=dict(points=points, bonuses=RICH, reserved=list(reserved)),
                     p1={}, visible=visible, decks=decks)


def _cut(result):
    return result["stats"]["reach_cut_refutations"]


@pytest.mark.parametrize("exact", [True, False])
def test_reach_below_fifteen_refutes_without_generating_moves(exact):
    # 8 + 2 + 2 + no noble = 12 < 15 whatever the two moves are.
    game = _attacker_to_move(8, visible=TWO_POINT_CARDS, decks=EMPTY_DECKS)
    result = _solve(game, 0, 2, exact=exact)
    assert result["proven"] is False and result["unknown_reason"] is None
    assert result["stats"]["nodes"] == 1 and result["stats"]["legal_moves"] == 0
    assert _cut(result) == 1
    assert _near_mate_value(game, 0, 2) == 0.0


def test_hidden_cards_of_a_level_with_a_deck_count_towards_the_reach():
    # A 5-point card hidden in the level-3 deck may surface: 8 + 2 + 5 = 15,
    # so the node is searched (and refuted by the search, not by the cut).
    game = _attacker_to_move(8, visible=TWO_POINT_CARDS, decks=[[], [], [L3_FIVE]])
    result = _solve(game, 0, 2)
    assert result["proven"] is False and result["unknown_reason"] is None
    assert result["stats"]["nodes"] > 1
    assert _cut(result) == 0


def test_mate_in_two_is_still_found():
    # 10 + 2 (visible) + 3 (reserved) = 15; the defender can only reserve.
    game = _attacker_to_move(10, visible=TWO_POINT_CARDS, decks=EMPTY_DECKS,
                             reserved=[L2_THREE_BLUE])
    result = _solve(game, 0, 2)
    assert result["proven"] is True
    assert _cut(result) == 0
    assert [cs.Action.unpack(e["action_code"]).type for e in result["line"]] == [
        cs.ActionType.PURCHASE, cs.ActionType.RESERVE_VISIBLE, cs.ActionType.PURCHASE]
