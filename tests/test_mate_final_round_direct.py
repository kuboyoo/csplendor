"""Final-round nodes decided directly skip the hash and the table.

doc/mate_usage.md, 「最終ラウンドの直接判定の前置き」: the defender's node after
the attacker's last move is decided from the board alone and is not stored;
the proof DAG and the frontier still expand it.
"""

from __future__ import annotations

import pytest

import csplendor as cs
from tests.test_mate_last_reply_cut import (
    ATTACKER_12_WITH_RESERVED_3,
    L2_THREE_BLUE,
    _position,
    _solve,
)


def _attacker_to_move():
    # P0 (12 points, rich) buys its reserved 3-point card and reaches 15; P1's
    # final-round reply cannot catch up.
    return _position(mover=0, p0=ATTACKER_12_WITH_RESERVED_3, p1={})


def _after_winning_purchase():
    game = _attacker_to_move()
    purchase = [a for a in game.legal_actions
                if a.type == cs.ActionType.PURCHASE and a.card_id == L2_THREE_BLUE][0]
    assert game.apply(purchase)
    assert game.board.final_round and game.current_player == 1
    return game


@pytest.mark.parametrize("exact", [True, False])
def test_direct_final_round_node_is_decided_without_a_table_entry(exact):
    result = _solve(_attacker_to_move(), 0, 1, exact=exact)
    assert result["proven"] is True
    assert result["stats"]["nodes"] == 2
    assert result["stats"]["final_round_direct_resolutions"] == 1
    assert result["memoized_states"] == 1  # the root only
    assert [cs.Action.unpack(e["action_code"]).card_id for e in result["line"]] == [L2_THREE_BLUE]


def test_proof_dag_expands_the_unstored_final_round_node():
    result = _solve(_attacker_to_move(), 0, 1, exact=False, include_proof_dag=True,
                    proof_dag_format="v1")
    dag = result["proof_dag"]
    assert dag["complete"] and dag["validated"]
    kinds = {node["kind"] for node in dag["nodes"]}
    assert "final_round_summary" in kinds
    summary = [node for node in dag["nodes"] if node["kind"] == "final_round_summary"][0]
    assert summary["player"] == 1 and len(summary["children"]) > 0


def test_frontier_expands_the_unstored_final_round_node():
    game = _after_winning_purchase()
    frontier = cs.solve_reveal_verified_frontier_cpp(
        game, attacker=0, depth=0, max_nodes=10_000, time_limit_seconds=5.0)
    assert frontier["proven"] and frontier["complete"]
    assert frontier["kind"] == "final_round_summary"
    assert {int(e["action_code"]) for e in frontier["edges"]} == {
        int(code) for code in game.legal_action_codes}
    assert all(e["child_depth"] == 0 for e in frontier["edges"])
