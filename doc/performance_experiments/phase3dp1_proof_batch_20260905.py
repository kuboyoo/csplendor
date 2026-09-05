#!/usr/bin/env python3
"""Supplementary Python-boundary proof guard using benchmark_solvers._sample.

This does not replace the native primary A/B or claim native-only timing.
"""
import argparse
import hashlib
import json
import resource
import sys
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package-root', required=True)
parser.add_argument('--proof', choices=['on', 'off'], required=True)
args = parser.parse_args()
sys.path.insert(0, args.package_root)
import csplendor as cs

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.benchmark_solvers import _reveal_fixture, _sample

game = _reveal_fixture()
proof = args.proof == 'on'
reference = None


def operation():
    global reference
    result = cs.solve_reveal_verified_mate_cpp(
        game, attacker=1, depth=1, max_nodes=200000, time_limit_seconds=0.0,
        include_proof_dag=proof, exact_reveal_search=True)
    result['stats'].pop('elapsed_ms')
    if reference is None:
        assert result['proven'] and not result['unknown_reason']
        reference = result
    else:
        assert result == reference
    return result['stats']['nodes']


sample = _sample(operation, calls=2000, repetitions=1)['samples'][0]
elapsed_ns = round(sample['seconds'] * 1e9)
print(json.dumps({
    'schema': 'csplendor.engine_hotpath.v1', 'workload': 'python_proof_batch',
    'fixture': 'existing_reveal_fixture_' + args.proof, 'seed': 0,
    'operations': int(sample['nodes']), 'elapsed_ns': elapsed_ns,
    'rate_per_second': sample['nodes'] * 1e9 / elapsed_ns,
    'rss_kib': resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
    'digest': hashlib.sha256(json.dumps(reference, sort_keys=True).encode()).hexdigest(),
    'counters': {'nodes': int(sample['nodes']), 'instrumentation_enabled': False},
    'semantics': {'correct': True, 'proven': True, 'proof_requested': proof,
                  'calls': 2000, 'scope': 'Python boundary + native solve + result equality; existing _sample'},
}))
