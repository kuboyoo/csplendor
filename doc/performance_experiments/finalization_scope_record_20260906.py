#!/usr/bin/env python3
"""Record F0 identities and reuse selected Phase 6 evidence; never build/merge/push.

Run once in the isolated report worktree. Output is exclusive-create, immutable.
Only known source/build files and explicitly selected evidence are read.
"""
import collections
import datetime
import gzip
import hashlib
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / 'doc/performance_experiments'
RAW = DOC / 'raw/finalization_20260906'
MAIN = 'f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc'
CANDIDATE = '9415de5766c356f9229e2bb2d22feb97d2c6b8bb'
SIDE = '595d58805a99ee0c48586d04ad23ab14b459750d'
BASE = '7835f642b23251d0cb91de180006084521c74aa6'
SOURCE_ROOTS = {'src', 'scripts', 'tests', 'csplendor'}
SOURCE_SUFFIXES = {'.h', '.cpp', '.py', '.txt', '.json'}
PACKAGING = {'setup.py', '_build_support.py', 'pyproject.toml', 'setup.cfg',
             'MANIFEST.in', '.github/workflows/ci.yml'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(*args, root=ROOT):
    return subprocess.check_output(['git', *args], cwd=root, text=True).strip()


def source_path(name):
    path = Path(name)
    return name == 'CMakeLists.txt' or (
        path.parts[0] in SOURCE_ROOTS and path.suffix in SOURCE_SUFFIXES)


def source_identity(commit):
    names = git('ls-tree', '-r', '--name-only', commit).splitlines()
    files, packaging = [], []
    for name in sorted(names):
        if source_path(name) or name in PACKAGING:
            data = subprocess.check_output(['git', 'show', f'{commit}:{name}'], cwd=ROOT)
            entry = {'path': name, 'sha256': sha(data)}
            (files if source_path(name) else packaging).append(entry)
    # Same path selection/order/serialization as phase3d1_finalize.source().
    digest = sha(''.join(f"{i['sha256']}  {i['path']}\n" for i in files).encode())
    return {'commit': commit, 'tree': git('rev-parse', f'{commit}^{{tree}}'),
            'source_digest': digest, 'files': files, 'packaging_files': packaging}


def snapshot(root):
    # Do not read arbitrary untracked files; their status/names suffice.
    result = {'head': git('rev-parse', 'HEAD', root=root),
              'status': git('status', '--short', '--branch', root=root),
              'tracked_diff_sha256': sha(subprocess.check_output(
                  ['git', 'diff', '--binary', 'HEAD', '--', 'src', 'csplendor',
                   'scripts', 'tests', 'CMakeLists.txt', *sorted(PACKAGING)], cwd=root))}
    if root.name == 'csplendor':
        request = root / 'doc/csplendor_finalization_request_20260906.md'
        result['user_request_sha256'] = sha(request.read_bytes())
    return result


def remote_refs():
    result = {}
    for line in git('ls-remote', 'origin', 'refs/heads/main',
                    'refs/heads/perf/build-profiles',
                    'refs/heads/codex-mate-frontier-fixes-20260906').splitlines():
        commit, name = line.split()
        result[name] = commit
    assert result == {'refs/heads/main': MAIN, 'refs/heads/perf/build-profiles': CANDIDATE,
                      'refs/heads/codex-mate-frontier-fixes-20260906': SIDE}, result
    return result


def main():
    assert git('branch', '--show-current') == 'docs/finalization-scope-20260906'
    assert git('rev-parse', 'HEAD') == CANDIDATE
    assert not RAW.exists(), 'Do not overwrite existing evidence'
    protected = [ROOT.parent / name for name in ('csplendor', 'csplendor-build-profiles')]
    before = {str(p): snapshot(p) for p in protected}
    local_refs = git('for-each-ref', '--format=%(refname) %(objectname)',
                     'refs/heads/main', 'refs/heads/perf/build-profiles',
                     'refs/heads/codex-mate-frontier-fixes-20260906', 'refs/remotes/origin/main')
    refs = remote_refs()
    identities = {label: source_identity(commit) for label, commit in
                  [('latest_main', MAIN), ('optimization_candidate', CANDIDATE), ('side_branch', SIDE)]}
    assert identities['latest_main']['tree'] == identities['side_branch']['tree']
    assert git('merge-base', MAIN, CANDIDATE) == BASE
    assert git('merge-base', MAIN, SIDE) == SIDE
    assert git('merge-base', CANDIDATE, SIDE) == BASE
    assert git('rev-list', '--left-right', '--count', f'{MAIN}...{CANDIDATE}').split() == ['9', '30']

    evidence_path = DOC / 'phase6_evidence_20260906.json'
    evidence = json.loads(evidence_path.read_text())
    assert identities['optimization_candidate']['source_digest'] == evidence['candidate_source_digest']
    for row in identities['optimization_candidate']['files'] + identities['optimization_candidate']['packaging_files']:
        assert sha((ROOT / row['path']).read_bytes()) == row['sha256'], row['path']
    artifacts = {item['path']: item['sha256'] for item in evidence['artifacts']}
    reused = []
    selected = ['final_source', 'full_native_portable', 'full_native_lto', 'full_python',
                'python_performance', 'python_compile', 'unit_asan', 'unit_tsan', 'unit_oracle',
                'portable_wheel', 'wheel_import', 'prior_artifact_audit', 'lto_final_primary',
                'lto_formal_primary', 'lto_holdout_primary']
    for name in selected:
        path = f'doc/performance_experiments/raw/phase6/{name}.json.gz'
        blob = (ROOT / path).read_bytes()
        assert sha(blob) == artifacts[path], path
        data = json.loads(gzip.decompress(blob))
        assert data.get('exit_code', 0) == 0, name
        if name == 'final_source':
            assert data['source']['source_digest'] == identities['optimization_candidate']['source_digest']
            assert data['source']['files'] == identities['optimization_candidate']['files']
        reused.append({'path': path, 'sha256': sha(blob), 'exit_code': data.get('exit_code'),
                       'command': data.get('command'), 'executed_again': False})

    def changed(a, b):
        return set(git('diff', '--name-only', a, b).splitlines())
    main_files, candidate_files = changed(BASE, MAIN), changed(BASE, CANDIDATE)
    identical, divergent = [], []
    for path in sorted(main_files & candidate_files):
        same = git('rev-parse', f'{MAIN}:{path}') == git('rev-parse', f'{CANDIDATE}:{path}')
        (identical if same else divergent).append(path)
    assert len(identical) == 58 and all(p.startswith('doc/performance_experiments/') for p in identical)
    outstanding = sorted(main_files - set(identical))
    assert len(outstanding) == 21
    histories = {label: git('log', '--reverse', '--format=%H %s', f'{BASE}..{commit}')
                 for label, commit in [('main', MAIN), ('candidate', CANDIDATE), ('side', SIDE)]}
    phase_commits = {
        '0': ['6d36e8a'], '1A': ['c1a65a8'], '2A': ['59b29e2'],
        '2B': ['e7642cc', 'c171cf3', '8cba194', 'b2b23aa'], '3A': ['de1dc76'],
        '3B': ['affb80a'], '3C': ['35048e7'], 'R0': ['86b7473'],
        '3D-P1': ['ff8b1f6'], '3D-P2': ['ef312a1'], '3D-1': ['763a910'],
        '3D-2/3 decisions': ['0f73b38'], '5B-R/4B-1': ['2f567ba'],
        '4C decisions': ['59f5656'], '5D decision': ['9801610'],
        '3E': ['2f2d8c8'], '5E': ['b7ed7ad'], '5A decision': ['1980b54'],
        '5C-B': ['3582909'], '6': [CANDIDATE],
    }
    for phase, commits in phase_commits.items():
        phase_commits[phase] = [git('rev-parse', c) for c in commits]
        for commit in phase_commits[phase]:
            assert git('merge-base', commit, CANDIDATE) == commit
    counts = collections.Counter(Path(p).parts[0] for p in changed(MAIN, CANDIDATE))
    raw = {'schema': 'csplendor.finalization.scope.raw.v1',
           'observed_at_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
           'remote_refs': refs, 'local_refs_unchanged': local_refs,
           'protected_before': before, 'identities': identities, 'histories': histories,
           'phase_commit_ancestry_verified': phase_commits,
           'diff_name_status': {label: git('diff', '--name-status', a, b) for label, a, b in
                                [('base_to_main', BASE, MAIN), ('base_to_candidate', BASE, CANDIDATE),
                                 ('main_to_candidate', MAIN, CANDIDATE)]},
           'duplicate_r0_paths': identical, 'divergent_overlap_paths': divergent,
           'main_changes_missing_or_different_in_candidate': outstanding,
           'main_changes_patch': git('diff', BASE, MAIN, '--', *outstanding),
           'reused_evidence': reused}
    after = {str(p): snapshot(p) for p in protected}
    assert before == after, 'Protected worktree changed; stop and report, never restore'
    assert refs == remote_refs(), 'Remote moved; scope needs a new decision'
    assert local_refs == git('for-each-ref', '--format=%(refname) %(objectname)',
                             'refs/heads/main', 'refs/heads/perf/build-profiles',
                             'refs/heads/codex-mate-frontier-fixes-20260906', 'refs/remotes/origin/main')
    raw['protected_after'] = after
    blob = gzip.compress((json.dumps(raw, sort_keys=True, indent=2) + '\n').encode(), mtime=0)
    RAW.mkdir(parents=True)
    raw_path = RAW / 'scope_inventory.json.gz'
    with raw_path.open('xb') as stream:
        stream.write(blob)
    manifest = {'schema': 'csplendor.finalization.scope.v1', 'status': 'BLOCKED',
                'stop_at': 'F0', 'reason': 'Latest main already contains necessary public API/frontier/compatibility changes absent from the optimization candidate; integration requires approval.',
                'BASE_MAIN_SHA': MAIN, 'CANDIDATE_SHA': CANDIDATE, 'FINAL_CANDIDATE_SHA': None,
                'SIDE_BRANCH_SHA': SIDE, 'COMMON_ANCESTOR_SHA': BASE,
                'report_branch': git('branch', '--show-current'),
                'report_parent_commit': CANDIDATE,
                'identity_note': 'Candidate SHA denotes unchanged engine source, not the later documentation commit. No integrated tree has been tested.',
                'source_digest_algorithm': 'Phase 3D-1/6: SHA256(sorted file SHA256 + two spaces + relative path + LF), CMakeLists.txt and src/scripts/tests/csplendor .h/.cpp/.py/.txt/.json. Extra packaging hashes in raw.',
                'source_digests': {k: v['source_digest'] for k, v in identities.items()},
                'tree_digests_git': {k: v['tree'] for k, v in identities.items()},
                'phase6_source_identity_matches': True,
                'prior_adoption_decisions': 'phase6_common_audit_20260906.md (unchanged; all listed milestone ancestors verified)',
                'main_already_contains_side_branch': True, 'main_tree_equals_side_tree': True,
                'diff_summary': {'main_vs_candidate_files': sum(counts.values()), 'by_top_level': dict(counts),
                                 'base_to_main_files': len(main_files), 'base_to_candidate_files': len(candidate_files),
                                 'duplicate_r0_files': len(identical), 'unintegrated_main_files': len(outstanding)},
                'divergent_overlap_paths': divergent, 'integration_scope': outstanding,
                'integration_contract': 'Keep main semantics including hints BEFORE filtering in BOTH compact and reference paths; do not import only old vector-path hint patch.',
                'excluded_from_tested_candidate': 'All nonduplicate side/main changes; not approved as final shipment exclusions.',
                'reused_tests': evidence['correctness'], 'reused_selected_artifacts_count': len(reused),
                'prior_artifact_audit': {'count': evidence['prior_artifacts_verified'], 'rerun': False},
                'phase6_manifest': {'path': str(evidence_path.relative_to(ROOT)), 'sha256': sha(evidence_path.read_bytes())},
                'F1': {'status': 'NOT_RUN_PENDING_SCOPE_APPROVAL', 'speedup': None, 'ci95': None, 'rss': None,
                       'semantic_comparison': 'not run; API/source divergence statically identified',
                       'separate_axes_on_resume': ['main portable -> integrated candidate portable',
                                                   'same candidate portable -> opt-in LTO (solver only)',
                                                   'main -> LTO deployment direct measurement, never product of phase ratios']},
                'not_executed': ['merge/cherry-pick', 'production code changes', 'new optimization',
                                 'benchmark/build/full test reruns', 'F2/F3/F4', 'push/PR/publication'],
                'failure_classification': {'integration_gap': 'STATICALLY_CONFIRMED_SCOPE_BLOCKER',
                                           'baseline_runtime_failure': 'NOT_EVALUATED',
                                           'candidate_runtime_failure': 'NOT_EVALUATED',
                                           'environment_unavailable': 'Phase 6 limitations reused, no new execution'},
                'prior_limitations': evidence['limitations'], 'next_step': 'Obtain explicit approval to integrate current main in a new isolated integration worktree, then fix candidate identity and rerun affected correctness/F1 gates.',
                'artifacts': [{'path': str(raw_path.relative_to(ROOT)), 'sha256': sha(blob), 'bytes': len(blob)}]}
    out = DOC / 'finalization_scope_manifest_20260906.json'
    with out.open('x') as stream:
        json.dump(manifest, stream, sort_keys=True, indent=2)
        stream.write('\n')
    print(json.dumps({'status': manifest['status'], 'source_digests': manifest['source_digests'],
                      'diff_summary': manifest['diff_summary'], 'raw_sha256': sha(blob)}, indent=2))


if __name__ == '__main__':
    main()
