#!/usr/bin/env python3
"""Check gates/provenance and print a compact manifest for durable recording."""
import gzip
import hashlib
import json
import statistics
from pathlib import Path
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase5br4b1'
USER_ROOT = ROOT.parent / 'csplendor'
BASE_SOURCE = ROOT.parent / 'csplendor-reveal-transactions'
BASE_COMMIT = '0f73b38241eaa54497d85ac0493e10acc4332f26'
BASE_DIGEST = '0a7f6d34f56e1d9738b52cdaaec03b1eebd096188a2dd629f8bd89d0a75b7e33'


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    starting = read('5br/v1/starting_worktrees')
    baseline = source(BASE_SOURCE)
    candidate = source(ROOT)
    assert baseline == starting['baseline']
    assert source(USER_ROOT) == starting['user_workspace']
    assert baseline['head'] == BASE_COMMIT and baseline['source_digest'] == BASE_DIGEST
    prefix = '4b1/v1/'
    required = ['native_full', 'final_native_full', 'python_full', 'python_performance', 'python_compile',
                'unit_asan', 'unit_tsan']
    required += ['unit_' + label for label in ('release', 'diagnostic', 'reference-diagnostic', 'verify')]
    required += ['final_alias_unit_' + label for label in ('release', 'diagnostic', 'reference-diagnostic', 'verify', 'asan', 'tsan')]
    for name in required:
        assert read(prefix + name)['exit_code'] == 0, name
    assert read(prefix + 'deployment_binary_identity')['identical']
    assert sha((ROOT / 'build/4b1-release/benchmark_engine_hotpaths').read_bytes()) == read(prefix + 'deployment_binary_identity')['after']
    measurements = {}
    failures = []
    for path in sorted(RAW.rglob('*.json.gz')):
        raw = json.loads(gzip.decompress(path.read_bytes()))
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        if raw.get('exit_code', 0):
            failures.append({'raw': str(path.relative_to(ROOT)), 'exit_code': raw['exit_code'],
                             'classification': ('environment_unavailable' if 'perf_environment' in name
                                                else 'candidate_test_build_failure_fixed' if name == '5br/v1/build_release'
                                                else 'measurement_harness_failure')})
        if 'comparison' not in raw:
            continue
        row = raw['comparison'][0]
        ratios = row['B_over_A']
        samples = [pair[side]['records'][0] for pair in raw['pairs'] for side in ('A', 'B')]
        asynchronous = any(r['semantics'].get('threads', 1) > 1 for r in samples)
        assert all(r['semantics']['correct'] for r in samples)
        if not asynchronous:
            assert len({r['digest'] for r in samples}) == 1, name
        selected = 'smoke' not in name
        if selected:
            assert len(raw['pairs']) == 22 and ratios['crossover_blocks'] == 11, name
        measurements[name] = {
            'raw': str(path.relative_to(ROOT)), 'role': 'formal_or_holdout' if selected else 'screening_only',
            'speed_ratio': ratios['median'], 'ci95': ratios['crossover_block_bootstrap_ci95'],
            'pairs': len(raw['pairs']), 'statistical_unit': ratios['bootstrap_unit'],
            'operations': samples[0]['operations'], 'workload': row['workload'], 'fixture': row['fixture'],
            'cpu_set': raw['settings']['cpu_set'], 'semantic_contract': 'S1' if asynchronous else 'S0',
            'absolute_medians': {key: value['median'] for key, value in row['absolute'].items()},
        }
        if asynchronous:
            ledger = []
            for side in ('A', 'B'):
                runs = [pair[side]['records'][0] for pair in raw['pairs']]
                for r in runs:
                    c, s = r['counters'], r['semantics']
                    assert s['ledger_integrity_ok'] and s['virtual_loss_balanced']
                    assert c['issued'] == c['completed'] + c['cancelled'] + c['failed']
                    assert c['completed'] == r['operations']
                    assert c['virtual_loss_added'] == c['virtual_loss_released']
                    assert c['selected'] == c['reservations_committed'] + c['reservations_aborted']
                ledger.append({'side': side, 'median_per_second': {
                    key: statistics.median(r['counters'][key] * 1e9 / r['elapsed_ns'] for r in runs)
                    for key in ('completed', 'evaluated_boards', 'selected', 'expansion_waited')},
                    'stop_reasons': sorted({r['semantics']['stop_reason'] for r in runs}),
                    'root_distribution_and_tree_digest_exact_equality_required': False})
            measurements[name]['throughput_ledger'] = ledger
    for stage in ('formal', 'holdout'):
        five = measurements['5br/v1/' + stage + '_primary']
        four = measurements[prefix + stage + '_primary']
        assert five['speed_ratio'] < 1.03
        assert four['speed_ratio'] > 1.05 and four['ci95'][0] > 1.0
    production_patch = __import__('subprocess').check_output(
        ['git', 'diff', '--', 'src', 'CMakeLists.txt'], cwd=ROOT).decode()
    assert 'mcts_game_scratch' not in production_patch and 'REUSE_GAME_SCRATCH' not in production_patch
    assert not (ROOT / 'src/mcts_game_scratch.h').exists()
    snapshot = {'baseline': baseline, 'candidate': candidate,
                'user_workspace_unchanged': True, 'user_workspace_start': starting['user_workspace']}
    snapshot_path = RAW / '4b1/v1/final_source_snapshot_v2.json.gz'
    if snapshot_path.exists():
        previous = json.loads(gzip.decompress(snapshot_path.read_bytes()))
        assert previous['candidate']['source_digest'] == candidate['source_digest']
        assert previous['baseline'] == baseline
    else:
        with snapshot_path.open('xb') as stream:
            stream.write(gzip.compress((json.dumps(snapshot, indent=2, sort_keys=True) + '\n').encode(), mtime=0))
    artifacts = []
    for path in sorted(RAW.rglob('*.gz')):
        data = path.read_bytes()
        artifacts.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(data), 'bytes': len(data)})
    manifest = {
        'schema': 'csplendor.phase5br4b1.evidence.v1',
        'baseline_commit': BASE_COMMIT, 'baseline_source_digest': BASE_DIGEST,
        'candidate_parent_commit': candidate['head'], 'candidate_source_digest': candidate['source_digest'],
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF; src/scripts/tests/csplendor safe source extensions plus CMakeLists.txt',
        'source_file_manifest': str(snapshot_path.relative_to(ROOT)),
        'branch': 'perf/mcts-state-records', 'original_dirty_worktree_preserved': True,
        'decisions': {'5B-R': 'REJECT_AND_REVERT', '4B-1': 'ADOPT', '4A-1': 'DEFERRED_BY_USER', '4A-2': 'DEFERRED_BY_USER'},
        'reference_build_option': 'CSPLENDOR_MCTS_LEGACY_TREE_RECORDS=OFF',
        'runtime_scope': 'native legacy 48-action MCTS with synthetic evaluator; no live NN/V3/Python Genbu speedup claim',
        'binary_sha256': {label: sha(path.read_bytes()) for label, path in {
            'baseline': BASE_SOURCE / 'build/3d23-final-release/benchmark_engine_hotpaths',
            'candidate': ROOT / 'build/4b1-release/benchmark_engine_hotpaths',
            'rejected_5br': ROOT / 'build/5br-release/benchmark_engine_hotpaths',
        }.items()},
        'validation': {'native_full': '39/39', 'python': '565 passed, 1 skipped, 4 deselected',
            'python_performance': '4 passed', 'py_compile': 'PASS', 'asan_ubsan': '8/8 MCTS suites',
            'tsan': '3/3 MCTS suites', 'release_perf_reference_verify': '6 suites each; 5 representative digest comparisons',
            'hardware_perf': 'N/A: perf_event_paranoid=4; no permission changes',
            '5br_full_python_and_sanitizers': 'NOT_RUN: failed performance gate before adoption'},
        'memory_tradeoff': {'primary_peak_rss_median_kib': [34794, 33746],
            'unexpanded_node_unit_peak_rss_single_run_kib': [49356, 58560],
            'reason': 'inline aux replaces lazy auxiliary map creation; unexpanded-node-heavy API usage retains more memory'},
        'limitations': ['No proof of <2% equivalence on noisy asynchronous 4T guard',
            'Prune session includes 49000 synthetic expanded historical nodes, not a naturally collected history',
            'Allocation failure positions change; access-only orphan entries from the old multi-map OOM path are not reproduced',
            'No multiplication with historical phase gains; no 5B-R production change retained'],
        'measurements': measurements, 'nonzero_invocations': failures, 'artifacts': artifacts,
    }
    print(json.dumps(manifest, indent=2, ensure_ascii=False, sort_keys=True))


if __name__ == '__main__':
    main()
