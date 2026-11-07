"""df-pn mate search beside the depth-first solver.

doc/mate_dfpn_cpp_design.md: ``algorithm="dfpn"`` searches the same AND/OR
graph (moves, reveals, cuts) as the depth-first search, so every decided
verdict must agree; lines and proof DAGs come from the shared decided tables.
"""

from __future__ import annotations

import random
from pathlib import Path

import pytest

import csplendor as cs
from scripts.mate_solver import load_game_from_usi_text
from tests.test_reveal_verified_solver import (
    BENCH_POSITION,
    _five_move_mate_fixture,
    _newly_revealed_purchase_fixture,
    _seven_move_mate_fixture,
    _six_move_mate_fixture,
)


def _solve(game, attacker, depth, algorithm, *, exact, **kwargs):
    kwargs.setdefault("max_nodes", 2_000_000)
    kwargs.setdefault("time_limit_seconds", 60.0)
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=attacker, depth=depth,
        exhaustive_attacker_actions=True, exact_reveal_search=exact,
        algorithm=algorithm, **kwargs)


def _verdict(result):
    if result["unknown_reason"] is not None:
        return "unknown"
    return "proven" if result["proven"] else "refuted"


def _replay(game, line):
    """Applies a principal line (reveal cards put on top of their deck)."""
    game = game.clone()
    for entry in line:
        action = cs.Action.unpack(int(entry["action_code"]))
        reveal = entry["reveal_card"]
        if reveal is not None:
            level = (int(action.deck_level)
                     if action.type == cs.ActionType.RESERVE_DECK
                     else int(cs.get_card(int(action.card_id)).level) - 1)
            decks = [[int(c) for c in deck] for deck in game.board.decks]
            if reveal in decks[level]:
                decks[level].remove(reveal)
            else:
                decks[level].pop()
            decks[level].append(reveal)
            game.board.decks = decks
        assert game.apply_action_code(int(entry["action_code"]), False)
    return game


FIXTURES = [
    ("five", _five_move_mate_fixture, 0, (4, 5)),
    ("six", _six_move_mate_fixture, 0, (5, 6)),
    ("seven", _seven_move_mate_fixture, 0, (6, 7)),
    ("revealed", _newly_revealed_purchase_fixture, 0, (1, 2)),
]


@pytest.mark.parametrize("exact", [True, False])
@pytest.mark.parametrize(("name", "fixture", "attacker", "depths"), FIXTURES)
def test_dfpn_agrees_with_depth_first_search_on_fixtures(
        name, fixture, attacker, depths, exact):
    game = fixture()
    for depth in depths:
        dfs = _solve(game, attacker, depth, "dfs", exact=exact)
        dfpn = _solve(game, attacker, depth, "dfpn", exact=exact)
        assert _verdict(dfpn) == _verdict(dfs) != "unknown", (name, depth)
    assert _verdict(dfpn) == "proven"


@pytest.mark.parametrize("exact", [True, False])
def test_dfpn_agrees_on_the_bench_position(exact):
    game = load_game_from_usi_text(BENCH_POSITION)
    for depth in (1, 2, 3):
        dfs = _solve(game, 0, depth, "dfs", exact=exact)
        dfpn = _solve(game, 0, depth, "dfpn", exact=exact)
        assert _verdict(dfpn) == _verdict(dfs) == "refuted", depth
    # Depth 1 is refuted by the last-move cut without generating moves.
    assert dfpn["stats"]["dfpn_expansions"] > 0
    assert dfs["stats"]["dfpn_expansions"] == 0


@pytest.mark.parametrize("exact", [True, False])
@pytest.mark.parametrize(("name", "fixture", "attacker", "depths"), FIXTURES)
def test_dfpn_principal_line_replays_and_starts_with_a_winning_move(
        name, fixture, attacker, depths, exact):
    game = fixture()
    depth = depths[-1]
    result = _solve(game, attacker, depth, "dfpn", exact=exact)
    assert result["proven"] is True
    line = result["line"]
    assert line, name
    _replay(game, line)
    root = int(line[0]["action_code"])
    assert line[0]["action_count"] == len(game.legal_actions)
    assert _solve(game, attacker, depth, "dfs", exact=exact,
                  required_root_action=root)["proven"] is True


