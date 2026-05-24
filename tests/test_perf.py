import random
import time

import pytest

from csplendor import Game

pytestmark = pytest.mark.performance


def test_random_playout_performance_smoke():
    rng = random.Random(42)
    start = time.perf_counter()
    total_moves = 0

    for seed in range(100):
        game = Game(seed=seed)
        while not game.is_game_over() and total_moves < 3000:
            legal_actions = game.legal_actions
            if not legal_actions:
                break
            assert game.apply(rng.choice(legal_actions)) is True
            total_moves += 1

    elapsed = time.perf_counter() - start
    assert total_moves > 0
    assert total_moves / max(elapsed, 1e-9) > 100
