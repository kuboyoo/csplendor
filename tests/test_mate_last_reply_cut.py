"""The defender's last-reply cut of the reveal-verified mate search.

doc/mate_usage.md, 「守り側の最終応手カット」: at a defender node with one
attacker move left, the node is proven without generating the replies when
the attacker keeps a purchase that ends the turn at 15+ points and beats the
defender's reach whatever the defender does.
"""

from __future__ import annotations

import random

import pytest

import csplendor as cs

L1_ZERO = [0, 1, 2, 3]  # level-1 cards worth 0 points (deck: 4, 5)
L2_TWO_A, L2_TWO_B = 42, 43  # level-2 cards worth 2 points
L2_THREE_BLUE = 45  # level-2, 3 points, cost blue 6
L2_THREE_RED = 51  # level-2, 3 points, cost red 6
L2_THREE_WHITE = 63  # level-2, 3 points, bonus white, cost white 6
L2_ONE = [46, 47, 52]  # level-2 cards worth 1 point
L3_THREE_A, L3_THREE_B = 70, 82  # level-3 cards worth 3 points
L3_FIVE = 89  # level-3, 5 points, cost blue 7 green 3
NOBLE_W4U4, NOBLE_W4K4 = 4, 3
RICH = [10] * 5  # bonuses that pay for every card


def _position(*, mover, p0, p1, visible=None, decks=None, nobles=()):
    """Players given as dicts (points, bonuses, gems, reserved)."""
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [4, 4, 4, 4, 4, 5]
    board.visible = visible or [L1_ZERO, [-1] * 4, [-1] * 4]
    board.decks = decks or [[4, 5], [], []]
    board.nobles = list(nobles)
    board.current_player = mover
    board.turn = 0
    for index, spec in enumerate((p0, p1)):
        player = board.get_player(index)
        player.points = spec.get("points", 0)
        player.bonuses = list(spec.get("bonuses", [0] * 5))
        player.gems = list(spec.get("gems", [0] * 6))
        reserved = list(spec.get("reserved", []))
        player.reserved = reserved + [-1] * (3 - len(reserved))
        player.reserved_is_hidden = [False] * 3
        player.acquired_nobles = []
        player.purchased_cards = []
        board.set_player(index, player)
    return game


def _solve(game, attacker, depth, *, exact=True, **kwargs):
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=attacker, depth=depth, max_nodes=300_000,
        time_limit_seconds=10.0, exhaustive_attacker_actions=True,
        exact_reveal_search=exact, **kwargs)


def _cut(result):
    return (result["stats"]["last_reply_cut_attempts"],
            result["stats"]["last_reply_cut_proofs"])


def _near_mate_value(game, attacker, depth):
    near = cs.near_mate_probability_cpp(
        game, attacker=attacker, depth=depth, max_nodes=300_000,
        time_limit_seconds=10.0)
    assert near["complete"]
    return near["value"]


ATTACKER_12_WITH_RESERVED_3 = dict(points=12, bonuses=RICH, reserved=[L2_THREE_BLUE])


@pytest.mark.parametrize("exact", [True, False])
def test_reserved_card_proves_the_defender_node_without_replies(exact):
    # P1 to move, P0 (attacker) holds an affordable 3-point reservation: every
    # reply leaves 12 + 3 = 15 > D1 = 0 and D2 = 0.
    game = _position(mover=1, p0=ATTACKER_12_WITH_RESERVED_3, p1={})
    result = _solve(game, 0, 1, exact=exact)
    assert result["proven"] is True
    assert _cut(result) == (1, 1)
    assert result["stats"]["nodes"] <= 3  # the root and the line's attacker node
    assert _near_mate_value(game, 0, 1) == 1.0
    # The line follows a representative reply and the attacker's purchase.
    codes = [cs.Action.unpack(e["action_code"]) for e in result["line"]]
    assert len(codes) == 2
    assert codes[1].type == cs.ActionType.PURCHASE and codes[1].card_id == L2_THREE_BLUE


