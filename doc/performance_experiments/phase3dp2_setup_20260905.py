#!/usr/bin/env python3
"""Persistent setup/diagnostics; the existing paired harness owns timing."""
import argparse
import json
import sys
from phase3dp2_record_20260905 import ROOT, BASE, BASE_SOURCE, RELEASE, CASES, run, save


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    assert result.returncode == 0, result.stdout.decode(errors='replace')[-4000:]
    return result


def build():
    for label, flags in (
        ('release', []),
        ('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON']),
        ('reference-diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON', '-DCSPLENDOR_REUSE_SEARCH_SCRATCH=OFF']),
        ('verify', ['-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON', '-DCSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON', '-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON']),
    ):
        directory = ROOT / ('build/3dp2-' + label)
        checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
            '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON',
            '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
        checked('build_' + label, ['cmake', '--build', str(directory), '-j4',
            '--target', 'benchmark_engine_hotpaths', 'solver_components_unit'])
        checked('unit_' + label, [str(directory / 'tests/solver_components_unit')])
    checked('benchmark_tool_tests', [sys.executable, '-m', 'pytest', 'tests/test_engine_benchmark_tools.py', 'tests/test_public_header_matrix.py'])


def diagnostics():
    for case, (workload, fixture, budget, extra) in CASES.items():
        results = []
        for label in ('reference-diagnostic', 'diagnostic', 'verify'):
            directory = ROOT / ('build/3dp2-' + label)
            result = checked(label + '_' + case, [str(directory / 'benchmark_engine_hotpaths'),
                '--workload', workload, '--fixture', fixture, '--iterations', str(budget),
                '--warmup', '100', '--seed', '42', *extra])
            results.append(json.loads(result.stdout))
        assert all(r['digest'] == results[0]['digest'] and r['semantics'] == results[0]['semantics'] for r in results), case


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'diagnostics'])
    args = parser.parse_args()
    {'build': build, 'diagnostics': diagnostics}[args.stage]()
