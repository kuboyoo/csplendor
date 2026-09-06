#!/usr/bin/env python3
"""Append-only compatibility evidence; isolated builds, no deployment."""
import ast
import difflib
import gzip
import hashlib
import json
from pathlib import Path
import sys
import zipfile

import f4_premerge_record_20260906 as prior

ROOT = prior.ROOT
RAW = ROOT / 'doc/performance_experiments/raw/f4_compat_20260906'
BASE = 'fa4ea6de2f81f17acf5d83bfa0f8f6c4e35670ec'
CODE = 'c725b5ed33e5ba4cd4cc99d9ccee8627550b754e'
ISOLATED = ROOT.parent.parent / 'csplendor-f4-compat-20260906'
WHEEL_SOURCE = ROOT.parent / 'csplendor-f4-compat-wheel-20260906'
prior.RAW = RAW
prior.prior.RAW = RAW
prior.ISOLATED = ISOLATED
save = prior.prior.save
sha = prior.prior.sha
git = prior.prior.git
source = prior.prior.source
run = prior.run


def start():
    assert git(ROOT, 'rev-parse', 'HEAD') == BASE
    evidence = {}
    for name in ['final_main_vs_candidate_manifest_20260906.json',
                 'f2_f3_manifest_20260906.json', 'f3_lint_manifest_20260906.json',
                 'f4_premerge_manifest_20260906.json']:
        path = ROOT / 'doc/performance_experiments' / name
        data = json.loads(path.read_text())
        items = data['artifacts'] + ([data['csv']] if 'csv' in data else [])
        for item in items:
            assert sha(ROOT / item['path']) == item['sha256'], item['path']
        evidence[name] = {'sha256': sha(path), 'artifacts_verified': len(items)}
        print(name, list(data), flush=True)
    save('start', {'candidate': source(ROOT), 'main': git(ROOT, 'rev-parse', 'origin/main'),
                   'local_main': git(ROOT, 'rev-parse', 'main'),
                   'original': source(ROOT.parent / 'csplendor'), 'evidence': evidence})
    print('PASS: prior artifact hashes verified; original worktree recorded')


