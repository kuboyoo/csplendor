"""Regressions for the C++ mate solvers (depth rule, session cache, rollback, memo cap)."""

from __future__ import annotations

import inspect
import subprocess
import sys
import textwrap
from pathlib import Path

import numpy as np
import pytest

import csplendor as cs
from csplendor import mate_depth, near_mate
from csplendor._csplendor import (
    NativeMateSearchSession,
    solve_reveal_verified_root_split_cpp,
)
from csplendor.mate_frontier import expand_mate_frontier

EMERALD_L1 = [
    c.id for c in cs.get_all_cards()
    if c.level == 1 and int(c.bonus) == 2 and c.points == 0
][0]


def _empty_board_game():
    game = cs.Game(seed=0)
    board = game.board
    board.decks = [[], [], []]
    board.current_player = 0
    board.turn = 0
    return game


def _set_players(game, bonuses, points, attacker=0, defender_points=0):
    board = game.board
    for idx in range(2):
        p = board.get_player(idx)
        p.gems = [0] * 6
        p.bonuses = list(bonuses) if idx == attacker else [0] * 5
        p.points = points if idx == attacker else defender_points
        p.acquired_nobles = []
        p.reserved = [-1, -1, -1] if idx == attacker else [0, 1, 2]
        p.reserved_is_hidden = [False] * 3
        p.purchased_cards = []
        board.set_player(idx, p)


def _noble_position(points, extra_l3=(), attacker=0, final_round=False,
                    defender_points=0):
    """Buying the emerald qualifies the attacker for nobles 7 and 2 (a choice)."""
    game = _empty_board_game()
    board = game.board
    board.current_player = attacker
    board.final_round = final_round
    board.bank = [0] * 6
    board.visible = [
        [EMERALD_L1, -1, -1, -1],
        [-1] * 4,
        list(extra_l3) + [-1] * (4 - len(extra_l3)),
    ]
    board.nobles = [7, 2]
    _set_players(game, [10, 4, 3, 10, 10], points, attacker, defender_points)
    return game


def _buy_emerald(game):
    buy = [
        a for a in game.legal_actions
        if a.type == cs.ActionType.PURCHASE and a.card_id == EMERALD_L1
    ][0]
    mover = game.current_player
    game.apply(buy)
    assert game.board.waiting_noble and game.current_player == mover
    return game


def _waiting_noble_root():
    """P0 waits to choose a noble (9 + 3 = 12); card 71 then wins: mate in 2."""
    return _buy_emerald(_noble_position(9, extra_l3=(71,)))


def _solve(game, depth, attacker=0, **kwargs):
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=attacker, depth=depth, max_nodes=200_000,
        time_limit_seconds=5.0, exhaustive_attacker_actions=True,
        exact_reveal_search=True, **kwargs)


def test_noble_choice_ends_the_turn_in_root_split():
    game = _waiting_noble_root()
    assert not _solve(game, 1)["proven"]
    assert _solve(game, 2)["proven"]
    for depth in (1, 2):
        split = solve_reveal_verified_root_split_cpp(game, attacker=0, depth=depth)
        assert {int(e["child_depth"]) for e in split["edges"]} == {depth - 1}


def test_noble_choice_ends_the_turn_in_root_split_and_frontier_for_player1():
    game = _buy_emerald(_noble_position(12, attacker=1))
    split = solve_reveal_verified_root_split_cpp(game, attacker=1, depth=1)
    assert {int(e["child_depth"]) for e in split["edges"]} == {0}
    frontier = expand_mate_frontier(game, attacker=1, depth=1,
                                    max_nodes=200_000, time_limit_seconds=5.0)
    assert frontier["complete"] and frontier["edges"]
    assert {int(e["child_depth"]) for e in frontier["edges"]} == {0}


def test_parallel_depth_sweep_reports_mate_depth_from_waiting_noble_root():
    result = mate_depth.search_reveal_verified_mate_depths(
        _waiting_noble_root(), attacker=0, min_depth=1, max_depth=3,
        max_nodes=200_000, time_limit_seconds=10.0, jobs=2)
    assert result["status"] == "mate"
    assert result["mate_depth"] == 2


def test_noble_choice_ends_the_turn_in_frontier():
    frontier = expand_mate_frontier(
        _waiting_noble_root(), attacker=0, depth=2,
        max_nodes=200_000, time_limit_seconds=5.0)
    assert frontier["complete"] and frontier["edges"]
    assert {int(e["child_depth"]) for e in frontier["edges"]} == {1}


@pytest.mark.parametrize("attacker", [0, 1])
def test_noble_choice_ends_the_turn_in_proof_dag(attacker):
    # 12 + 3 (noble) = 15: mate in 1 through the noble choice.
    result = _solve(_noble_position(12, attacker=attacker), 1,
                    attacker=attacker, include_proof_dag=True)
    assert result["proven"]
    dag = result["proof_dag"]
    assert dag["complete"] and dag["validated"]
    for node in dag["nodes"]:
        if node["kind"] == "state" and node["player"] == attacker:
            assert node["depth"] == 1
        else:
            assert node["depth"] == 0