def test_defender_ending_the_game_first_blocks_the_cut():
    # P1 (13, rich) buys a 2-point card and completes the round at 15.
    game = _position(mover=1, p0=ATTACKER_12_WITH_RESERVED_3,
                     p1=dict(points=13, bonuses=RICH),
                     visible=[L1_ZERO, [L2_TWO_A, -1, -1, -1], [-1] * 4])
    result = _solve(game, 0, 1)
    assert result["proven"] is False and result["unknown_reason"] is None
    assert _cut(result) == (1, 0)


@pytest.mark.parametrize("defender_points, proven, cut", [
    (10, True, (1, 1)),   # D2 = 10 + 2 + 2 = 14 < 15
    (11, True, (1, 0)),   # D2 = 15: no cut; the full search wins the tie on cards
    (12, False, (1, 0)),  # the defender reaches 16 in the final round
])
def test_two_move_reach_of_the_first_player_defender(defender_points, proven, cut):
    game = _position(mover=1, p0=ATTACKER_12_WITH_RESERVED_3,
                     p1=dict(points=defender_points, bonuses=RICH),
                     visible=[L1_ZERO, [L2_TWO_A, L2_TWO_B, -1, -1], [-1] * 4])
    result = _solve(game, 0, 1)
    assert result["unknown_reason"] is None
    assert result["proven"] is proven
    assert _cut(result) == cut
    assert (_near_mate_value(game, 0, 1) == 1.0) is proven


def test_second_player_attacker_needs_a_strict_lead():
    # P0 (defender) to move, P1 attacks: the game ends after P1's turn.
    game = _position(mover=0, p0={}, p1=ATTACKER_12_WITH_RESERVED_3)
    result = _solve(game, 1, 1)
    assert result["proven"] is True and _cut(result) == (1, 1)

    # D1 = 13 + 2 = 15 = S*: no cut, and the full search finds the draw.
    game = _position(mover=0, p0=dict(points=13, bonuses=RICH),
                     p1=ATTACKER_12_WITH_RESERVED_3,
                     visible=[L1_ZERO, [L2_TWO_A, -1, -1, -1], [-1] * 4])
    result = _solve(game, 1, 1)
    assert result["proven"] is False and result["unknown_reason"] is None
    assert _cut(result) == (1, 0)


def test_a_visible_card_counts_only_when_the_defender_cannot_remove_it():
    attacker = dict(points=12, bonuses=RICH)
    visible = [L1_ZERO, [L2_THREE_BLUE, -1, -1, -1], [-1] * 4]
    # The defender can reserve the card: no cut, and reserving refutes.
    game = _position(mover=1, p0=attacker, p1={}, visible=visible)
    result = _solve(game, 0, 1)
    assert result["proven"] is False and _cut(result) == (1, 0)
    # Three reservations and no gems: the card cannot be taken away.
    game = _position(mover=1, p0=attacker, p1=dict(reserved=L2_ONE), visible=visible)
    result = _solve(game, 0, 1)
    assert result["proven"] is True and _cut(result) == (1, 1)


def test_two_winning_visible_cards_survive_one_removal():
    game = _position(mover=1, p0=dict(points=12, bonuses=RICH), p1={},
                     visible=[L1_ZERO, [L2_THREE_BLUE, L2_THREE_RED, -1, -1], [-1] * 4])
    result = _solve(game, 0, 1)
    assert result["proven"] is True and _cut(result) == (1, 1)
    assert _near_mate_value(game, 0, 1) == 1.0


