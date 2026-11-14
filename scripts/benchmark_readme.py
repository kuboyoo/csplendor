"""Python-side measurements behind the README benchmark table.

Usage: python scripts/benchmark_readme.py [THREADS ...]   (default: 1 4 10)

Neural-network inference is excluded: V3SearchSession is fed uniform priors
and a value computed from the leaf features.
"""
import random
import statistics
import sys
import time

import numpy as np

import csplendor as cs


def median_seconds(fn, repeats=5):
    samples = []
    for _ in range(repeats):
        start = time.perf_counter()
        fn()
        samples.append(time.perf_counter() - start)
    return statistics.median(samples)


def random_position(seed, plies):
    rng = random.Random(seed)
    game = cs.Game(seed=42)
    game.simple_payment_mode = False
    for _ in range(plies):
        game.apply(rng.choice(game.legal_actions))
    return game


def pseudo_network(features, legal_ids, offsets):
    priors = np.empty(len(legal_ids), dtype=np.float32)
    for i in range(len(offsets) - 1):
        lo, hi = offsets[i], offsets[i + 1]
        priors[lo:hi] = 1.0 / max(hi - lo, 1)
    values = np.tanh(features.sum(axis=1) * 0.01).astype(np.float32)
    return priors, values


def v3_search_rate(threads, num_games=64, simulations=400):
    config = cs.V3SearchConfig()
    config.num_simulations = simulations
    config.leaf_batch_size = 32
    config.num_threads = threads
    session = cs.V3SearchSession(config)
    for index in range(num_games):
        game = random_position(index, 10)
        session.add_game(game, observer=game.current_player, seed=index,
                         root_noise=False)
    start = time.perf_counter()
    while not session.all_done():
        features, legal_ids, offsets, slots = session.collect()
        if len(slots) == 0:
            break
        priors, values = pseudo_network(features, legal_ids, offsets)
        session.apply(legal_ids, offsets, priors, values)
    return num_games * simulations / (time.perf_counter() - start)


def random_selfplay_rate(games=200):
    start = time.perf_counter()
    moves = 0
    for seed in range(games):
        game = cs.Game(seed=seed)
        rng = random.Random(seed)
        while not game.is_game_over():
            game.apply_random_action(rng.getrandbits(32))
            moves += 1
    return moves / (time.perf_counter() - start)


def main():
    threads = [int(value) for value in sys.argv[1:]] or [1, 4, 10]
    game = random_position(seed=1, plies=17)
    observer = game.current_player
    calls = 20000
    print("legal actions in the position:", game.legal_action_count)

    seconds = median_seconds(lambda: [game.legal_actions for _ in range(calls)])
    print(f"Game.legal_actions: {calls / seconds:,.0f} calls/s")
    seconds = median_seconds(
        lambda: [cs.ActionEncoderV5.legal_action_ids(game) for _ in range(calls)])
    print(f"ActionEncoderV5.legal_action_ids: {seconds / calls * 1e6:.2f} us")
    seconds = median_seconds(
        lambda: [cs.ActionEncoderV5.get_action_mask(game) for _ in range(calls)])
    print(f"ActionEncoderV5.get_action_mask: {seconds / calls * 1e6:.2f} us")
    featurizer = cs.StateFeaturizer()
    seconds = median_seconds(
        lambda: [featurizer.featurize(game, observer=observer)
                 for _ in range(calls)])
    print(f"StateFeaturizer.featurize: {seconds / calls * 1e6:.2f} us")
    snapshot = game.serialize_snapshot()
    seconds = median_seconds(
        lambda: [cs.Game.deserialize_snapshot(snapshot) for _ in range(calls)])
    print(f"Game.deserialize_snapshot ({len(snapshot)} bytes): "
          f"{seconds / calls * 1e9:,.0f} ns")
    print(f"random self-play (apply_random_action): "
          f"{random_selfplay_rate():,.0f} moves/s")
    for count in threads:
        rate = statistics.median(v3_search_rate(count) for _ in range(3))
        print(f"V3SearchSession 64 games x 400 simulations, "
              f"num_threads={count}: {rate:,.0f} sim/s")


if __name__ == "__main__":
    main()
