#!/usr/bin/env python3
"""Thin orchestration over the established paired/digest/bootstrap harness."""
import argparse
import os
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
PHASE = os.environ.get('CSPLENDOR_MCTS_PHASE', '5br')
VARIANT = os.environ.get('CSPLENDOR_MCTS_VARIANT', 'v1')
assert PHASE in {'5br', '4b1', 'final'} and VARIANT.isalnum()
RAW = ROOT / 'doc/performance_experiments/raw/phase5br4b1' / PHASE / VARIANT
BASE_SOURCE = ROOT.parent / 'csplendor-reveal-transactions'
BASE = BASE_SOURCE / 'build/3d23-final-release'
RELEASE = ROOT / ('build/' + PHASE + '-release')
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run, paired = prior.save, prior.run, prior.paired
CASES = {
    'primary': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    'observable': ('parallel_scheduler', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
    'opening': ('parallel_scheduler', 'initial', 10000, ['--threads', '1']),
    'batch1': ('parallel_scheduler', 'five_moves', 10000, ['--threads', '1', '--batch-size', '1']),
    'warm': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1', '--retained-tree', 'true']),
    'legacy': ('legacy_mcts', 'midgame_250', 20000, ['--threads', '1']),
    'legacy_observable': ('legacy_mcts', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
    'legacy_warm': ('legacy_mcts', 'midgame_250', 20000, ['--threads', '1', '--retained-tree', 'true']),
    'root': ('root_parallel', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
}
if PHASE != '5br':
    CASES = {
        'primary': ('legacy_mcts', 'midgame_250', 20000, ['--threads', '1']),
        'observable': ('legacy_mcts', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
        'opening': ('legacy_mcts', 'initial', 10000, ['--threads', '1']),
        'batch1': ('legacy_mcts', 'midgame_250', 10000, ['--threads', '1', '--batch-size', '1']),
        'warm': ('legacy_mcts', 'midgame_250', 20000, ['--threads', '1', '--retained-tree', 'true']),
        'parallel': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
        'root': ('root_parallel', 'hidden_reserve', 10000, ['--threads', '1', '--determinization', 'true']),
    }


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-4000:])
        raise SystemExit(result.returncode)
    return result


def build():
    # 5B-R was rejected; its full implementation is archived with the raw
    # results. Reproduction requires restoring that patch in a separate tree.
    unit = 'mcts_game_scratch_unit' if PHASE == '5br' else 'mcts_legacy_records_unit'
    reference = ('-DCSPLENDOR_MCTS_REUSE_GAME_SCRATCH=OFF' if PHASE == '5br'
                 else '-DCSPLENDOR_MCTS_LEGACY_TREE_RECORDS=OFF')
    verify = ['-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON']
    if PHASE == '5br':
        verify.append('-DCSPLENDOR_VERIFY_MCTS_GAME_SCRATCH=ON')
    for label, flags in (
        ('release', []), ('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON']),
        ('reference-diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON', reference]),
        ('verify', verify),
    ):
        directory = ROOT / ('build/' + PHASE + '-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON',
            '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target',
            'benchmark_engine_hotpaths', unit, 'mcts_optimization_unit',
            'mcts_scheduler_lifecycle', 'mcts_scheduler_hidden', 'mcts_scheduler_limits', 'mcts_parallel_replay'])
        checked('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure', '-R',
            '^(' + unit + '|mcts_optimization_unit|mcts_scheduler_lifecycle|mcts_scheduler_hidden|mcts_scheduler_limits|mcts_parallel_replay)$'])


def diagnostics():
    import json
    for case in (('primary', 'observable', 'legacy', 'legacy_observable', 'warm') if PHASE == '5br'
                 else ('primary', 'observable', 'opening', 'batch1', 'warm')):
        workload, fixture, budget, extra = CASES[case]
        results = []
        for label in ('reference-diagnostic', 'diagnostic', 'verify'):
            directory = ROOT / ('build/' + PHASE + '-' + label)
            result = checked(label + '_' + case, [str(directory / 'benchmark_engine_hotpaths'),
                '--workload', workload, '--fixture', fixture, '--iterations', str(budget),
                '--warmup', '100', '--seed', '42', *extra])
            results.append(json.loads(result.stdout))
        # The native scheduler emits this assertion-result flag only in PERF
        # builds. It is not an execution semantic and is absent in VERIFY-only.
        def semantics(result):
            result = dict(result['semantics'])
            if 'ledger_instrumentation_totals_match' in result:
                assert result.pop('ledger_instrumentation_totals_match') is True
            return result
        assert all(r['digest'] == results[0]['digest'] and semantics(r) == semantics(results[0]) for r in results), case


def measure(stage):
    short = ('primary', 'legacy', 'observable') if PHASE == '5br' else ('primary', 'observable', 'warm')
    names = CASES if stage == 'formal' else short
    for case in names:
        paired(stage + '_' + case, *CASES[case], pairs=4 if stage == 'smoke' else 22)


