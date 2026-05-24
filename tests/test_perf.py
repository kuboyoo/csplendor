import random
import time

import pytest

from csplendor import Game


MIN_LEGAL_ACTIONS_PER_SEC = 5_000
MIN_PLAYOUT_MOVES_PER_SEC = 10_000


def _make_midgame(seed=42, plies=12):
    rng = random.Random(seed)
    game = Game(seed=seed)
    for _ in range(plies):
        actions = game.legal_actions
        if not actions or game.is_game_over():
            break
        game.apply(rng.choice(actions))
    return game


def _best_rate(work, iterations, rounds=5):
    for _ in range(2):
        work()

    best_elapsed = None
    for _ in range(rounds):
        start = time.perf_counter()
        for _ in range(iterations):
            work()
        elapsed = time.perf_counter() - start
        if best_elapsed is None or elapsed < best_elapsed:
            best_elapsed = elapsed
    return iterations / best_elapsed


@pytest.mark.performance
def test_legal_actions_generation_benchmark(record_property):
    game = _make_midgame()
    actions = game.legal_actions
    assert len(actions) >= 100

    iterations = 2_000
    actions_per_sec = _best_rate(lambda: game.legal_actions, iterations)
    record_property("legal_actions_per_sec", round(actions_per_sec, 2))

    assert actions_per_sec >= MIN_LEGAL_ACTIONS_PER_SEC


@pytest.mark.performance
def test_random_playout_benchmark(record_property):
    rng = random.Random(123)
    total_moves = 0
    n_games = 50
    start = time.perf_counter()

    for seed in range(n_games):
        game = Game(seed=seed)
        while not game.is_game_over() and game.turn < 200:
            actions = game.legal_actions
            if not actions:
                break
            game.apply(rng.choice(actions))
            total_moves += 1

    elapsed = time.perf_counter() - start
    moves_per_sec = total_moves / elapsed
    record_property("playout_moves_per_sec", round(moves_per_sec, 2))
    record_property("playout_total_moves", total_moves)

    assert total_moves >= n_games
    assert moves_per_sec >= MIN_PLAYOUT_MOVES_PER_SEC
