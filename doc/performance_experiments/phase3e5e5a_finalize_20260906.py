#!/usr/bin/env python3
"""Validate durable evidence and emit the 3E/5E/5A decision manifest."""
import argparse
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase3e5e5a'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def main(output):
    start = read('3e/starting_worktrees')
    assert source(ROOT.parent / 'csplendor') == start['user_workspace']
    assert source(ROOT.parent / 'csplendor-v3-payment-dp') == start['baseline']
    assert source(ROOT.parent / 'csplendor-selection-baseline') == read('5e/baseline')
    assert source(ROOT.parent / 'csplendor-policy-baseline') == read('5a/baseline')
    assert start['baseline']['head'] == '9801610556419bf86ca71530592f643f76dc7ec0'
    # No direct-policy prototype survives: only the two accepted tickets.
    assert not subprocess.check_output(['git', 'diff', read('5a/baseline')['head'],
                                        '--', 'src', 'CMakeLists.txt'], cwd=ROOT)
    assert 'CSPLENDOR_DIRECT_POLICY_APPLY' not in (ROOT / 'CMakeLists.txt').read_text()
    for profile in ('final-release', 'final-reference', 'final-diagnostic', 'final-asan'):
        assert read('final/unit_' + profile)['exit_code'] == 0
    for name in ('unit_final_asan_selection', 'native_full', 'python_full_fixed',
                 'python_performance', 'python_compile', 'orchestration_compile'):
        assert read('final/' + name)['exit_code'] == 0, name
    for profile in ('reference', 'diagnostic'):
        assert read('5a/unit_5a-' + profile)['exit_code'] == 0
    identity = read('final/deployment_binary_identity')
    final_sha = sha((ROOT / 'build/final-release/benchmark_engine_hotpaths').read_bytes())
    assert identity['identical'] and identity['after'] == final_sha

    known_failures = {
        'final/perf_environment': 'ENVIRONMENT_UNAVAILABLE: perf_event_paranoid=4; no permission changes',
        '5e/build_5e-release': 'FIXED_TEST_BUILD: initializer_list uint16/int mismatch',
        '5a/build_5a-release': 'FIXED_TEST_BUILD: pop_back returns void; use back then pop',
        '5a/unit_fixed': 'FIXED_ORACLE_PRECONDITION: historical scan helper -1 matches non-policy moves; constrain oracle to 0..47, additive prototype rejects invalid indices',
        'final/python_full': 'FIXED_CONTRACT_METADATA: register solver_take_groups.h as internal; 571 other tests passed',
    }
    measurements, failures, diagnostics = {}, [], {}
    for path in sorted(RAW.rglob('*.json.gz')):
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        data = json.loads(gzip.decompress(path.read_bytes()))
        if data.get('exit_code', 0):
            assert name in known_failures, (name, data.get('stderr'))
            failures.append({'raw': str(path.relative_to(ROOT)), 'classification': known_failures[name]})
        if '/profile_' in name and 'stdout' in data:
            record = json.loads(data['stdout'])
            assert record['semantics']['correct'], name
            diagnostics[name] = record
        if 'comparison' not in data:
            continue
        row = data['comparison'][0]
        samples = [p[s]['records'][0] for p in data['pairs'] for s in ('A', 'B')]
        assert all(r['semantics']['correct'] for r in samples), name
        assert len({r['digest'] for r in samples}) == 1, name
        assert all(c['identical_in_every_pair'] for c in row['counters'].values()
                   if c['classification'] == 'correctness'), name
        ratio = row['B_over_A']
        assert len(data['pairs']) == (4 if '/smoke_' in name else 22), name
        measurements[name] = {
            'raw': str(path.relative_to(ROOT)), 'workload': row['workload'], 'fixture': row['fixture'],
            'contract': 'S0', 'ratio': ratio['median'], 'ci95': ratio['crossover_block_bootstrap_ci95'],
            'pairs': len(data['pairs']), 'blocks': ratio['crossover_blocks'], 'digest': samples[0]['digest'],
            'absolute_medians': {k: v['median'] for k, v in row['absolute'].items()},
            'binary_sha256': {s: data['manifests'][s]['binary']['sha256'] for s in ('A', 'B')},
            'baseline_git': data['manifests']['A']['git'], 'candidate_git': data['manifests']['B']['git'],
            'settings': data['settings'],
        }
    for ticket, threshold in (('3e_v2', 1.05), ('5e', 1.03)):
        for stage in ('formal', 'holdout'):
            row = measurements[ticket + '/' + stage + '_primary']
            assert row['ratio'] >= threshold and row['ci95'][0] > 1
    for stage in ('formal', 'holdout'):
        row = measurements['5a/' + stage + '_primary']
        assert row['ratio'] < 1.03, '5A rejection reason changed'
    for name in ('common_visible', 'common_random', 'common_mcts', 'common_decode', 'common_v3'):
        row = measurements['final/' + name]
        assert row['binary_sha256']['B'] == final_sha
    for ticket, names in {
        '3e_v2': ('returns', 'cycle', 'solver', 'mcts'),
        '5e': ('simple', 'count', 'codes', 'editor', 'solver', 'mcts'),
    }.items():
        for name in names:
            row = measurements[ticket + '/formal_' + name]
            assert row['ratio'] >= 0.98 or row['ci95'][1] >= 1, (ticket, name)
    matched = measurements['final/matched_decode_long']
    assert matched['ratio'] >= 0.98 and matched['ci95'][1] >= 1
    candidate = source(ROOT)
    snapshot = {'start': start, 'candidate': candidate,
                '5e_baseline': read('5e/baseline'), '5a_baseline': read('5a/baseline')}
    snapshot_path = RAW / 'final/source_snapshot.json.gz'
    if snapshot_path.exists():
        assert json.loads(gzip.decompress(snapshot_path.read_bytes()))['candidate']['source_digest'] == candidate['source_digest']
    else:
        with snapshot_path.open('xb') as stream:
            stream.write(gzip.compress((json.dumps(snapshot, indent=2, sort_keys=True) + '\n').encode(), mtime=0))
    manifest = {
        'schema': 'csplendor.phase3e5e5a.evidence.v1',
        'decisions': {'3E': 'ADOPT_V2', '5E': 'ADOPT', '5A': 'REJECT_AND_REVERT'},
        'baseline_commit': start['baseline']['head'], 'baseline_source_digest': start['baseline']['source_digest'],
        'candidate_parent_commit': candidate['head'], 'candidate_source_digest': candidate['source_digest'],
        'candidate_binary_sha256': final_sha, 'source_file_manifest': str(snapshot_path.relative_to(ROOT)),
        'source_digest_algorithm': 'sha256 of sorted file sha256 + two spaces + relative path + LF, safe source extensions',
        'branch': 'perf/action-selection', 'original_dirty_workspace_preserved': True,
        'scope': 'visible-only representative grouping; full-action count/rank random selection; native48 apply prototype rejected',
        'contracts': ['capped full-action order', 'minimum ActionOrderKey', 'full delta equality, not hash-only',
                      'child-key final grouping', 'editor fallback', 'RNG modulo', '48/V3 schemas unchanged',
                      'PASS outside policy', '3B sidecar / 3C TT / rollback / cache / reference paths unchanged',
                      'float order unchanged; no new worker concurrency'],
        'runtime_paths': {'3E': 'VisibleOnlySolver::representative_actions',
                          '5E': 'Game legal_action_code_at/apply_legal_action_index/apply_random_action -> MoveGenerator select_all_capped',
                          '48': 'Existing decode_trusted + Game::apply_trusted; direct source trial reverted',
                          'NN': 'not loaded; no NN speed/strength claim'},
        'preserved_decisions': {'4A-1/2': 'DEFERRED', '5B-R': 'REJECTED', '4B-1': 'ADOPTED', '4C-1/2/3': 'REJECTED', '5D': 'REJECTED'},
        'prototypes': {'3E_v1': 'doc/performance_experiments/raw/phase3e5e5a/3e/prototype_v1.json.gz',
                       '3E_v2': 'doc/performance_experiments/raw/phase3e5e5a/3e/accepted_v2.json.gz',
                       '5E': 'doc/performance_experiments/raw/phase3e5e5a/5e/accepted.json.gz',
                       '5A': 'doc/performance_experiments/raw/phase3e5e5a/5a/rejected.json.gz'},
        'hardware_perf': 'N/A (paranoid=4)', 'tsan': 'N/A: concurrency code unchanged',
        'measurement_limitations': 'Common-baseline decode and V3 guards were noisy (all CIs include 1); matched-harness 2M decode ratio 0.9867, CI [0.9793,1.0061]. No blanket micro speedup or strict 2% noninferiority claim.',
        'failures': failures, 'measurements': measurements, 'diagnostics': diagnostics,
        'test_logs': {name: str((RAW / 'final' / (name + '.json.gz')).relative_to(ROOT))
                      for name in ('native_full', 'python_full_fixed', 'python_performance', 'unit_final-asan', 'unit_final_asan_selection')},
        'artifacts': [{'path': str(p.relative_to(ROOT)), 'bytes': p.stat().st_size, 'sha256': sha(p.read_bytes())}
                      for p in sorted(RAW.rglob('*.gz'))],
    }
    with output.open('x') as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print('verified manifest:', output, 'source:', candidate['source_digest'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args().output)
