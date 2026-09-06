#!/usr/bin/env python3
"""Real StateFeaturizer consumer; emit the existing paired-runner row schema."""
import argparse
import hashlib
import json
import sys
import time
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo', type=Path, required=True)
parser.add_argument('--iterations', type=int, default=50000)
parser.add_argument('--pipeline', action='store_true')
parser.add_argument('--legacy', action='store_true', help='within-build list reference')
args = parser.parse_args()
sys.path.insert(0, str(args.repo.resolve()))
import numpy as np
import csplendor as cs
import csplendor._csplendor as native

assert Path(native.__file__).resolve().parent == args.repo.resolve() / 'csplendor'
consumer = cs.StateFeaturizer()
featurize = (lambda game, observer: np.asarray(cs.StateEncoder.encode(game, observer), dtype=np.float32)) if args.legacy else consumer.featurize

game = cs.Game(seed=42)
games = []
for ply in range(32):
    games.append(game.clone_light())
    assert game.apply_random_action(7919 + ply * 97, False)

def perform(count):
    game = cs.Game(seed=42) if args.pipeline else None
    checksum = 0.0
    resets = 0
    for index in range(count):
        if args.pipeline:
            if game.is_game_over():
                resets += 1
                game = cs.Game(seed=42 + resets)
            row = featurize(game, index % 3 - 1)
            assert game.apply_random_action(7919 + index * 97, False)
        else:
            row = featurize(games[index % len(games)], index % 3 - 1)
        checksum += float(row[index % row.size])
    return checksum, game, resets

# Full-byte semantic oracle is outside the timed portion.
digest = hashlib.sha256()
for game in games:
    for observer in (-1, 0, 1):
        expected = np.asarray(cs.StateEncoder.encode(game, observer), dtype=np.float32)
        actual = featurize(game, observer)
        assert actual.dtype == np.float32 and actual.shape == (196,)
        assert actual.flags.c_contiguous and actual.tobytes() == expected.tobytes()
        digest.update(actual.tobytes())
perform(200)
start = time.perf_counter_ns()
checksum, game, resets = perform(args.iterations)
elapsed = time.perf_counter_ns() - start
digest.update(np.float64(checksum).tobytes())
if game is not None:
    digest.update(game.serialize_snapshot())
print(json.dumps({
    'schema': 'csplendor.engine_hotpath.v1',
    'workload': 'python_feature_pipeline' if args.pipeline else 'python_featurize',
    'fixture': 'reachable_32_seed42', 'seed': 42,
    'operations': args.iterations, 'elapsed_ns': elapsed,
    'rate_per_second': args.iterations * 1e9 / elapsed,
    'rss_kib': None, 'digest': digest.hexdigest(), 'counters': {'game_resets': resets},
    'semantics': {'correct': True, 'schema_fingerprint': cs.StateEncoder.schema_fingerprint(),
                  'observers': [-1, 0, 1], 'feature_dtype': 'float32',
                  'float_order': 'unchanged', 'nn_evaluator': False},
}))