@pytest.mark.parametrize("exact", [True, False])
def test_dfpn_proof_dag_is_validated(exact):
    game = _newly_revealed_purchase_fixture()
    result = _solve(game, 0, 2, "dfpn", exact=exact, include_proof_dag=True,
                    proof_dag_format="compact")
    assert result["proven"] is True
    dag = result["proof_dag"]
    assert dag["complete"] is True and dag["validated"] is True
    five = _solve(_five_move_mate_fixture(), 0, 5, "dfpn", exact=exact,
                  include_proof_dag=True)
    assert five["proof_dag"]["validated"] is True


def test_dfpn_stops_undecided_at_the_node_limit():
    game = load_game_from_usi_text(BENCH_POSITION)
    result = _solve(game, 0, 4, "dfpn", exact=True, max_nodes=2_000)
    assert result["proven"] is False
    assert result["unknown_reason"] == "node limit exceeded"
    assert result["stats"]["nodes"] == 2_000


def test_dfpn_table_stays_within_its_byte_budget():
    game = load_game_from_usi_text(BENCH_POSITION)
    # 8/9 of 1024 bytes holds four buckets of four 48-byte exact slots (768
    # bytes), so the last growth (old + new table) stays within the budget.
    budget = 1024
    small = _solve(game, 0, 3, "dfpn", exact=True, dfpn_table_bytes=budget)
    large = _solve(game, 0, 3, "dfpn", exact=True)
    assert _verdict(small) == _verdict(large) == "refuted"
    assert small["stats"]["dfpn_table_bytes"] == 768
    assert small["stats"]["dfpn_table_stores"] > 0


def test_public_solver_header_does_not_expose_the_dfpn_search():
    root = Path(__file__).resolve().parents[1]
    header = (root / "src" / "reveal_verified_solver.h").read_text(encoding="utf-8")
    assert '"dfpn_mate_solver.h"' not in header


def test_unknown_algorithm_is_rejected():
    with pytest.raises(ValueError, match="algorithm"):
        _solve(_five_move_mate_fixture(), 0, 1, "bfs", exact=True)


def _late_position(seed):
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    target = rng.choice((7, 8, 9, 10, 11, 12, 13))
    while max(int(p.points) for p in game.board.players) < target:
        actions = game.legal_actions
        if not actions:
            return None
        game.apply(actions[rng.randrange(len(actions))], False)
    return game if game.legal_actions else None


@pytest.mark.parametrize("seed", range(3000, 3012))
def test_dfpn_agrees_on_random_endgames(seed):
    game = _late_position(seed)
    if game is None:
        pytest.skip("game ended before the target score")
    for attacker in (0, 1):
        for exact in (True, False):
            for depth in (1, 2):
                dfs = _solve(game, attacker, depth, "dfs", exact=exact,
                             max_nodes=300_000)
                dfpn = _solve(game, attacker, depth, "dfpn", exact=exact,
                              max_nodes=300_000)
                if "unknown" in (_verdict(dfs), _verdict(dfpn)):
                    continue
                assert _verdict(dfpn) == _verdict(dfs), (attacker, exact, depth)


@pytest.mark.parametrize("jobs", [1, 2])
def test_depth_sweep_selects_dfpn(jobs):
    sweeps = [
        cs.search_reveal_verified_mate_depths(
            _six_move_mate_fixture(), attacker=0, min_depth=4, max_depth=7,
            time_limit_seconds=60.0, jobs=jobs, algorithm=algorithm)
        for algorithm in ("dfs", "dfpn")
    ]
    for key in ("status", "mate_depth", "verified_no_mate_through_depth"):
        assert sweeps[0][key] == sweeps[1][key]
    assert sweeps[1]["mate_depth"] == 6
    with pytest.raises(ValueError, match="algorithm"):
        cs.search_reveal_verified_mate_depths(
            _six_move_mate_fixture(), attacker=0, min_depth=1, max_depth=1,
            algorithm="bfs")