def prune_build():
    import shlex
    for label, directory in (('baseline', BASE), ('candidate', RELEASE)):
        flags = []
        for line in (directory / 'CMakeFiles/benchmark_engine_hotpaths.dir/flags.make').read_text().splitlines():
            if line.startswith(('CXX_DEFINES = ', 'CXX_INCLUDES = ', 'CXX_FLAGS = ')):
                flags += shlex.split(line.split(' = ', 1)[1])
        checked('prune_build_' + label, ['c++', *flags,
            str(ROOT / 'doc/performance_experiments/phase4b1_prune_session_20260905.cpp'),
            str(directory / 'libcsplendor_core.a'), '-pthread', '-o',
            str(RELEASE / ('benchmark_legacy_prune_' + label))])


def prune_measure():
    import json
    import shlex
    target = RAW / 'prune_session.json'
    common = ['--workload', 'legacy_mcts', '--fixture', 'midgame_250',
              '--iterations', '20000', '--batch-size', '16', '--threads', '1',
              '--seed', '42', '--warmup', '0']
    done = checked('prune_session_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
        'paired', '--baseline-command', shlex.join([str(RELEASE / 'benchmark_legacy_prune_baseline'), *common]),
        '--candidate-command', shlex.join([str(RELEASE / 'benchmark_legacy_prune_candidate'), *common]),
        '--pairs', '22', '--warmups', '2', '--bootstrap-iterations', '10000',
        '--rotate-binary-slots', '--cpu-set', '4', '--baseline-repo-root', str(BASE_SOURCE),
        '--candidate-repo-root', str(ROOT), '--baseline-cmake-cache', str(BASE / 'CMakeCache.txt'),
        '--candidate-cmake-cache', str(RELEASE / 'CMakeCache.txt'), '--timeout', '45', '--output', str(target)])
    assert done.returncode == 0
    data = target.read_bytes()
    save('prune_session.json', data)
    target.unlink()  # Only this run's output, now durably archived.
    print(json.loads(data)['comparison'][0]['B_over_A'], flush=True)


def deploy():
    import hashlib
    import pybind11
    before = hashlib.sha256((RELEASE / 'benchmark_engine_hotpaths').read_bytes()).hexdigest()
    checked('configure_deploy', ['cmake', '-S', str(ROOT), '-B', str(RELEASE),
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=ON', '-DPython_EXECUTABLE=' + sys.executable,
        '-Dpybind11_DIR=' + pybind11.get_cmake_dir(), '-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' + str(ROOT / 'csplendor')])
    checked('build_deploy', ['cmake', '--build', str(RELEASE), '-j4'])
    after = hashlib.sha256((RELEASE / 'benchmark_engine_hotpaths').read_bytes()).hexdigest()
    save('deployment_binary_identity.json', {'before': before, 'after': after, 'identical': before == after})
    assert before == after
    checked('native_full', ['ctest', '--test-dir', str(RELEASE), '--output-on-failure', '-j2'])
    checked('python_full', [sys.executable, '-c', "import csplendor._csplendor as n;print(n.__file__,flush=True);import pytest;raise SystemExit(pytest.main(['tests']))"])
    checked('python_performance', [sys.executable, '-m', 'pytest', '-m', 'performance', 'tests'])
    checked('python_compile', [sys.executable, '-m', 'py_compile', *map(str, sorted((ROOT / 'csplendor').glob('*.py')))])


def sanitizers():
    for label, sanitizer, targets in (
        ('asan', 'address-undefined', ['mcts_legacy_records_unit', 'mcts_optimization_unit',
         'mcts_ownership_unit', 'mcts_scheduler_lifecycle', 'mcts_scheduler_limits',
         'mcts_scheduler_hidden', 'mcts_parallel_replay', 'mcts_parallel_stress']),
        ('tsan', 'thread', ['mcts_legacy_records_unit', 'mcts_ownership_unit', 'mcts_parallel_stress']),
    ):
        directory = ROOT / ('build/' + PHASE + '-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
            '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', '-DCSPLENDOR_SANITIZER=' + sanitizer])
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets])
        # Retain nonzero sanitizer output for diagnosis, never silently retry
        # a code finding or turn an unsupported runtime into a passing test.
        run('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
            '-R', '^(' + '|'.join(targets) + ')$'], timeout=600)


def throughput():
    def on_four_cpus(name, args, timeout=300):
        args = list(args)
        if '--cpu-set' in args:
            args[args.index('--cpu-set') + 1] = '4-7'
        return run(name, args, timeout)
    original = prior.run
    try:
        prior.run = on_four_cpus
        paired('throughput_4t', 'parallel_scheduler', 'hidden_reserve', 10000,
               ['--threads', '4', '--batch-size', '16', '--determinization', 'true'], pairs=22)
    finally:
        prior.run = original


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'diagnostics', 'smoke', 'formal', 'holdout',
                                        'prune_build', 'prune_measure', 'deploy', 'sanitizers', 'throughput'])
    args = parser.parse_args()
    measure(args.stage) if args.stage in {'smoke', 'formal', 'holdout'} else globals()[args.stage]()
