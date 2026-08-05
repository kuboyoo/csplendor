"""Golden contracts for the R6 solver and puzzle-tooling split."""

from __future__ import annotations

import hashlib
import json

import csplendor as cs
from scripts import dfpn_mate_solver
from scripts.dfpn_proof import extract_tree, selected_children
from scripts.dfpn_table import DFPNTranspositionTable
from scripts.dfpn_types import DFPNStats, RevealVerifiedStats, _DFPNNode
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
    assert _digest_without_elapsed(result) == (
        "9732dd60583389a9220ae450619dcaecadc1defc73f6c9cfcbec128173fe2ba2"
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
    assert _digest_without_elapsed(node_limited) == (
        "5a07ad46e3fda32a97a7471378fd82c5758471e3769de259815b9d5a4102ebdb"
    )


def test_dfpn_types_table_and_proof_components_keep_legacy_surface():
    assert dfpn_mate_solver.DFPNStats is DFPNStats
    assert dfpn_mate_solver.RevealVerifiedStats is RevealVerifiedStats

    leaf = _DFPNNode("leaf", "OR", 0, proof=0, disproof=10)
    other = _DFPNNode("other", "OR", 0, proof=2, disproof=0)
    root = _DFPNNode("root", "OR", 1, children=[leaf, other])
    assert selected_children(root, want_proof=True) == [leaf]
    assert extract_tree(
        root,
        want_proof=True,
        summarize=lambda node: {"kind": node.kind},
    ) == {"kind": "root", "children": [{"kind": "leaf"}]}

    table: DFPNTranspositionTable[_DFPNNode] = DFPNTranspositionTable()
    table[("state", 1)] = root
    assert len(table) == 1
    assert table[("state", 1)] is root
    table.clear()
    assert len(table) == 0


def test_puzzle_legacy_paths_forward_to_split_components():
    assert PuzzlePlayer is ComponentPuzzlePlayer
    assert generate_candidate_position is component_candidate_position
    assert generate_candidate_positions is component_candidate_positions
