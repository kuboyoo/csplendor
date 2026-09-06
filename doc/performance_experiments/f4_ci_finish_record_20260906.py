#!/usr/bin/env python3
"""Append-only F4 CI finishing evidence, without deployment or main changes."""
import gzip
import ast
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import zipfile

import f4_compat_record_20260906 as previous

ROOT = previous.ROOT
BASE = 'da73c5ddfb4cd4da6a1525e4b362c48fe24b6df1'
MAIN = 'f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc'
CODE = '8b6dd8b48526dfa1eda8ccbc00a9355f5abc8cdb'
RAW = ROOT / 'doc/performance_experiments/raw/f4_ci_finish_20260906'
ISOLATED = ROOT.parent.parent / 'csplendor-f4-ci-finish-20260906'
WHEEL_SOURCE = ROOT.parent / 'csplendor-f4-ci-finish-wheel3-20260906'
previous.prior.RAW = RAW
previous.prior.prior.RAW = RAW
previous.prior.ISOLATED = ISOLATED
save, sha, git, source, run = previous.save, previous.sha, previous.git, previous.source, previous.run


def read_raw(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def start():
    assert git(ROOT, 'rev-parse', 'HEAD') == BASE
    assert git(ROOT, 'rev-parse', 'origin/main') == MAIN
    assert git(ROOT, 'merge-base', MAIN, BASE) == MAIN
    manifest = json.loads((ROOT / 'doc/performance_experiments/f4_compat_manifest_20260906.json').read_text())
    for item in manifest['artifacts']:
        assert sha(ROOT / item['path']) == item['sha256']
    assert source(ROOT)['source_digest'] == manifest['source_digest']
    save('start', {'source': source(ROOT), 'main': MAIN, 'local_main': git(ROOT, 'rev-parse', 'main'),
                   'original': source(ROOT.parent / 'csplendor'), 'prior_manifest': manifest,
                   'prior_raw_verified': len(manifest['artifacts'])})
    print('PASS: fixed HEAD/base/source and 61 prior artifact hashes')


def ci_snapshot(label, run_id, expected):
    assert run(label + '_ci', ROOT, ['gh', 'run', 'view', run_id, '--repo', 'kuboyoo/csplendor',
               '--json', 'databaseId,url,headSha,event,status,conclusion,jobs']) == 0
    data = json.loads(read_raw(label + '_ci')['stdout'])
    assert data['headSha'] == expected and data['status'] == 'completed'
    assert run(label + '_pr', ROOT, ['gh', 'api', 'repos/kuboyoo/csplendor/pulls/26', '--jq',
        '{head:.head.sha,base:.base.sha,test_merge:.merge_commit_sha,merged,auto_merge}']) == 0
    pr = json.loads(read_raw(label + '_pr')['stdout'])
    assert pr['head'] == expected and pr['base'] == MAIN and not pr['merged'] and pr['auto_merge'] is None
    for job in data['jobs']:
        if not job['steps']:
            continue
        name = label + '_job_' + str(job['databaseId'])
        assert run(name, ROOT, ['gh', 'api', 'repos/kuboyoo/csplendor/actions/jobs/' +
                               str(job['databaseId']) + '/logs']) == 0
        log = read_raw(name)['stdout']
        assert all(value in log for value in [expected, MAIN, pr['test_merge']]), job['name']
    print('PASS: all applicable job checkout head/base/test-merge verified')


def provenance(record_name='provenance3', acceptance_name='wheel_cycle3_acceptance'):
    initial = read_raw('start')
    current, clean = source(ROOT), source(WHEEL_SOURCE)
    assert current['source_digest'] == clean['source_digest']
    assert git(WHEEL_SOURCE, 'rev-parse', 'HEAD') == CODE
    assert git(WHEEL_SOURCE, 'status', '--porcelain') == ''
    assert source(ROOT.parent / 'csplendor') == initial['original']
    assert git(ROOT, 'rev-parse', 'main') == initial['local_main']
    for item in initial['prior_manifest']['artifacts']:
        assert sha(ROOT / item['path']) == item['sha256']
    f1 = previous.prior.prior.ENGINE
    assert git(ROOT, 'diff', f1, '--', 'csplendor', 'CMakeLists.txt', 'setup.py',
               '_build_support.py', 'pyproject.toml', '.github') == ''
    production = git(ROOT, 'diff', '--name-only', f1, CODE, '--', 'src').splitlines()
    assert production == ['src/noble_data.h', 'src/solver_action_filter.h', 'src/solver_path.h']
    assert git(ROOT, 'diff', BASE, CODE, '--', 'src/noble_data.h', 'scripts/run_phase0_baseline.py') == ''
    assert git(ROOT, 'diff', '3c8921c06b47d48edd558c0c807890ab3f3485dc', CODE,
               '--', 'src', 'csplendor', 'CMakeLists.txt', 'setup.py', '_build_support.py', 'pyproject.toml') == ''
    def definitions(text):
        return {n.name: ast.dump(n, include_attributes=False) for n in ast.parse(text).body
                if isinstance(n, ast.FunctionDef)}
    old = definitions(git(ROOT, 'show', BASE + ':scripts/run_paired_benchmarks.py'))
    new = definitions((ROOT / 'scripts/run_paired_benchmarks.py').read_text())
    assert set(new) - set(old) == {'_gnu_time_supports_rss'}
    assert {name for name in old if old[name] != new[name]} == {'_run_with_rss', '_terminate_process_group'}
    acceptance = json.loads(read_raw(acceptance_name)['stdout'])
    assert acceptance['status'] == 'PASS'
    ident = acceptance['identity']
    assert sha(ident['wheel']) == ident['wheel_sha256']
    assert sha(ident['extension']) == ident['extension_sha256']
    old_identity = initial['prior_manifest']['wheel_acceptance']['identity']
    unchanged = [name for name, digest in ident['installed_files'].items()
                 if old_identity['installed_files'].get(name) == digest]
    assert len(unchanged) == 28 and all(name.endswith('.py') for name in unchanged)
    assert ident['extension_sha256'] != old_identity['extension_sha256']
    with zipfile.ZipFile(old_identity['wheel']) as before, zipfile.ZipFile(ident['wheel']) as after:
        wheel_diffs = [name for name in before.namelist() if before.read(name) != after.read(name)]
        member = 'csplendor-0.1.0.dist-info/METADATA'
        assert before.read(member).split(b'\n\n', 1)[0] == after.read(member).split(b'\n\n', 1)[0]
    build_suffix = 'build/temp.linux-x86_64-cpython-312/portable/default/release'
    build = WHEEL_SOURCE / build_suffix
    old_source = previous.WHEEL_SOURCE
    old_build = old_source / build_suffix
    def cache(path):
        return dict(line.split('=', 1) for line in path.read_text().splitlines()
                    if line.startswith(('CSPLENDOR_', 'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:',
                                        'CMAKE_BUILD_TYPE:', 'CMAKE_CXX_COMPILER:')))
    settings = cache(build / 'CMakeCache.txt')
    assert settings == cache(old_build / 'CMakeCache.txt')
    flags = {}
    for name in ['CMakeFiles/_csplendor.dir/flags.make', 'CMakeFiles/_csplendor.dir/link.txt']:
        flags[name] = (build / name).read_text()
        expected = (old_build / name).read_text().replace(str(old_source), str(WHEEL_SOURCE)).replace(
            str(previous.ISOLATED), str(ISOLATED))
        assert flags[name] == expected
    binaries = {}
    for name, path in [('F1_common', ROOT / 'build/final-common-candidate/final_common_benchmark'),
                       ('fixed_common', ROOT / 'build/f4-ci-finish-common/final_common_benchmark'),
                       ('F1_extension', Path(old_identity['extension'])),
                       ('fixed_extension', Path(ident['extension']))]:
        section = subprocess.run(['objcopy', '-O', 'binary', '--only-section=.text', str(path), '/dev/stdout'],
                                 check=True, capture_output=True).stdout
        binaries[name] = {'path': str(path), 'sha256': sha(path), 'bytes': path.stat().st_size,
                          'text_sha256': hashlib.sha256(section).hexdigest(), 'text_bytes': len(section)}
    guards = {}
    for name in ['guard_exact', 'guard_visible', 'runner_final_aa']:
        data = json.loads(read_raw(name)['stdout'])
        slots = {}
        for i, pair in enumerate(data['pairs']):
            assert pair['order'] == (['A', 'B'] if i % 2 == 0 else ['B', 'A'])
            for side in ['A', 'B']:
                slot = pair[side]['binary_slot']
                slots.setdefault(slot['slot_id'], set()).add((slot['slot_device'], slot['slot_inode']))
                assert slot['source_binary_sha256'] == slot['staged_binary_sha256'] == slot['post_run_sha256']
                assert not Path(slot['slot_path']).exists()
                assert pair[side]['runner_rss_kib'] > 0
        assert len(slots) == 2 and all(len(values) == 1 for values in slots.values())
        guards[name] = {'settings': data['settings'], 'comparison': data['comparison']}
    prefix = (ROOT / 'src/noble_data.h').read_text().split('constexpr bool noble_mask_entries_are_exact()')[0]
    prefix_sha = hashlib.sha256(prefix.encode()).hexdigest()
    old_provenance = json.loads(gzip.decompress((ROOT / 'doc/performance_experiments/raw/f4_compat_20260906/provenance.json.gz').read_bytes()))
    assert prefix_sha == old_provenance['noble_data_table_runtime_prefix_sha256']
    save(record_name, {'code_commit': CODE, 'F1_code': f1, 'source': current, 'clean_source': clean,
        'production_diff_from_F1': production,
        'measurement_diff_from_F1': git(ROOT, 'diff', '--name-only', f1, CODE, '--', 'scripts').splitlines(),
        'noble_data_full_header_unchanged_from_previous_fix': True,
        'noble_data_prefix_sha256': prefix_sha,
        'build_dependency_CI_unchanged': True, 'cache': settings, 'flags_and_link': flags,
        'unchanged_driver_top_level_ASTs': sorted(name for name in old if old[name] == new[name]),
        'wheel_member_diffs': wheel_diffs, 'unchanged_28_python_files': unchanged,
        'F1_F2_extension_byte_identity': False, 'binaries': binaries,
        'acceptance': acceptance, 'guards': guards,
        'user_worktree_local_main_prior_raw_unchanged': True})
    print(json.dumps({'source_digest': current['source_digest'], 'wheel_member_diffs': wheel_diffs,
                      'binaries': binaries, 'identity': ident}, indent=2))


def manifest():
    provenance_data = read_raw('provenance3')
    ci = json.loads(read_raw('cycle3_ci')['stdout'])
    pr = json.loads(read_raw('cycle3_pr')['stdout'])
    assert pr['head'] == CODE and ci['headSha'] == CODE
    assert source(ROOT)['source_digest'] == provenance_data['source']['source_digest']
    counts, summaries, errors, steps = {}, {}, {}, {}
    for job in ci['jobs']:
        counts[job['conclusion']] = counts.get(job['conclusion'], 0) + 1
        steps[job['name']] = {step['name']: step['conclusion'] for step in job['steps']}
        if not job['steps']:
            assert job['name'] == 'nightly-native-soak' and job['conclusion'] == 'skipped'
            continue
        log = read_raw('cycle3_job_' + str(job['databaseId']))['stdout']
        assert all(sha_value in log for sha_value in [CODE, MAIN, pr['test_merge']])
        summaries[job['name']] = [line for line in log.splitlines() if
            ('passed' in line and ('deselected' in line or '100% tests passed' in line)) or 'Total coverage:' in line]
        errors[job['name']] = [line for line in log.splitlines() if any(marker in line for marker in
            ['error:', 'error C', 'FAILED tests/', 'ThreadSanitizer: data race', 'ERROR: AddressSanitizer'])]
        if job['name'].startswith('sanitizer') and job['conclusion'] == 'success':
            assert '100% tests passed, 0 tests failed out of 44' in log
    local_names = ['python_full_v3', 'python_coverage_v3', 'cleanup_v3', 'lint_v3', 'security_final',
                   'compile_final', 'native_clang', 'native_perf', 'native_gcc13', 'native_asan',
                   'native_tsan_hook', 'hook_tsan_v3', 'hook_asan_v3', 'wheel_cycle3_acceptance']
    local = {}
    for name in local_names:
        row = read_raw(name)
        assert row['exit_code'] == 0, name
        local[name] = {key: row[key] for key in ['command', 'cwd', 'exit_code', 'elapsed_seconds']}
    artifacts = [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p), 'bytes': p.stat().st_size}
                 for p in sorted(RAW.glob('*.json.gz'))]
    data = {'schema': 'csplendor.f4_ci_finish.v1',
        'decision': 'READY_FOR_REVIEW' if counts == {'success': 16, 'skipped': 1} else 'BLOCKED',
        'starting_head': BASE, 'fixed_code_head': CODE, 'F1_code': previous.prior.prior.ENGINE,
        'origin_main': MAIN, 'fixed_test_merge': pr['test_merge'],
        'branch': git(ROOT, 'branch', '--show-current'),
        'source_digest': provenance_data['source']['source_digest'],
        'fix_commits': git(ROOT, 'log', '--reverse', '--format=%H %s', BASE + '..' + CODE).splitlines(),
        'production_diff_from_F1': provenance_data['production_diff_from_F1'],
        'measurement_diff_from_F1': provenance_data['measurement_diff_from_F1'],
        'build_dependency_CI_unchanged': True, 'F1_F2_extension_byte_identical': False,
        'wheel_acceptance': provenance_data['acceptance'],
        'build_tools': json.loads(read_raw('build_tools_final')['stdout']),
        'provenance': 'raw/f4_ci_finish_20260906/provenance3.json.gz',
        'guard_summary': {name: [{
            'workload': row['workload'], 'fixture': row['fixture'], 'digest': row['digest'],
            'B_over_A': row['B_over_A'], 'absolute': row['absolute'],
            'all_counters_identical': all(value['identical_in_every_pair'] for value in row['counters'].values())}
            for row in result['comparison']] for name, result in provenance_data['guards'].items()},
        'CI_code': ci, 'CI_counts': counts, 'CI_summaries': summaries, 'CI_errors': errors,
        'CI_steps': steps, 'all_applicable_checkout_head_base_test_merge_verified': True,
        'local_checks': local,
        'prior_failed_runs': [34015102420, 34016455084, 34016913244],
        'not_executed': ['full F1 rerun', 'current binary real-model reload', 'GPU', 'browser',
                         'AI playing strength', 'nightly soak', 'merge', 'production deployment'],
        'reuse_scope': 'F1/F2 retained as historical old-binary evidence; unchanged table/runtime prefix and 28 Python files/build/fixtures reused. Changed native and extension .text are covered by new scoped solver guards/native/Python/wheel acceptance, not claimed byte-identical.',
        'limits': ['asynchronous MCTS and additional LTO cumulative gains remain uncertain',
                   'depth7 million-node guard is UNKNOWN, not a completed mate-time benchmark',
                   'Windows actual ELF/POSIX slot I/O unsupported, reference contracts run',
                   'Darwin GNU RSS unsupported -> None; actual timeout cleanup tested'],
        'final_CI_policy': 'Record-only commit follows this code snapshot. Verify its head/test merge and publish final CI in PR body without recursive evidence commits.',
        'merge_deployment': 'NOT_EXECUTED_AWAITING_APPROVAL', 'artifacts': artifacts}
    path = ROOT / 'doc/performance_experiments/f4_ci_finish_manifest_20260906.json'
    with path.open('x') as stream:
        json.dump(data, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print(json.dumps({'decision': data['decision'], 'counts': counts, 'artifacts': len(artifacts),
                      'source_digest': data['source_digest'], 'manifest_sha256': sha(path)}))


if __name__ == '__main__':
    if sys.argv[1] == 'start':
        start()
    elif sys.argv[1] in {'run', 'isolated'}:
        sys.exit(run(sys.argv[2], Path(sys.argv[3]), sys.argv[4:], sys.argv[1] == 'isolated'))
    elif sys.argv[1] == 'ci':
        ci_snapshot(*sys.argv[2:])
    elif sys.argv[1] == 'provenance':
        provenance(*sys.argv[2:])
    elif sys.argv[1] == 'manifest':
        manifest()
    else:
        raise ValueError(sys.argv[1])
