"""Golden contracts for the R6 solver and puzzle-tooling split."""

from __future__ import annotations

import hashlib
import json

import csplendor as cs
from scripts import dfpn_mate_solver
from scripts.dfpn_types import RevealVerifiedStats
from scripts.generate_mate_puzzles import (
    PuzzlePlayer,
    generate_candidate_position,
    generate_candidate_positions,
)
from scripts.puzzle_candidates import (
    generate_candidate_position as component_candidate_position,
)
from scripts.puzzle_candidates import (
    generate_candidate_positions as component_candidate_positions,
)
from scripts.puzzle_engine_adapter import PuzzlePlayer as ComponentPuzzlePlayer


def _digest_without_elapsed(result: dict[str, object]) -> str:
    normalized = dict(result)
    normalized["stats"] = dict(normalized["stats"])
    normalized["stats"].pop("elapsed_ms", None)
    payload = json.dumps(
        normalized,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _one_move_win_game() -> cs.Game:
    game = cs.Game(seed=0)
    game.board.visible = [
        [7, -1, -1, -1],
        [-1, -1, -1, -1],
        [-1, -1, -1, -1],
    ]
    game.board.decks = [[15], [], []]
    game.board.bank = [0, 0, 0, 0, 0, 0]
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(0, player)
    return game


def _reveal_win_game() -> cs.Game:
    game = cs.Game(seed=0)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(1, player)
    return game


def test_visible_solver_result_score_and_action_order_golden():
    result = cs.solve_visible_only_winner_cpp(
        _one_move_win_game(),
        max_nodes=100,
        time_limit_seconds=1.0,
    )

    assert result["winner"] == 0
    assert result["forced_win_depth"] == 1
    assert [entry["action_code"] for entry in result["line"]] == [66, 37, 6]
    assert _digest_without_elapsed(result) == (
        "e59ea8ee05c26d7389eaba07c3ce812db4ef21e9bd98fb93c50b813610e0fed3"
    )


def test_reveal_solver_proof_dag_and_action_order_golden():
    result = cs.solve_reveal_verified_mate_cpp(
        _reveal_win_game(),
        attacker=1,
        depth=1,
        include_proof_dag=True,
        proof_dag_node_limit=1000,
        proof_dag_edge_limit=5000,
    )

    assert result["proven"] is True
    assert [entry["action_code"] for entry in result["line"]] == [692, 37]
    assert result["proof_dag"]["complete"] is True
    assert result["proof_dag"]["validated"] is True
    # 2026-10-02 added six RevealVerifiedSearchStats counters (hint_*,
    # line_order_hits, probe_dropped_actions, memo_flushes; all 0 here). With
    # them removed the payload still hashes to the pre-change golden
    # 9732dd60583389a9220ae450619dcaecadc1defc73f6c9cfcbec128173fe2ba2.
    # 2026-10-05 added "waiting_return" to every proof-DAG node and result
    # (the post-deck-reservation return phase); without it the payload still
    # hashes to 6b2453972ede6937b7a1d7bf0b36013d5303ca004b48f32de3ae9d2e8a29717e.
    # 2026-11-07 labels the terminal children of the noble choice depth 0
    # (VISIT_NOBLE ends the attacker's turn); before, c95c3c49...fa92.
    # 2026-11-07 added the counters blank_cut_attempts / blank_cut_proofs
    # (both 0 here); every other field is unchanged from 71e6e7bc...b033e74.
    # 2026-11-07 (守り側の最終応手カット) added last_reply_cut_attempts /
    # last_reply_cut_proofs (both 0 here); otherwise unchanged from
    # 472f243c...7fe50f.
    # 2026-11-07 (攻め側の2手の到達上界による反証) added reach_cut_refutations
    # (0 here); otherwise unchanged from cc1273eb...0936e54c.
    # 2026-11-07 (C++ 版 df-pn) added the dfpn_* counters (0 for the default
    # DFS); otherwise unchanged from e3a9874a...006d4a3b.
    # 2026-11-08 (並列の詰み探索) added the payload's "jobs" (1 here);
    # otherwise unchanged from 88ecb241...8d497ba52e.
    assert _digest_without_elapsed(result) == (
        "b1f31a5854141ce8ddc416658a97aa5c24898f428644cefa6a37aadf0d61beec"
    )


def test_solver_node_and_time_limit_boundaries_remain_distinct():
    node_limited = cs.solve_visible_only_winner_cpp(
        cs.Game(seed=1),
        max_nodes=1,
        time_limit_seconds=1.0,
    )
    time_limited = cs.solve_visible_only_winner_cpp(
        cs.Game(seed=1),
        max_nodes=0,
        time_limit_seconds=1e-12,
    )

    assert node_limited["unknown_reason"] == "node limit exceeded"
    assert time_limited["unknown_reason"] == "time limit exceeded"
    # 2026-11-07: the last-move cut refutes the depth-1 root without move
    # generation (terminal_nodes 0 -> 1, legal_moves 9 -> 0); before,
    # 5a07ad46...2ebdb.
    assert _digest_without_elapsed(node_limited) == (
        "e1638a9d437e9b74fe0290da10f5f996e92ca760491e9eb40ad51b74102c8807"
    )


def test_reveal_verified_types_are_shared_with_the_driver():
    assert dfpn_mate_solver.RevealVerifiedStats is RevealVerifiedStats


def test_puzzle_legacy_paths_forward_to_split_components():
    assert PuzzlePlayer is ComponentPuzzlePlayer
    assert generate_candidate_position is component_candidate_position
    assert generate_candidate_positions is component_candidate_positions
