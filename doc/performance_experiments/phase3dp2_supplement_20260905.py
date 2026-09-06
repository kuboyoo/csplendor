#!/usr/bin/env python3
"""Repeat the existing Python proof batch guard; retain native uncertainty."""
import argparse
import json
import shlex
import sys
from phase3dp2_record_20260905 import ROOT, RAW, BASE_SOURCE, run, save


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    assert result.returncode == 0, result.stdout.decode(errors='replace')[-4000:]
    return result


def build():
    directory = ROOT / 'build/3dp2-proof-driver'
    checked('configure_proof_driver', ['cmake', '-S', str(ROOT / 'doc/performance_experiments/proof_batch_driver'),
        '-B', str(directory), '-DCMAKE_BUILD_TYPE=Release', '-DPYTHON_PATH=' + sys.executable,
        '-DPROBE_PATH=' + str(ROOT / 'doc/performance_experiments/phase3dp1_proof_batch_20260905.py'),
        '-DPACKAGE_A=' + str(BASE_SOURCE), '-DPACKAGE_B=' + str(ROOT)])
    checked('build_proof_driver', ['cmake', '--build', str(directory), '-j2'])
    checked('build_buffer_probe', ['c++', '-O3', '-DNDEBUG', '-std=c++17', '-I' + str(ROOT / 'src'),
        str(ROOT / 'doc/performance_experiments/phase3dp2_buffer_probe_20260905.cpp'),
        '-o', str(ROOT / 'build/3dp2-buffer-probe')])


def measure():
    directory = ROOT / 'build/3dp2-proof-driver'
    for proof in ('off', 'on'):
        name = 'python_proof_batch_' + proof
        target = RAW / (name + '.json')
        checked(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
            'paired', '--baseline-command', shlex.join([str(directory / 'proof_launcher_A'), '--proof', proof]),
            '--candidate-command', shlex.join([str(directory / 'proof_launcher_B'), '--proof', proof]),
            '--baseline-repo-root', str(BASE_SOURCE), '--candidate-repo-root', str(ROOT),
            '--baseline-cmake-cache', str(directory / 'CMakeCache.txt'),
            '--candidate-cmake-cache', str(directory / 'CMakeCache.txt'),
            '--pairs', '22', '--warmups', '2', '--bootstrap-iterations', '10000',
            '--rotate-binary-slots', '--cpu-set', '4', '--timeout', '45', '--output', str(target)])
        data = target.read_bytes()
        save(name + '.json', data)
        target.unlink()  # Only this run's now-durably-archived output.
        print(name, json.loads(data)['comparison'][0]['B_over_A'], flush=True)
    checked('buffer_probe', ['taskset', '-c', '4', str(ROOT / 'build/3dp2-buffer-probe')])
    # Read only. No privilege/sysctl changes if hardware counters are unavailable.
    run('hardware_perf', ['perf', 'stat', '-e', 'cycles,instructions', '--', 'true'])
    run('perf_access_setting', ['sysctl', 'kernel.perf_event_paranoid'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'measure'])
    args = parser.parse_args()
    {'build': build, 'measure': measure}[args.stage]()
