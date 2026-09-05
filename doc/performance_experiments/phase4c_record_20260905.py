#!/usr/bin/env python3
"""4C orchestration on the existing paired/digest/manifest harness."""
import argparse
import gzip
import hashlib
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
PHASE = os.environ.get('CSPLENDOR_4C_PHASE', 'diagnostic')
VARIANT = os.environ.get('CSPLENDOR_4C_VARIANT', 'v1')
assert PHASE in {'diagnostic', '4c1', '4c2', '4c3', 'final'} and VARIANT.isalnum()
RAW = ROOT / 'doc/performance_experiments/raw/phase4c' / PHASE / VARIANT
BASE_SOURCE = Path(os.environ.get('CSPLENDOR_4C_BASE_SOURCE', str(ROOT.parent / 'csplendor-mcts-concurrency-baseline')))
BASE = BASE_SOURCE / os.environ.get('CSPLENDOR_4C_BASE_BUILD', 'build/4c-baseline-release')
RELEASE = ROOT / ('build/' + PHASE + '-release')
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run = prior.save, prior.run


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-4000:], flush=True)
        raise SystemExit(result.returncode)
    return result


CASES = {
    'primary': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '8', '--determinization', 'true']),
    'serial_exact': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    'serial_hidden': ('parallel_scheduler', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
    'four_threads': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '4', '--determinization', 'true']),
    'sixteen_threads': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '16', '--determinization', 'true']),
    'batch1': ('parallel_scheduler', 'hidden_reserve', 10000, ['--threads', '8', '--batch-size', '1', '--determinization', 'true']),
    'batch64': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '8', '--batch-size', '64', '--determinization', 'true']),
    'latency250': ('parallel_scheduler', 'hidden_reserve', 10000, ['--threads', '8', '--latency-us', '250', '--determinization', 'true']),
    'warm': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '8', '--retained-tree', 'true', '--determinization', 'true']),
    'coarse': ('parallel_scheduler', 'hidden_reserve', 10000, ['--threads', '8', '--tree-backend', 'coarse', '--determinization', 'true']),
    'root': ('root_parallel', 'hidden_reserve', 10000, ['--threads', '4', '--determinization', 'true']),
    'legacy': ('legacy_mcts', 'midgame_250', 20000, ['--threads', '1']),
}


def paired(name, case, pairs=22):
    workload, fixture, budget, extra = CASES[case]
    threads = int(extra[extra.index('--threads') + 1])
    cpus = '4' if threads == 1 else '4-' + str(threads + 3)
    def on_cpus(name, args, timeout=600):
        args = list(args)
        if '--cpu-set' in args:
            args[args.index('--cpu-set') + 1] = cpus
        return run(name, args, timeout)
    original = prior.run
    try:
        prior.run = on_cpus
        prior.paired(name, workload, fixture, budget, extra, pairs)
    finally:
        prior.run = original


def measure(stage):
    names = ('primary', 'serial_exact', 'batch64') if stage == 'smoke' else CASES if stage == 'formal' else ('primary', 'serial_exact', 'latency250')
    for case in names:
        paired(stage + '_' + case, case, 4 if stage == 'smoke' else 22)


def build():
    reference = {'4c1': 'CSPLENDOR_STRIPED_LEDGER_METRICS', '4c2': 'CSPLENDOR_INLINE_RESERVATIONS', '4c3': 'CSPLENDOR_FUSED_NODE_SELECTION'}.get(PHASE)
    targets = ['benchmark_engine_hotpaths', 'mcts_parallel_unit', 'mcts_optimization_unit',
               'mcts_scheduler_lifecycle', 'mcts_scheduler_limits', 'mcts_scheduler_hidden',
               'mcts_parallel_replay', 'mcts_ownership_unit', 'perf_counters_unit']
    unit = ROOT / ('tests/' + PHASE + '_unit.cpp')
    if unit.exists():
        targets.append(PHASE + '_unit')
    for label, flags in [('release', []), ('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON']),
                         *([('reference', ['-D' + reference + '=OFF'])] if reference else [])]:
        directory = ROOT / ('build/' + PHASE + '-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
            '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets])
        checked('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
                                '-R', '^(' + '|'.join(targets[1:]) + ')$'])


def diagnostics():
    directory = ROOT / ('build/' + PHASE + '-diagnostic')
    for case in ('primary', 'serial_exact', 'batch64', 'latency250'):
        workload, fixture, budget, extra = CASES[case]
        checked('profile_' + case, [str(directory / 'benchmark_engine_hotpaths'),
            '--workload', workload, '--fixture', fixture, '--iterations', str(budget),
            '--warmup', '100', '--seed', '42', *extra])


def deploy():
    """Test the shipped source; assert Python enablement did not relink timing code."""
    import pybind11
    before = hashlib.sha256((RELEASE / 'benchmark_engine_hotpaths').read_bytes()).hexdigest()
    checked('configure_deploy', ['cmake', '-S', str(ROOT), '-B', str(RELEASE),
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=ON', '-DPython_EXECUTABLE=' + sys.executable,
        '-Dpybind11_DIR=' + pybind11.get_cmake_dir(),
        '-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' + str(ROOT / 'csplendor')])
    checked('build_deploy', ['cmake', '--build', str(RELEASE), '-j4'], timeout=1200)
    after = hashlib.sha256((RELEASE / 'benchmark_engine_hotpaths').read_bytes()).hexdigest()
    save('deployment_binary_identity.json', {'before': before, 'after': after, 'identical': before == after})
    assert before == after
    checked('native_full', ['ctest', '--test-dir', str(RELEASE), '--output-on-failure', '-j2'])
    checked('python_full', [sys.executable, '-c',
        "import csplendor._csplendor as n;print(n.__file__,flush=True);"
        "import pytest;raise SystemExit(pytest.main(['tests']))"], timeout=1200)
    checked('python_performance', [sys.executable, '-m', 'pytest', '-m', 'performance', 'tests'])
    checked('python_compile', [sys.executable, '-m', 'py_compile',
        *map(str, sorted((ROOT / 'csplendor').glob('*.py')))])


def sanitizers():
    # All three speed prototypes were rejected. Exercise the actually retained
    # depth/TLS instrumentation, not a PERF-OFF build with those sites removed.
    assert PHASE == 'final'
    for label, sanitizer, targets in (
        ('asan-perf', 'address-undefined', ['perf_counters_unit', 'mcts_parallel_unit',
         'mcts_ownership_unit', 'mcts_scheduler_lifecycle', 'mcts_scheduler_limits',
         'mcts_scheduler_hidden', 'mcts_parallel_replay', 'mcts_parallel_stress']),
        ('tsan-perf', 'thread', ['perf_counters_unit', 'mcts_ownership_unit',
         'mcts_parallel_stress', 'mcts_scheduler_limits']),
    ):
        directory = ROOT / ('build/' + PHASE + '-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
            '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', '-DCSPLENDOR_PERF_INSTRUMENTATION=ON',
            '-DCSPLENDOR_SANITIZER=' + sanitizer])
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets], timeout=1200)
        # Keep failures verbatim. Unsupported runtime is N/A, not a passing test.
        run('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
            '-R', '^(' + '|'.join(targets) + ')$'], timeout=600)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'diagnostics', 'smoke', 'formal', 'holdout',
                                        'deploy', 'sanitizers'])
    args = parser.parse_args()
    measure(args.stage) if args.stage in {'smoke', 'formal', 'holdout'} else globals()[args.stage]()