def test_final_round_noble_choice_proof_dag_is_complete_for_player1():
    # P0 already has 15; P1 (13) buys the emerald and a noble takes it to 16.
    game = _noble_position(13, attacker=1, final_round=True, defender_points=15)
    result = _solve(game, 1, attacker=1, include_proof_dag=True)
    assert result["proven"]
    dag = result["proof_dag"]
    assert dag["complete"], dag["omitted_reason"]
    assert dag["validated"]


def _mate_in_two_by_take_then_buy():
    """P0 (11 pt, W6) takes the last white, then buys card 71 (4 pt, W7)."""
    game = _empty_board_game()
    board = game.board
    board.bank = [1, 0, 0, 0, 0, 0]
    board.visible = [[-1] * 4, [-1] * 4, [71, -1, -1, -1]]
    board.nobles = []
    _set_players(game, [6, 0, 0, 0, 0], 11)
    return game


def _reserve_only_hints(game):
    reserve = [int(a.pack()) for a in game.legal_actions
               if a.type == cs.ActionType.RESERVE_VISIBLE]
    assert reserve
    return cs.MateOrderHints(
        np.asarray([game.board.set_deck_search_hash()], dtype=np.uint64),
        np.asarray([0, len(reserve)], dtype=np.int64),
        np.asarray(reserve, dtype=np.uint64),
        attacker_restrict_top_k=1,
    )


@pytest.mark.parametrize("restriction", ["no_take_probe", "top_k_hints"])
def test_restricted_session_search_does_not_poison_exhaustive_search(restriction):
    game = _mate_in_two_by_take_then_buy()
    options = ({"no_take_probe": True} if restriction == "no_take_probe"
               else {"order_hints": _reserve_only_hints(game)})
    session = NativeMateSearchSession(0)
    probe = session.search(game, depth=2, max_nodes=200_000,
                           time_limit_seconds=5.0, **options)
    assert not probe["proven"]
    full = session.search(game, depth=2, max_nodes=200_000,
                          time_limit_seconds=5.0)
    assert full["proven"]


def test_restricted_session_search_keeps_proofs():
    game = _mate_in_two_by_take_then_buy()
    session = NativeMateSearchSession(0)
    assert session.search(game, depth=2, max_nodes=200_000,
                          time_limit_seconds=5.0)["proven"]
    session.search(game, depth=2, max_nodes=200_000, time_limit_seconds=5.0,
                   no_take_probe=True)
    again = session.search(game, depth=2, max_nodes=200_000,
                           time_limit_seconds=5.0)
    assert again["proven"] and again["stats"]["persistent_memo_hits"] > 0


def test_session_carry_over_stays_below_the_flush_cap():
    # The carried-over table is trimmed below the flush cap, so the next
    # search does not flush away everything the session kept.
    game = _waiting_noble_root()
    reference = NativeMateSearchSession(0)
    reference.search(game, depth=2, max_nodes=200_000, time_limit_seconds=5.0)
    cap = reference.memoized_states
    assert cap >= 2
    session = NativeMateSearchSession(0)
    options = dict(depth=2, max_nodes=200_000, time_limit_seconds=5.0,
                   max_cache_states=cap, max_memo_states=cap)
    session.search(game, **options)
    assert session.memoized_states == cap
    # The repeat is a one-node hit of the root, so it stores nothing; the
    # table it leaves is the trimmed carry-over (half the flush cap).
    again = session.search(game, **options)
    assert again["proven"] and again["stats"]["persistent_memo_hits"] == 1
    assert session.memoized_states == cap // 2


def test_visible_only_survives_pass_below_long_repetition_window():
    # The search from this random late position grows a repetition window
    # past the inline capacity and then reaches PASS, whose Board move shrank
    # the window's capacity; the rollback restore then used to abort().
    script = textwrap.dedent(
        """
        import random
        import csplendor as cs
        rng = random.Random(3)
        game = cs.Game(seed=3)
        while max(int(p.points) for p in game.board.players) < 9:
            actions = game.legal_actions
            game.apply(actions[rng.randrange(len(actions))], False)
        cs.solve_visible_only_winner_cpp(game, max_nodes=150000,
                                         time_limit_seconds=5.0)
        """
    )
    completed = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True,
        timeout=120, cwd=Path(__file__).resolve().parents[1])
    assert completed.returncode == 0, completed.stderr


def test_memo_cap_is_bounded_by_default():
    cap = cs.DEFAULT_MAX_MEMO_STATES
    assert cap == 2_000_000
    assert cs.ExactMateSearchConfig().max_memo_states == cap
    assert cs.NearMateSearchConfig().max_memo_states == cap
    functions = [
        mate_depth.search_reveal_verified_mate_depths,
        mate_depth.search_reveal_verified_mate_anytime,
        near_mate.search_near_mate,
    ]
    for function in functions:
        default = inspect.signature(function).parameters["max_memo_states"].default
        assert default == cap, function.__name__
