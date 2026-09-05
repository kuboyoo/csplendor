#!/usr/bin/env python3
"""3D-2 prototypes / final diagnostic-only tree; reuse existing statistics.

v1/v2 build stages require the archived prototype sources. Use VARIANT=final
for the retained tree; rejected native patches are not reapplied automatically.
"""
import argparse
import hashlib
import json
import os
import shlex
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
PHASE = '3d2'
VARIANT = os.environ.get('CSPLENDOR_TX_VARIANT', 'v1')
assert VARIANT.isalnum()
BUILD_TAG = '3d23-final' if VARIANT == 'final' else PHASE
RAW = ROOT / 'doc/performance_experiments/raw' / ('phase' + PHASE) / VARIANT
BASE_SOURCE = ROOT.parent / 'csplendor-normal-rollback'
BASE = BASE_SOURCE / 'build/3d1-release'
RELEASE = ROOT / ('build/' + BUILD_TAG + '-release')
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run, paired, CASES = prior.save, prior.run, prior.paired, prior.CASES


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-5000:], flush=True)
        raise SystemExit(result.returncode)
    return result


def build():
    for label, flags in (
        ('release', []),
        ('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON']),
        ('reference-diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON'] if VARIANT == 'final' else
            ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON', '-DCSPLENDOR_SOLVER_DECK_ROLLBACK=OFF']),
        ('verify', ['-DCSPLENDOR_VERIFY_SOLVER_ROLLBACK=ON', '-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON',
                    '-DCSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON', '-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON']),
    ):
        directory = ROOT / ('build/' + BUILD_TAG + '-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON',
            '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
        targets = ['benchmark_engine_hotpaths', 'solver_components_unit', 'solver_normal_rollback_unit']
        if VARIANT != 'final':
            targets.append('solver_deck_rollback_unit')
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets])
        for unit in targets[1:]:
            checked(label + '_' + unit, [str(directory / 'tests' / unit)])
    checked('benchmark_tool_tests', [sys.executable, '-m', 'pytest', 'tests/test_engine_benchmark_tools.py',
                                    'tests/test_public_header_matrix.py'])


def diagnostics():
    for case, (workload, fixture, budget, extra) in CASES.items():
        results = []
        for label in ('reference-diagnostic', 'diagnostic', 'verify'):
            directory = ROOT / ('build/' + BUILD_TAG + '-' + label)
            result = checked(label + '_' + case, [str(directory / 'benchmark_engine_hotpaths'),
                '--workload', workload, '--fixture', fixture, '--iterations', str(budget),
                '--warmup', '100', '--seed', '42', *extra])
            results.append(json.loads(result.stdout))
        assert all(r['digest'] == results[0]['digest'] and r['semantics'] == results[0]['semantics'] for r in results), case


def measure(stage):
    names = ('deep', 'warm', 'proof_on') if stage == 'smoke' else CASES if stage == 'formal' else ('deep', 'visible', 'proof_off', 'proof_on')
    for case in names:
        paired(stage + '_' + case, *CASES[case], pairs=4 if stage == 'smoke' else 22)


def native_build():
    for side, root in (('A', BASE_SOURCE), ('B', ROOT)):
        directory = ROOT / ('build/' + BUILD_TAG + '-native-' + side)
        checked('configure_native_' + side, ['cmake', '-S', str(ROOT / 'doc/performance_experiments/native_proof_batch_driver'),
            '-B', str(directory), '-DCMAKE_BUILD_TYPE=Release', '-DENGINE_SOURCE_ROOT=' + str(root)])
        checked('build_native_' + side, ['cmake', '--build', str(directory), '--target', 'native_proof_batch', '-j2'])


def native_measure(pairs):
    for proof in ('off', 'on'):
        name = ('native_holdout_' if pairs == 66 else 'native_') + 'proof_' + proof
        target = RAW / (name + '.json')
        common = ['--workload', 'exact_reveal', '--fixture', 'reveal_heavy', '--iterations', '200000',
                  '--depth', '1', '--warmup', '100', '--seed', '42', '--proof-dag', 'true' if proof == 'on' else 'false']
        a, b = (ROOT / ('build/' + BUILD_TAG + '-native-' + side) for side in ('A', 'B'))
        checked(name + '_invocation', [sys.executable, str(ROOT / 'scripts/run_paired_benchmarks.py'),
            'paired', '--baseline-command', shlex.join([str(a / 'native_proof_batch'), *common]),
            '--candidate-command', shlex.join([str(b / 'native_proof_batch'), *common]),
            '--baseline-repo-root', str(BASE_SOURCE), '--candidate-repo-root', str(ROOT),
            '--baseline-cmake-cache', str(a / 'CMakeCache.txt'), '--candidate-cmake-cache', str(b / 'CMakeCache.txt'),
            '--pairs', str(pairs), '--warmups', '2', '--bootstrap-iterations', '10000',
            '--rotate-binary-slots', '--cpu-set', '4', '--timeout', '45', '--output', str(target)])
        data = target.read_bytes()
        save(name + '.json', data)
        target.unlink()  # Only the plain output just archived durably under doc.
        ratio = json.loads(data)['comparison'][0]['B_over_A']
        print(name, ratio['median'], ratio['crossover_block_bootstrap_ci95'], flush=True)


def deploy():
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


def verify():
    import pybind11
    import shutil
    directory = ROOT / ('build/' + BUILD_TAG + '-verify')
    package = directory / 'python/csplendor'
    package.mkdir(parents=True, exist_ok=True)
    for path in (ROOT / 'csplendor').glob('*.py'):
        shutil.copy2(path, package / path.name)
    checked('configure_verify_python', ['cmake', '-S', str(ROOT), '-B', str(directory),
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=ON', '-DPython_EXECUTABLE=' + sys.executable,
        '-Dpybind11_DIR=' + pybind11.get_cmake_dir(), '-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' + str(package)])
    checked('build_verify_python', ['cmake', '--build', str(directory), '--target', '_csplendor', '-j4'])
    checked('verify_python_solver', [sys.executable, '-c',
        "import sys;sys.path.insert(0,sys.argv[1]);import csplendor._csplendor as n;print(n.__file__,flush=True);import pytest;raise SystemExit(pytest.main(['tests/test_reveal_verified_solver.py']))", str(package.parent)])
    for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return'):
        for simple in ('false', 'true'):
            checked('verify_special_' + fixture + '_' + simple, [str(directory / 'benchmark_engine_hotpaths'),
                '--workload', 'exact_reveal', '--fixture', fixture, '--depth', '3', '--iterations', '10000',
                '--warmup', '100', '--simple-payment', simple])


def asan():
    directory = ROOT / ('build/' + BUILD_TAG + '-asan')
    checked('configure_asan', ['cmake', '-S', str(ROOT), '-B', str(directory),
        '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
        '-DCSPLENDOR_SANITIZER=address-undefined', '-DCSPLENDOR_VERIFY_SOLVER_ROLLBACK=ON',
        '-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON', '-DCSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON',
        '-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON'])
    checked('build_asan', ['cmake', '--build', str(directory), '-j4'])
    checked('asan_native_full', ['ctest', '--test-dir', str(directory), '--output-on-failure', '-j2'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'diagnostics', 'smoke', 'formal', 'holdout', 'native_build', 'native_measure', 'deploy', 'verify', 'asan'])
    parser.add_argument('--pairs', type=int, choices=[22, 66], default=22)
    args = parser.parse_args()
    if args.stage in {'smoke', 'formal', 'holdout'}:
        measure(args.stage)
    elif args.stage == 'native_measure':
        native_measure(args.pairs)
    else:
        globals()[args.stage]()
