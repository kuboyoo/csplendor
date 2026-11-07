"""The attacker's last move outside the final round must reach 15 points."""

from __future__ import annotations

import csplendor as cs
from scripts.mate_solver import load_game_from_usi_text
from tests.test_mate_search_regressions import _empty_board_game, _set_players
from tests.test_reveal_verified_solver import BENCH_POSITION

NOBLE_7 = 7  # requires W3 U3 G3, 3 points


def _exact(game, depth, attacker=0, max_nodes=3_000_000):
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=attacker, depth=depth, max_nodes=max_nodes,
        time_limit_seconds=30.0, exhaustive_attacker_actions=True,
        exact_reveal_search=True)


def test_last_move_cut_shrinks_the_exact_leaf_layer():
    # Without the cut this refutation takes about 1.33M nodes.
    result = _exact(load_game_from_usi_text(BENCH_POSITION), 3)
    assert result["unknown_reason"] is None
    assert not result["proven"]
    assert result["stats"]["nodes"] < 200_000


def test_last_move_cut_shrinks_the_visible_only_search():
    # Without the cut this mate in 5 takes about 1.49M nodes.
    result = cs.solve_visible_only_winner_cpp(
        load_game_from_usi_text(BENCH_POSITION), max_nodes=3_000_000,
        time_limit_seconds=30.0)
    assert result["winner"] == 0 and result["forced_win_depth"] == 5
    assert result["stats"]["nodes"] < 700_000


def _pending_noble_game(gems):
    """P0 (12 pt) already meets noble 7 and receives it at the end of the turn."""
    game = _empty_board_game()
    board = game.board
    board.bank = [4, 4, 4, 4, 4, 5]
    board.visible = [[-1] * 4, [-1] * 4, [-1] * 4]
    board.nobles = [NOBLE_7]
    _set_players(game, [3, 3, 3, 0, 0], 12)
    player = board.get_player(0)
    player.gems = list(gems)
    board.set_player(0, player)
    return game


def test_last_move_keeps_turn_ending_moves_that_collect_a_noble():
    game = _pending_noble_game([0] * 6)
    assert all(a.type != cs.ActionType.PURCHASE for a in game.legal_actions)
    assert _exact(game, 1)["proven"]
    near = cs.near_mate_probability_cpp(game, attacker=0, depth=1,
                                        max_nodes=100_000,
                                        time_limit_seconds=5.0)
    assert near["complete"] and near["value"] == 1.0


def test_last_move_cut_waits_for_the_return_that_ends_the_turn():
    # Ten gems: reserving a deck card for gold leaves a return to make.
    game = _pending_noble_game([2, 2, 2, 2, 2, 0])
    board = game.board
    board.decks = [[0], [], []]
    reserve = [a for a in game.legal_actions
               if a.type == cs.ActionType.RESERVE_DECK][0]
    game.apply(reserve)
    assert game.board.waiting_return and game.current_player == 0
    assert _exact(game, 1)["proven"]


def test_final_round_last_move_still_wins_without_points():
    # P1 leads 16-15 in the final round: any last move ends the game in its favour.
    game = _pending_noble_game([0] * 6)
    board = game.board
    board.nobles = []
    board.final_round = True
    board.current_player = 1
    _set_players(game, [0] * 5, 16, attacker=1, defender_points=15)
    assert _exact(game, 1, attacker=1)["proven"]


def test_last_move_cut_shrinks_the_near_mate_search():
    # Without the cut this depth-2 query exceeds 3M nodes undecided.
    result = cs.near_mate_probability_cpp(
        load_game_from_usi_text(BENCH_POSITION), attacker=0, depth=2,
        max_nodes=100_000, time_limit_seconds=30.0)
    assert result["complete"] and result["value"] == 0.0
