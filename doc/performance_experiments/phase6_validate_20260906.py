#!/usr/bin/env python3
"""Final integration gates, separate from all performance measurements."""
import csv
import gzip
import hashlib
import json
import sys
from pathlib import Path
import phase6_record_20260906 as r


def archives():
    root = r.ROOT/'doc/performance_experiments'
    checked = {}
    for evidence in sorted(root.glob('*evidence*.json')):
        obj = json.loads(evidence.read_text())
        for item in obj.get('artifacts', []):
            p = r.ROOT/item['path']
            assert p.is_relative_to(root) and p.is_file(), p
            assert hashlib.sha256(p.read_bytes()).hexdigest() == item['sha256'], p
            checked[str(p.relative_to(r.ROOT))] = item['sha256']
    table = root/'raw/manifest.tsv'
    with table.open() as stream:
        for row in csv.DictReader(stream, delimiter='\t'):
            # The old migration manifest uses paths relative to its own root.
            p = r.ROOT/row['archive_path']
            assert hashlib.sha256(p.read_bytes()).hexdigest() == row['archive_sha256'], p
            assert hashlib.sha256(gzip.decompress(p.read_bytes())).hexdigest() == row['original_sha256'], p
            checked[str(p.relative_to(r.ROOT))] = row['archive_sha256']
    r.save('prior_artifact_audit.json', {'files': checked, 'count': len(checked)})


def integration():
    for profile in ('portable', 'lto'):
        r.run('full_build_' + profile, ['cmake','--build',r.directory(profile),'-j4'])
        r.run('full_native_' + profile, ['ctest','--test-dir',r.directory(profile),'--output-on-failure','-j2'])
    r.run('full_python', [sys.executable,'-c',
        'import csplendor._csplendor as c;print(c.__file__,flush=True);'
        'import pytest;raise SystemExit(pytest.main(["tests"]))'])
    r.run('python_performance', [sys.executable,'-m','pytest','-m','performance','tests'])
    r.run('python_compile', [sys.executable,'-m','py_compile',
        *sorted((r.ROOT/'csplendor').glob('*.py')), *sorted((r.ROOT/'doc/performance_experiments').glob('phase6*.py'))])
    r.build('asan', ['-DCSPLENDOR_SANITIZER=address-undefined'],
        ('state_feature_table_unit','action_selection_unit','solver_components_unit','state_copy_unit',
         'mcts_scheduler_limits','mcts_parallel_replay','mcts_legacy_records_unit','encoding_schema_unit'))
    r.build('tsan', ['-DCSPLENDOR_SANITIZER=thread'],
        ('mcts_parallel_unit','mcts_scheduler_limits','mcts_parallel_replay','mcts_ownership_unit'))
    r.run('perf_environment',['perf','stat','-e','cycles','--','true'], check=False)
    r.run('portable_wheel',[sys.executable,'setup.py','bdist_wheel','--dist-dir','build/phase6-wheel'])
    r.run('native_wheel_guard',['env','CSPLENDOR_CPU_TARGET=native',sys.executable,'setup.py',
                              'bdist_wheel','--dist-dir','build/phase6-native-wheel-forbidden'], check=False)
    r.run('skip_build_wheel_guard',[sys.executable,'setup.py','bdist_wheel','--skip-build',
                                  '--dist-dir','build/phase6-skip-wheel-forbidden'], check=False)


def final_checks():
    r.build('oracle', ['-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON',
        '-DCSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON', '-DCSPLENDOR_VERIFY_SOLVER_ROLLBACK=ON',
        '-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON'],
        ('solver_components_unit','state_copy_unit','mcts_parallel_replay'))
    r.build('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON','-DCSPLENDOR_ENABLE_LTO=ON'],
        ('benchmark_engine_hotpaths','perf_counters_unit'))
    for name in ('primary','shared'):
        workload, fixture, n, flags = r.CASES[name]
        r.run('diagnostic_' + name, [r.directory('diagnostic')/'benchmark_engine_hotpaths',
            '--workload',workload,'--fixture',fixture,'--iterations',str(n),'--seed','42',*flags])
    wheel = next((r.ROOT/'build/phase6-wheel').glob('*.whl'))
    target = r.ROOT/'build/phase6-wheel-unpacked'
    assert not target.exists()
    r.run('wheel_unpack',['unzip','-q',wheel,'-d',target])
    extension = next((target/'csplendor').glob('_csplendor*.so'))
    r.run('wheel_import',[sys.executable,r.ROOT/'doc/performance_experiments/phase6_python_probe_20260906.py',extension])
    r.save('wheel_identity.json', {'path':str(wheel),'sha256':hashlib.sha256(wheel.read_bytes()).hexdigest(),
        'extension_sha256':hashlib.sha256(extension.read_bytes()).hexdigest(), 'bytes':wheel.stat().st_size})
    for profile in ('portable','lto'):
        # Final CMake after rejected options have been removed must reproduce
        # the exact deployment executables used for the formal measurements.
        old = json.loads(gzip.decompress((r.RAW/('audit_'+profile+'.json.gz')).read_bytes()))
        binary = r.directory(profile)/'benchmark_engine_hotpaths'
        assert hashlib.sha256(binary.read_bytes()).hexdigest() == old['binaries'][0]['sha256']
    r.save('final_source.json', {'source':r.source(r.ROOT), 'native_formal_binaries_unchanged':True})


if __name__ == '__main__':
    {'archives':archives, 'integration':integration, 'final':final_checks}[sys.argv[1]]()
