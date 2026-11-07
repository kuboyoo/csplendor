"""Blank (unknown-card) semantics and the blank dominance cut of the exact search.

doc/mate_usage.md, 「blank 意味論」「blank 意味論の支配」「blank 支配カット」.
"""

from __future__ import annotations

import random

import pytest

import csplendor as cs
from scripts.mate_solver import load_game_from_usi_text
from tests.test_reveal_verified_solver import BENCH_POSITION
from tests.test_root_mate_probe import _endgame

L1_ZERO = [0, 1, 2, 3, 4, 5]   # level-1 cards worth 0 points
L1_ONE = 7                     # level-1 card worth 1 point
L2_TWO = 42                    # level-2 card worth 2 points


def _blank_deck_fixture(points, reserved):
    """P0 (attacker, bonuses 10) can buy anything; P1 can only reserve.

    Level-1 market: four 0-point cards face up, two more in the deck. Every
    purchase refills from that deck, so the blank search consumes a deck card
    into a blank slot at once.
    """
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [0, 0, 0, 0, 0, 0]
    board.visible = [[0, 1, 2, 3], [-1] * 4, [-1] * 4]
    board.decks = [[4, 5], [], []]
    board.nobles = []
    board.current_player = 0
    board.turn = 0
    for index in range(2):
        player = board.get_player(index)
        player.gems = [0] * 6
        player.bonuses = [10] * 5 if index == 0 else [0] * 5
        player.points = points if index == 0 else 0
        player.acquired_nobles = []
        player.reserved = list(reserved) if index == 0 else [-1, -1, -1]
        player.reserved_is_hidden = [False] * 3
        player.purchased_cards = []
        board.set_player(index, player)
    return game


def _buy_code(game, card_id):
    return next(int(a.pack()) for a in game.legal_actions
                if a.type == cs.ActionType.PURCHASE and a.card_id == card_id)


def _solve(game, depth, *, exact, **kwargs):
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=0, depth=depth, max_nodes=200_000,
        time_limit_seconds=10.0, exhaustive_attacker_actions=True,
        exact_reveal_search=exact, **kwargs)


def test_blank_deck_reserve_candidates_include_the_card_consumed_into_a_blank_slot():
    # P0 (14) buys a 0-point card; P1 answers (every answer loses to the
    # reserved 1-point card), so the defender node enumerates every reply.
    game = _blank_deck_fixture(14, [L1_ONE, 8, 9])
    root = _buy_code(game, 0)

    blank = _solve(game, 2, exact=False, required_root_action=root)
    assert blank["proven"] is True
    # The deck holds {4, 5}; the refill consumed one of them into the blank
    # slot, yet the real deck may still hold either card.
    assert blank["stats"]["deck_reserve_candidates"] == 2

    exact = _solve(game, 2, exact=True, required_root_action=root)
    assert exact["proven"] is True
    # The first concrete reveal leaves one card in the deck (1 candidate); the
    # blank search that then covers the other reveal counts both (2).
    assert exact["stats"]["blank_cut_attempts"] == 1
    assert exact["stats"]["blank_cut_proofs"] == 1
    assert exact["stats"]["deck_reserve_candidates"] == 3


def test_oracle_actions_consume_a_deck_card_like_the_real_refill():
    # P0 (11) needs both reserved 2-point cards: root purchase, then two more
    # moves. After the root purchase one card is left in the deck. P1's reply
    # (oracle purchase/reserve of the blank slot, deck reserve or a visible
    # reserve) always consumes it, so P1's second turn has no deck to reserve
    # from: the only deck reservations are P1's first-turn ones (two
    # candidates, see the test above). Before 2026-11-07 the oracle move left
    # its card in the deck array; a later deck reservation then had no
    # candidate at all and the search stopped undecided instead of proving.
    game = _blank_deck_fixture(11, [L2_TWO, 43, -1])
    root = _buy_code(game, 0)
    assert _solve(game, 2, exact=False, required_root_action=root)["proven"] is False

    blank = _solve(game, 3, exact=False, required_root_action=root)
    assert blank["proven"] is True
    assert blank["stats"]["oracle_reserve_actions"] > 0
    assert blank["stats"]["deck_reserve_candidates"] == 2


def _late_position(seed):
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    while not game.is_game_over():
        if max(int(p.points) for p in game.board.players) >= 10:
            return game
        actions = game.legal_actions
        game.apply(actions[rng.randrange(len(actions))], False)
    return None


@pytest.mark.parametrize("seed", range(1, 9))
def test_blank_proofs_never_exceed_exact_proofs(seed):
    game = _late_position(seed)
    if game is None:
        pytest.skip("game ended before the endgame")
    attacker = int(game.current_player)
    for depth in (1, 2):
        blank = cs.solve_reveal_verified_mate_cpp(
            game, attacker=attacker, depth=depth, max_nodes=100_000,
            time_limit_seconds=5.0, exhaustive_attacker_actions=True)
        if not blank["proven"]:
            continue
        exact = cs.solve_reveal_verified_mate_cpp(
            game, attacker=attacker, depth=depth, max_nodes=2_000_000,
            time_limit_seconds=20.0, exhaustive_attacker_actions=True,
            exact_reveal_search=True)
        assert exact["proven"] is True, (seed, depth, exact["unknown_reason"])


def test_blank_cut_reports_its_statistics():
    result = _solve(_blank_deck_fixture(14, [L1_ONE, 8, 9]), 1, exact=True)
    assert result["stats"]["blank_cut_attempts"] == 0
    assert result["stats"]["blank_cut_proofs"] == 0


def test_blank_cut_shrinks_an_exact_endgame_proof():
    # Random-purchase endgame, mate in 2 for the mover. Without the cut the
    # exact sweep needs 1,006 nodes; with it 98 (node limits only).
    game = _endgame(139, 10)
    config = cs.ExactMateSearchConfig()
    config.attacker = int(game.current_player)
    config.min_depth, config.max_depth, config.max_nodes = 1, 3, 2_000
    result = cs.exact_mate_search(game, config)
    assert result.status == "mate" and result.mate_depth == 2
    assert result.nodes < 500


def test_blank_cut_decides_the_bench_position_at_depth_4():
    # Without the cut this depth is still undecided after 387M nodes (240 s);
    # with it the refutation takes about 2.4M nodes (3.5 s, 2026-11-07).
    result = cs.solve_reveal_verified_mate_cpp(
        load_game_from_usi_text(BENCH_POSITION), attacker=0, depth=4,
        max_nodes=6_000_000, time_limit_seconds=120.0,
        exhaustive_attacker_actions=True, exact_reveal_search=True)
    assert result["unknown_reason"] is None
    assert result["proven"] is False
    assert result["stats"]["blank_cut_proofs"] > 10_000
