#!/usr/bin/env python3
"""Native-only proof aggregation with the existing paired A/B framework."""
import argparse
import json
import shlex
import sys
from phase3d1_record_20260905 import ROOT, RAW, BASE_SOURCE, run, save


def checked(name, args):
    result = run(name, args, timeout=600)
    assert result.returncode == 0, result.stdout.decode(errors='replace')[-4000:]


def build():
    for side, root in (('A', BASE_SOURCE), ('B', ROOT)):
        directory = ROOT / ('build/3d1-native-proof-' + side)
        checked('configure_native_proof_' + side, ['cmake', '-S', str(ROOT / 'doc/performance_experiments/native_proof_batch_driver'),
            '-B', str(directory), '-DCMAKE_BUILD_TYPE=Release', '-DENGINE_SOURCE_ROOT=' + str(root)])
        checked('build_native_proof_' + side, ['cmake', '--build', str(directory), '--target', 'native_proof_batch', '-j2'])


def measure(prefix='native', pairs=22, cases=('off', 'on')):
    a, b = (ROOT / ('build/3d1-native-proof-' + side) for side in ('A', 'B'))
    for proof in cases:
        name = prefix + '_proof_batch_' + proof
        target = RAW / (name + '.json')
        common = ['--workload', 'exact_reveal', '--fixture', 'reveal_heavy', '--iterations', '200000',
                  '--depth', '1', '--warmup', '100', '--seed', '42', '--proof-dag', 'true' if proof == 'on' else 'false']
        if proof == 'shallow':
            common = ['--workload', 'exact_reveal', '--fixture', 'five_moves', '--iterations', '500000',
                      '--depth', '3', '--warmup', '100', '--seed', '42']
        checked(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
            'paired', '--baseline-command', shlex.join([str(a / 'native_proof_batch'), *common]),
            '--candidate-command', shlex.join([str(b / 'native_proof_batch'), *common]),
            '--baseline-repo-root', str(BASE_SOURCE), '--candidate-repo-root', str(ROOT),
            '--baseline-cmake-cache', str(a / 'CMakeCache.txt'), '--candidate-cmake-cache', str(b / 'CMakeCache.txt'),
            '--pairs', str(pairs), '--warmups', '2', '--bootstrap-iterations', '10000',
            '--rotate-binary-slots', '--cpu-set', '4', '--timeout', '45', '--output', str(target)])
        data = target.read_bytes()
        save(name + '.json', data)
        target.unlink()  # Archive exists before deleting this run's plain output.
        print(name, json.loads(data)['comparison'][0]['B_over_A'], flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'measure'])
    parser.add_argument('--prefix', default='native')
    parser.add_argument('--pairs', type=int, default=22)
    parser.add_argument('--cases', nargs='+', choices=['off', 'on', 'shallow'], default=['off', 'on'])
    args = parser.parse_args()
    if args.stage == 'build':
        build()
    else:
        measure(args.prefix, args.pairs, args.cases)
