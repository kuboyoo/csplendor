import random

import csplendor as cs
from scripts.puzzle_engine_adapter import RandomPurchasePlayer


def _play(seed: int, plies: int) -> list[int]:
    game = cs.Game(seed=7)
    game.simple_payment_mode = True
    player = RandomPurchasePlayer(random.Random(seed))
    moves = []
    for _ in range(plies):
        if game.is_game_over():
            break
        legal = [int(action.pack()) for action in game.legal_actions]
        action = player.select_action(game)
        packed = int(action.pack())
        assert packed in legal
        if any(
            int(candidate.type) == int(cs.ActionType.PURCHASE)
            for candidate in game.legal_actions
        ):
            assert int(action.type) == int(cs.ActionType.PURCHASE)
        assert game.apply(action, False)
        moves.append(packed)
    return moves


def test_random_purchase_player_plays_legal_actions_preferring_purchases():
    moves = _play(seed=1, plies=200)
    assert len(moves) > 20


def test_random_purchase_player_is_deterministic_for_a_fixed_seed():
    assert _play(seed=3, plies=60) == _play(seed=3, plies=60)
    assert _play(seed=3, plies=60) != _play(seed=4, plies=60)