def read_raw(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def provenance():
    initial = read_raw('start')
    current = source(ROOT)
    clean = source(WHEEL_SOURCE)
    assert current['source_digest'] == clean['source_digest']
    assert git(WHEEL_SOURCE, 'rev-parse', 'HEAD') == CODE
    assert git(WHEEL_SOURCE, 'status', '--porcelain') == ''
    assert source(ROOT.parent / 'csplendor') == initial['original']
    assert git(ROOT, 'rev-parse', 'main') == initial['local_main']
    assert git(ROOT, 'diff', prior.prior.ENGINE, '--', 'csplendor', 'CMakeLists.txt',
               'setup.py', '_build_support.py', 'pyproject.toml', '.github') == ''
    old_header = git(ROOT, 'show', BASE + ':src/noble_data.h')
    new_header = (ROOT / 'src/noble_data.h').read_text()
    prefix = old_header.split('constexpr bool noble_mask_table_is_exact()')[0]
    assert prefix == new_header.split('constexpr bool noble_mask_entries_are_exact()')[0]
    unchanged_functions = {}
    for filename in ['run_paired_benchmarks.py', 'run_phase0_baseline.py']:
        name = 'scripts/' + filename
        def definitions(text):
            return {n.name: ast.dump(n, include_attributes=False) for n in ast.parse(text).body
                    if isinstance(n, ast.FunctionDef)}
        old = definitions(git(ROOT, 'show', BASE + ':' + name))
        new = definitions((ROOT / name).read_text())
        expected = {'_native_binary_identity'} if 'paired' in filename else {'main'}
        assert set(old) == set(new)
        assert {k for k in old if old[k] != new[k]} == expected
        unchanged_functions[name] = sorted(k for k in old if old[k] == new[k])
    acceptance = json.loads(read_raw('wheel_acceptance')['stdout'])
    assert acceptance['status'] == 'PASS'
    ident = acceptance['identity']
    for key in ['wheel', 'extension']:
        assert sha(ident[key]) == ident[key + '_sha256']
    old_manifest = json.loads((ROOT / 'doc/performance_experiments/f4_premerge_manifest_20260906.json').read_text())
    old_raw = json.loads(gzip.decompress((prior.ROOT / 'doc/performance_experiments/raw/f4_premerge_20260906/wheel_provenance.json.gz').read_bytes()))
    old_ident = old_raw['acceptance']['identity']
    assert ident['extension_sha256'] == old_ident['extension_sha256']
    assert ident['installed_files'] == old_ident['installed_files']
    with zipfile.ZipFile(old_ident['wheel']) as old, zipfile.ZipFile(ident['wheel']) as new:
        assert old.namelist() == new.namelist()
        wheel_member_diffs = [n for n in old.namelist() if old.read(n) != new.read(n)]
    build = WHEEL_SOURCE / 'build/temp.linux-x86_64-cpython-312/portable/default/release'
    cache = dict(line.split('=', 1) for line in (build / 'CMakeCache.txt').read_text().splitlines()
                 if line.startswith(('CSPLENDOR_', 'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:',
                                     'CMAKE_BUILD_TYPE:', 'CMAKE_CXX_COMPILER:')))
    assert cache == old_raw['cache']
    flags = {str(p.relative_to(build)): p.read_text() for p in
             [build / 'CMakeFiles/_csplendor.dir/flags.make', build / 'CMakeFiles/_csplendor.dir/link.txt']}
    # Include/output roots legitimately differ in clean worktrees/venvs.
    old_source = str(prior.ROOT.parent / 'csplendor-f4-wheel-20260906')
    old_env = str(prior.ROOT.parent.parent / 'csplendor-f4-acceptance-20260906')
    for name, value in flags.items():
        expected = old_raw['flags_and_link'][name].replace(old_source, str(WHEEL_SOURCE)).replace(old_env, str(ISOLATED))
        assert value == expected, name
    paired = json.loads(read_raw('paired_real_elf_guard')['stdout'])
    assert len(paired['pairs']) == 4
    slots = {}
    for i, pair in enumerate(paired['pairs']):
        assert pair['order'] == (['A', 'B'] if i % 2 == 0 else ['B', 'A'])
        for side in ['A', 'B']:
            slot = pair[side]['binary_slot']
            slots.setdefault(slot['slot_id'], set()).add((slot['slot_device'], slot['slot_inode']))
            assert slot['source_binary_sha256'] == slot['staged_binary_sha256'] == slot['post_run_sha256']
            assert not Path(slot['slot_path']).exists()
    assert len(slots) == 2 and all(len(s) == 1 for s in slots.values())
    save('provenance', {'code_commit': CODE, 'source': current, 'clean_source': clean,
        'old_source_digest': old_manifest['candidate_source_digest'],
        'noble_data_table_runtime_prefix_sha256': hashlib.sha256(prefix.encode()).hexdigest(),
        'data_table_runtime_prefix_unchanged': True, 'cache': cache, 'flags_and_link': flags,
        'build_versions': old_manifest['build_versions'], 'wheel_member_diffs': wheel_member_diffs,
        'F1_F2_extension_byte_identical': True, 'all_29_installed_py_so_identical': True,
        'unchanged_driver_top_level_function_ASTs': unchanged_functions,
        'acceptance': acceptance, 'real_ELF_AA_guard': paired['comparison'],
        'user_worktree_and_local_main_unchanged': True})
    print(json.dumps({'source_digest': current['source_digest'], 'wheel_member_diffs': wheel_member_diffs,
                      'identity': ident, 'real_ELF_AA_guard': paired['comparison']}, indent=2))


def ci_snapshot(run_id):
    run('ci_fixed', ROOT, ['gh', 'run', 'view', run_id, '--repo', 'kuboyoo/csplendor',
                           '--json', 'databaseId,url,headSha,event,status,conclusion,jobs'])
    data = json.loads(read_raw('ci_fixed')['stdout'])
    assert data['headSha'] == CODE and data['status'] == 'completed'
    for job in data['jobs']:
        if job['steps']:
            name = 'ci_job_' + str(job['databaseId'])
            assert run(name, ROOT, ['gh', 'api', 'repos/kuboyoo/csplendor/actions/jobs/' +
                       str(job['databaseId']) + '/logs']) == 0
    run('pr_fixed', ROOT, ['gh', 'api', 'repos/kuboyoo/csplendor/pulls/26', '--jq',
        '{head:.head.sha,base:.base.sha,test_merge:.merge_commit_sha,merged,auto_merge}'])


def metadata_difference():
    old_path = ROOT.parent.parent / 'csplendor-f4-acceptance-20260906/wheels/csplendor-0.1.0-cp312-cp312-linux_x86_64.whl'
    new_path = ISOLATED / 'wheels/csplendor-0.1.0-cp312-cp312-linux_x86_64.whl'
    member = 'csplendor-0.1.0.dist-info/METADATA'
    with zipfile.ZipFile(old_path) as old, zipfile.ZipFile(new_path) as new:
        before, after = (archive.read(member).decode() for archive in (old, new))
    # Distribution headers (including dependency declarations) are identical;
    # only the embedded README body reflects the prior F4 documentation commit.
    assert before.split('\n\n', 1)[0] == after.split('\n\n', 1)[0]
    difference = '\n'.join(difflib.unified_diff(before.splitlines(), after.splitlines(), n=0))
    save('wheel_metadata_difference', {'headers_and_dependencies_identical': True,
         'difference': difference, 'reason': 'f6 -> fa4 README-only F4 progress, included in wheel METADATA'})
    print(difference)


def manifest():
    provenance_data = read_raw('provenance')
    ci = json.loads(read_raw('ci_fixed')['stdout'])
    pr = json.loads(read_raw('pr_fixed')['stdout'])
    assert pr['head'] == CODE and pr['base'] == read_raw('start')['main']
    assert not pr['merged'] and pr['auto_merge'] is None
    assert git(ROOT, 'diff', CODE, '--', 'src', 'csplendor', 'scripts', 'tests',
               'CMakeLists.txt', 'setup.py', '_build_support.py', 'pyproject.toml', '.github') == ''
    counts = {}
    checkout_verified = []
    errors = {}
    summaries = {}
    for job in ci['jobs']:
        counts[job['conclusion']] = counts.get(job['conclusion'], 0) + 1
        if not job['steps']:
            continue
        log = read_raw('ci_job_' + str(job['databaseId']))['stdout']
        assert pr['test_merge'] in log and CODE in log and pr['base'] in log, job['name']
        checkout_verified.append(job['name'])
        errors[job['name']] = [s for s in log.splitlines() if any(
            marker in s for marker in ['error:', 'error C', 'warning C4244', 'warning C4267',
                                      'FAILED tests/', 'illegal option -- f'])]
        summaries[job['name']] = [s for s in log.splitlines()
             if ' passed' in s or 'tests passed' in s or 'Total coverage:' in s]
    artifacts = [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p), 'bytes': p.stat().st_size}
                 for p in sorted(RAW.glob('*.json.gz'))]
    data = {'schema': 'csplendor.f4_compatibility.v1', 'decision': 'BLOCKED',
            'starting_head': BASE, 'fixed_code_head': CODE, 'F1_code': prior.prior.ENGINE,
            'origin_main': pr['base'], 'fixed_test_merge': pr['test_merge'],
            'branch': git(ROOT, 'branch', '--show-current'),
            'source_digest': provenance_data['source']['source_digest'],
            'fix_commits': git(ROOT, 'log', '--format=%H %s', BASE + '..' + CODE).splitlines(),
            'production_diff_from_F1': ['src/noble_data.h'],
            'measurement_diff_from_F1': ['scripts/run_phase0_baseline.py', 'scripts/run_paired_benchmarks.py'],
            'build_dependency_CI_unchanged': True,
            'wheel_acceptance': provenance_data['acceptance'],
            'build_tools': json.loads(read_raw('build_tools_versions')['stdout']),
            'table_runtime_prefix_unchanged': True,
            'F1_F2_binary_byte_identity': True,
            'performance_guard': 'No new engine timing: installed py/so byte-identical; actual Linux ELF A/A runner guard added, not a speedup measurement',
            'CI_fixed_code': ci, 'CI_counts': counts, 'CI_errors': errors, 'CI_summaries': summaries,
            'checkout_head_base_test_merge_verified_for': checkout_verified,
            'new_blockers': ['solver_path.h:58 unused-but-set-variable',
                             'solver_action_filter.h:50,64 unused-lambda-capture',
                             'macOS GNU time -f/-o assumption in existing timeout test',
                             'MSVC C4244/C4267 in state_feature_table_unit/action_selection_unit'],
            'not_executed': ['sanitizer runtime (build failed)', 'macOS native/wheel (pytest blocked)',
                             'Windows native execution/wheel (native build failed)',
                             'nightly soak (PR-inapplicable)', 'new real-model/GPU/browser acceptance',
                             'merge/deployment/publication'],
            'final_CI_policy': 'Verify record-only final head separately; store final result in PR body, no recursive evidence commits',
            'artifacts': artifacts}
    output = ROOT / 'doc/performance_experiments/f4_compat_manifest_20260906.json'
    with output.open('x') as stream:
        json.dump(data, stream, ensure_ascii=False, sort_keys=True, indent=2)
        stream.write('\n')
    print(json.dumps({'counts': counts, 'PR': pr, 'summaries': summaries}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    if sys.argv[1] == 'start':
        start()
    elif sys.argv[1] in {'run', 'isolated'}:
        sys.exit(run(sys.argv[2], Path(sys.argv[3]), sys.argv[4:], sys.argv[1] == 'isolated'))
    elif sys.argv[1] == 'provenance':
        provenance()
    elif sys.argv[1] == 'ci':
        ci_snapshot(sys.argv[2])
    elif sys.argv[1] == 'metadata':
        metadata_difference()
    elif sys.argv[1] == 'manifest':
        manifest()
    else:
        raise ValueError(sys.argv[1])
