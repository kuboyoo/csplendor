#!/usr/bin/env python3
"""5C-B orchestration using the existing fixed-slot ABBA/semantic runner."""
import os
import json
import shlex
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
VARIANT = os.environ.get('CSPLENDOR_5CB_VARIANT', 'table')
assert VARIANT.replace('_', '').isalnum()
RAW = ROOT / 'doc/performance_experiments/raw/phase5cb' / VARIANT
BASE_SOURCE = ROOT.parent / 'csplendor-action-selection'
BASE = BASE_SOURCE / 'build/final-release'
RELEASE = ROOT / ('build/5cb-' + VARIANT)
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run, paired = prior.save, prior.run, prior.paired


def checked(name, args, timeout=1200):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-6000:], flush=True)
        raise SystemExit(result.returncode)
    return result


def build(label, flags=(), targets=('benchmark_engine_hotpaths', 'state_feature_table_unit', 'mcts_optimization_unit')):
    directory = ROOT / ('build/5cb-' + label)
    checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
        '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
        '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
    checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets])
    checked('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
                            '-R', '^(' + '|'.join(t for t in targets if t != 'benchmark_engine_hotpaths') + ')$'])


CASES = {
    'primary': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    'features': ('state_encoder', 'midgame_250', 100000, []),
    'hidden': ('state_encoder', 'hidden_reserve', 100000, []),
    'opening': ('state_encoder', 'initial', 100000, []),
    'solver': ('visible_solver', 'five_moves', 100000, []),
    'random': ('random_selfplay_apply', 'initial', 20000, []),
    'mask': ('action_mask', 'midgame_250', 200000, []),
}


def measure(stage, names=None):
    for name in names or (('primary', 'features') if stage == 'smoke' else CASES):
        paired(stage + '_' + name, *CASES[name], pairs=4 if stage == 'smoke' else 22)


def launchers():
    source = ROOT / 'doc/performance_experiments/phase5cb_boundary_launcher_20260906.cpp'
    benchmark = ROOT / 'doc/performance_experiments/phase5cb_boundary_bench_20260906.py'
    for label, root, flags in [('reference', BASE_SOURCE, []), ('candidate', ROOT, []),
                               ('matched', ROOT, ['-DCSPLENDOR_BOUNDARY_LEGACY=1'])]:
        checked('launcher_' + label, ['c++', '-std=c++17', '-O2', str(source),
            '-DCSPLENDOR_PYTHON_BINARY="' + sys.executable + '"',
            '-DCSPLENDOR_BOUNDARY_SCRIPT="' + str(benchmark) + '"',
            '-DCSPLENDOR_BOUNDARY_REPO="' + str(root) + '"', *flags,
            '-o', str(RELEASE / ('boundary_' + label))])


def python_pair(stage, pipeline=False, matched=False):
    name = stage + ('_pipeline' if pipeline else '_features') + ('_matched' if matched else '')
    benchmark = ROOT / 'doc/performance_experiments/phase5cb_boundary_bench_20260906.py'
    common = ['--iterations', '10000' if pipeline else '50000']
    if pipeline: common += ['--pipeline']
    a_root = ROOT if matched else BASE_SOURCE
    a = [str(RELEASE / ('boundary_matched' if matched else 'boundary_reference')), *common]
    b = [str(RELEASE / 'boundary_candidate'), *common]
    output = RAW / (name + '.json')
    RAW.mkdir(parents=True, exist_ok=True)
    assert not output.exists()
    result = run(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
        'paired', '--baseline-command', shlex.join(a), '--candidate-command', shlex.join(b),
        '--pairs', '4' if stage == 'smoke' else '22', '--warmups', '2', '--bootstrap-iterations', '10000',
        '--rotate-binary-slots', '--cpu-set', '4', '--timeout', '45',
        '--baseline-repo-root', str(a_root), '--candidate-repo-root', str(ROOT),
        '--baseline-cmake-cache', str((RELEASE if matched else BASE) / 'CMakeCache.txt'),
        '--candidate-cmake-cache', str(RELEASE / 'CMakeCache.txt'), '--output', str(output)], timeout=1200)
    if output.exists():
        data = output.read_bytes()
        save(name + '.json', data)
        output.unlink() # only this run's already archived uncompressed output
        ratio = json.loads(data)['comparison'][0]['B_over_A']
        print(name, ratio['median'], ratio['crossover_block_bootstrap_ci95'], flush=True)
    assert result.returncode == 0, name


def validate_final():
    checked('native_full', ['ctest', '--test-dir', str(RELEASE), '--output-on-failure', '-j2'])
    checked('python_full', [sys.executable, '-c',
        'import csplendor._csplendor as n;print(n.__file__,flush=True);'
        'import pytest;raise SystemExit(pytest.main(["tests"]))'])
    checked('python_performance', [sys.executable, '-m', 'pytest', '-m', 'performance', 'tests'])
    checked('python_compile', [sys.executable, '-m', 'py_compile',
        *map(str, sorted((ROOT / 'csplendor').glob('*.py')))])
    build('final-diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON'])
    build('final-asan', ['-DCSPLENDOR_SANITIZER=address-undefined'],
          ('state_feature_table_unit', 'encoding_schema_unit', 'rule_query_unit', 'mcts_optimization_unit'))
    run('perf_environment', ['perf', 'stat', '-e', 'cycles', '--', 'true'])


if __name__ == '__main__':
    if sys.argv[1] == 'validate':
        validate_final()
    elif sys.argv[1].startswith('python_'):
        stage = sys.argv[1].removeprefix('python_')
        python_pair(stage)
        python_pair(stage, pipeline=True)
    else:
        measure(sys.argv[1], sys.argv[2:] or None)