def test_a_noble_the_defender_can_take_is_not_guaranteed():
    # P0 (9) buys the white card (12) and qualifies for noble W4U4; P1 already
    # qualifies for it and takes it with any reply.
    attacker = dict(points=9, bonuses=[3, 4, 0, 0, 4], gems=[3, 0, 0, 0, 0, 0],
                    reserved=[L2_THREE_WHITE])
    defender = dict(bonuses=[4, 4, 0, 0, 0])
    game = _position(mover=1, p0=attacker, p1=defender, nobles=[NOBLE_W4U4])
    result = _solve(game, 0, 1)
    assert result["proven"] is False and result["unknown_reason"] is None
    assert _cut(result) == (1, 0)
    # A second noble (W4K4) only the attacker qualifies for is guaranteed.
    game = _position(mover=1, p0=attacker, p1=defender, nobles=[NOBLE_W4U4, NOBLE_W4K4])
    result = _solve(game, 0, 1)
    assert result["proven"] is True and _cut(result) == (1, 1)
    assert _near_mate_value(game, 0, 1) == 1.0


def _depth_two_position(deck_card):
    # P0 (9) to move: buy the visible 3-point card (12, refill from the
    # level-3 deck), then the reserved 3-point card (15). P1 (10, rich) buys
    # whatever appears.
    return _position(mover=0, p0=dict(points=9, bonuses=RICH, reserved=[L2_THREE_BLUE]),
                     p1=dict(points=10, bonuses=RICH),
                     visible=[L1_ZERO, [-1] * 4, [L3_THREE_A, -1, -1, -1]],
                     decks=[[], [], [deck_card]])


@pytest.mark.parametrize("exact", [True, False])
def test_refilled_card_enters_the_defender_reach_in_both_modes(exact):
    # The level-3 deck holds a 5-point card. After P0 buys card 70 the refill
    # (a concrete reveal, or a blank slot with the oracle purchase) lets P1
    # reach 15 and end the game: no cut, refuted.
    result = _solve(_depth_two_position(L3_FIVE), 0, 2, exact=exact)
    assert result["proven"] is False and result["unknown_reason"] is None
    attempts, proofs = _cut(result)
    assert attempts >= 1 and proofs == 0

    # A 3-point deck card keeps P1 at 13 after two moves: the cut proves the
    # reply node reached through every reveal.
    result = _solve(_depth_two_position(L3_THREE_B), 0, 2, exact=exact,
                    include_proof_dag=True, proof_dag_node_limit=10_000,
                    proof_dag_edge_limit=50_000)
    assert result["proven"] is True
    assert _cut(result)[1] >= 1
    assert result["proof_dag"]["complete"] is True
    assert result["proof_dag"]["validated"] is True
    codes = [cs.Action.unpack(e["action_code"]) for e in result["line"]]
    assert len(codes) == 3
    assert codes[0].card_id == L3_THREE_A and codes[2].card_id == L2_THREE_BLUE


def test_exact_mate_search_uses_the_cut():
    config = cs.ExactMateSearchConfig()
    config.attacker, config.min_depth, config.max_depth = 0, 1, 3
    result = cs.exact_mate_search(_depth_two_position(L3_THREE_B), config)
    assert result.status == "mate" and result.mate_depth == 2
    assert len(result.line) == 3


def _late_position(seed):
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    while not game.is_game_over():
        if max(int(p.points) for p in game.board.players) >= 9 + seed % 4:
            return game
        actions = game.legal_actions
        game.apply(actions[rng.randrange(len(actions))], False)
    return None


@pytest.mark.parametrize("seed", range(1, 7))
def test_verdicts_agree_with_the_near_mate_search_on_random_endgames(seed):
    # near_mate_probability_cpp enumerates every reply (no cut): a decided
    # verdict must agree with its value being 1.0.
    game = _late_position(seed)
    if game is None:
        pytest.skip("game ended before the endgame")
    for attacker in (0, 1):
        for depth in (1, 2):
            exact = _solve(game, attacker, depth)
            if exact["unknown_reason"]:
                continue
            near = cs.near_mate_probability_cpp(
                game, attacker=attacker, depth=depth, max_nodes=300_000,
                time_limit_seconds=10.0)
            if not near["complete"]:
                continue
            assert exact["proven"] is (near["value"] == 1.0), (seed, attacker, depth)
            if exact["proven"]:
                break
