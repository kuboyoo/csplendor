#!/usr/bin/env python3
"""Phase3D-1 orchestration; timing/bootstrap/digests use the existing harness."""
import argparse
import gzip
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase3d1'
VARIANT = os.environ.get('CSPLENDOR_3D1_VARIANT', '')
if VARIANT:
    if VARIANT not in {'candidate_v2', 'candidate_v3'}:
        raise ValueError('Unsupported 3D-1 variant: ' + VARIANT)
    RAW = RAW / VARIANT
BASE_SOURCE = Path('/home/kuboyu/workspace/repos/csplendor-search-scratch')
BASE = BASE_SOURCE / 'build/3dp2-release'
RELEASE = ROOT / 'build/3d1-release'


def save(name, payload):
    RAW.mkdir(parents=True, exist_ok=True)
    if not isinstance(payload, bytes):
        payload = (json.dumps(payload, indent=2, sort_keys=True) + '\n').encode()
    target = RAW / (name + '.gz')
    with target.open('xb') as stream:
        stream.write(gzip.compress(payload, compresslevel=9, mtime=0))


def run(name, args, timeout=300):
    done = subprocess.run(args, cwd=ROOT, capture_output=True, timeout=timeout)
    save(name + '.json', {'command': args, 'cwd': str(ROOT), 'exit_code': done.returncode,
                         'stdout': done.stdout.decode(errors='replace'),
                         'stderr': done.stderr.decode(errors='replace')})
    print(name, 'exit', done.returncode, flush=True)
    if done.returncode:
        print(done.stderr.decode(errors='replace')[-2000:], flush=True)
    return done


def paired(name, workload, fixture, budget, extra, pairs):
    common = ['--workload', workload, '--fixture', fixture, '--iterations', str(budget),
              '--warmup', '100', '--seed', '42', *extra]
    target = RAW / (name + '.json')
    RAW.mkdir(parents=True, exist_ok=True)
    assert not target.exists()
    done = run(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
        'paired', '--baseline-command', shlex.join([str(BASE / 'benchmark_engine_hotpaths'), *common]),
        '--candidate-command', shlex.join([str(RELEASE / 'benchmark_engine_hotpaths'), *common]),
        '--pairs', str(pairs), '--warmups', '2', '--bootstrap-iterations', '10000',
        '--rotate-binary-slots', '--cpu-set', '4',
        '--baseline-repo-root', str(BASE_SOURCE), '--candidate-repo-root', str(ROOT),
        '--baseline-cmake-cache', str(BASE / 'CMakeCache.txt'),
        '--candidate-cmake-cache', str(RELEASE / 'CMakeCache.txt'),
        '--timeout', '45', '--output', str(target)], timeout=600)
    if target.exists():
        data = target.read_bytes()
        save(name + '.json', data)
        target.unlink()  # Only this run's uncompressed, durably archived result.
        result = json.loads(data)
        print(name, [row['B_over_A'] for row in result.get('comparison', [])], flush=True)
    assert done.returncode == 0, name


CASES = {
    'deep': ('exact_reveal', 'hidden_reserve', 1000000, ['--depth', '7']),
    'shallow': ('exact_reveal', 'five_moves', 500000, ['--depth', '3']),
    'warm': ('exact_reveal', 'five_moves', 500000, ['--depth', '7', '--persistent-reuse', 'true']),
    'visible': ('visible_solver', 'five_moves', 100000, []),
    'cycle': ('visible_solver', 'forced_pass', 1000000, []),
    'editor': ('exact_reveal', 'editor_fallback', 100000, ['--depth', '3']),
    'proof_on': ('exact_reveal', 'reveal_heavy', 200000, ['--depth', '1', '--proof-dag', 'true']),
    'proof_off': ('exact_reveal', 'reveal_heavy', 200000, ['--depth', '1']),
    'defender': ('exact_reveal', 'hidden_reserve', 100000, ['--depth', '3', '--attacker', '0']),
}


def smoke():
    assert run('unit_smoke', ['ctest', '--test-dir', str(RELEASE), '-R', '^solver_components_unit$',
                             '--output-on-failure']).returncode == 0
    paired('smoke_deep', *CASES['deep'], pairs=4)
    paired('smoke_visible', *CASES['visible'], pairs=4)


def formal():
    for name in CASES:
        paired('formal_' + name, *CASES[name], pairs=22)


def holdout():
    paired('holdout_visible', *CASES['visible'], pairs=22)
    paired('holdout_deep', *CASES['deep'], pairs=22)
    paired('holdout_proof_off', *CASES['proof_off'], pairs=22)
    paired('holdout_proof_on', *CASES['proof_on'], pairs=22)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['smoke', 'formal', 'holdout'])
    args = parser.parse_args()
    {'smoke': smoke, 'formal': formal, 'holdout': holdout}[args.stage]()
