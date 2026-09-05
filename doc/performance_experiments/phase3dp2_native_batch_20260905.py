#!/usr/bin/env python3
"""Native-only proof aggregation with the existing paired A/B framework."""
import argparse
import json
import shlex
import sys
from phase3dp2_record_20260905 import ROOT, RAW, BASE_SOURCE, run, save


def checked(name, args):
    result = run(name, args, timeout=600)
    assert result.returncode == 0, result.stdout.decode(errors='replace')[-4000:]


def build():
    for side, root in (('A', BASE_SOURCE), ('B', ROOT)):
        directory = ROOT / ('build/3dp2-native-proof-' + side)
        checked('configure_native_proof_' + side, ['cmake', '-S', str(ROOT / 'doc/performance_experiments/native_proof_batch_driver'),
            '-B', str(directory), '-DCMAKE_BUILD_TYPE=Release', '-DENGINE_SOURCE_ROOT=' + str(root)])
        checked('build_native_proof_' + side, ['cmake', '--build', str(directory), '--target', 'native_proof_batch', '-j2'])


def measure():
    a, b = (ROOT / ('build/3dp2-native-proof-' + side) for side in ('A', 'B'))
    for proof in ('off', 'on'):
        name = 'native_proof_batch_' + proof
        target = RAW / (name + '.json')
        common = ['--workload', 'exact_reveal', '--fixture', 'reveal_heavy', '--iterations', '200000',
                  '--depth', '1', '--warmup', '100', '--seed', '42', '--proof-dag', 'true' if proof == 'on' else 'false']
        checked(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
            'paired', '--baseline-command', shlex.join([str(a / 'native_proof_batch'), *common]),
            '--candidate-command', shlex.join([str(b / 'native_proof_batch'), *common]),
            '--baseline-repo-root', str(BASE_SOURCE), '--candidate-repo-root', str(ROOT),
            '--baseline-cmake-cache', str(a / 'CMakeCache.txt'), '--candidate-cmake-cache', str(b / 'CMakeCache.txt'),
            '--pairs', '22', '--warmups', '2', '--bootstrap-iterations', '10000',
            '--rotate-binary-slots', '--cpu-set', '4', '--timeout', '45', '--output', str(target)])
        data = target.read_bytes()
        save(name + '.json', data)
        target.unlink()  # Archive exists before deleting this run's plain output.
        print(name, json.loads(data)['comparison'][0]['B_over_A'], flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'measure'])
    args = parser.parse_args()
    {'build': build, 'measure': measure}[args.stage]()
