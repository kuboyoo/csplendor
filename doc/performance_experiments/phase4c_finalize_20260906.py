#!/usr/bin/env python3
"""Audit 4C gates, preserved sources and compressed records; emit evidence JSON."""
import gzip
import hashlib
import json
import statistics
from pathlib import Path
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase4c'
BASE = ROOT.parent / 'csplendor-mcts-concurrency-baseline'
PRIOR = ROOT.parent / 'csplendor-mcts-state-records'
USER = ROOT.parent / 'csplendor'
BASE_COMMIT = '0fa867d64a6ab98ce1b7e77c1e5e9745ef9c7a57'
BASE_DIGEST = '99cabb84383dae5d757a6ff049a1c8541e751b32cf86f828f12d3fd0f1235323'
VARIANTS = ('4c1/v1', '4c2/v1', '4c3/v2', '4c3/v3')


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ledger(r):
    c, s = r['counters'], r['semantics']
    assert s['correct'] and s['completed_exact'] and not s['partial']
    assert s['ledger_integrity_ok'] and s['virtual_loss_balanced']
    assert c['issued'] == c['completed'] + c['cancelled'] + c['failed']
    assert c['completed'] == r['operations']
    assert c['virtual_loss_added'] == c['virtual_loss_released']
    assert c['selected'] == c['reservations_committed'] + c['reservations_aborted']
    assert c['evaluation_owners'] == c['evaluated_boards'] == c['expansion_published']
    assert all(c[k] == 0 for k in ('stale_result', 'duplicate_result', 'invalid_replay', 'integrity_errors', 'failed'))


def main(output=None):
    starting = read('diagnostic/v1/starting_sources')
    baseline, candidate = source(BASE), source(ROOT)
    assert baseline == read('4c1/v1/starting_sources')['baseline']
    assert source(PRIOR) == starting['baseline']
    assert source(USER) == starting['user_workspace']
    assert baseline['head'] == BASE_COMMIT
    assert baseline['source_digest'] == candidate['source_digest'] == BASE_DIGEST
    for name in ('STRIPED_LEDGER_METRICS', 'INLINE_RESERVATIONS', 'FUSED_NODE_SELECTION'):
        assert name not in (ROOT / 'CMakeLists.txt').read_text()
    assert not any((ROOT / ('tests/' + name + '_unit.cpp')).exists() for name in ('4c1', '4c2', '4c3'))

    required = ['native_full', 'python_full', 'python_performance', 'python_compile',
                'unit_asan-perf', 'unit_tsan-perf', 'unit_release', 'unit_diagnostic',
                'orchestration_compile']
    for name in required:
        assert read('final/v1/' + name)['exit_code'] == 0, name
    for variant in VARIANTS:
        for profile in ('release', 'diagnostic', 'reference'):
            assert read(variant + '/unit_' + profile)['exit_code'] == 0
    binary_identity = read('final/v1/reverted_source_and_binary_identity')
    assert binary_identity['all_binary_identical']
    deployment_identity = read('final/v1/deployment_binary_identity')
    assert deployment_identity['identical']
    final_binary = sha((ROOT / 'build/final-release/benchmark_engine_hotpaths').read_bytes())
    assert final_binary == deployment_identity['after'] == binary_identity['binary_sha256']['baseline']

    measurements, diagnostics, failures = {}, {}, []
    failure_classes = {
        'diagnostic/v1/perf_environment': 'environment_unavailable_perf_event_paranoid_4',
        '4c3/v1/unit_release': 'invalid_new_test_setup_fixed_in_v2_not_engine_failure',
    }
    for path in sorted(RAW.rglob('*.json.gz')):
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        data = json.loads(gzip.decompress(path.read_bytes()))
        if data.get('exit_code', 0):
            assert name in failure_classes, name
            failures.append({'raw': str(path.relative_to(ROOT)), 'exit_code': data['exit_code'],
                             'classification': failure_classes[name]})
        if '/profile_' in name:
            r = json.loads(data['stdout'])
            ledger(r)
            assert r['semantics']['ledger_instrumentation_totals_match']
            keys = [k for k in r['counters'] if k.startswith(('parallel_', 'global_'))]
            diagnostics[name] = {'raw': str(path.relative_to(ROOT)), 'operations': r['operations'],
                'selected': r['counters']['selected'], 'current_rss_kib': r['rss_kib'],
                'observer_effect': True, 'counters': {k: r['counters'][k] for k in keys}}
        if 'comparison' not in data:
            continue
        row = data['comparison'][0]
        ratios = row['B_over_A']
        screening = '/smoke_' in name
        assert len(data['pairs']) == (4 if screening else 22)
        assert ratios['crossover_blocks'] == (2 if screening else 11)
        samples = [p[side]['records'][0] for p in data['pairs'] for side in ('A', 'B')]
        asynchronous = samples[0]['semantics']['threads'] > 1
        for r in samples:
            ledger(r)
        if not asynchronous:
            assert len({r['digest'] for r in samples}) == 1, name
            assert all(c['identical_in_every_pair'] for c in row['counters'].values()
                       if c['classification'] == 'correctness'), name
        metrics = {}
        for side in ('A', 'B'):
            runs = [p[side]['records'][0] for p in data['pairs']]
            metrics[side] = {
                'median_rates_per_second': {k: statistics.median(r['counters'][k] * 1e9 / r['elapsed_ns'] for r in runs)
                    for k in ('completed', 'evaluated_boards', 'selected', 'evaluation_owners', 'inference_waiters')},
                'median_counts': {k: statistics.median(r['counters'][k] for r in runs)
                    for k in ('completed', 'evaluated_boards', 'selected', 'evaluation_owners', 'inference_waiters', 'tree_size')},
                'median_owner_waiter_ratio': statistics.median(r['counters']['evaluation_owners'] / r['counters']['inference_waiters']
                    for r in runs) if all(r['counters']['inference_waiters'] for r in runs) else None,
                'stop_reasons': sorted({r['semantics']['stop_reason'] for r in runs}),
            }
        measurements[name] = {'raw': str(path.relative_to(ROOT)), 'screening_only': screening,
            'speed_ratio': ratios['median'], 'ci95': ratios['crossover_block_bootstrap_ci95'],
            'pairs': len(data['pairs']), 'statistical_unit': ratios['bootstrap_unit'],
            'workload': row['workload'], 'fixture': row['fixture'],
            'cpu_set': data['settings']['cpu_set'], 'contract': 'S1' if asynchronous else 'S0',
            'absolute_medians': {k: v['median'] for k, v in row['absolute'].items()},
            'runtime_ledger': metrics,
            'binary_sha256': {s: data['manifests'][s]['binary']['sha256'] for s in ('A', 'B')},
            'asynchronous_root_tree_digest_equality_required': False,
        }
        variant = '/'.join(name.split('/')[:2])
        if variant in VARIANTS:
            assert measurements[name]['binary_sha256']['A'] == final_binary
            assert measurements[name]['binary_sha256']['B'] == read(variant + '/prototype_sources')['binary_sha256']

    for variant in VARIANTS:
        assert all(measurements[variant + '/' + stage + '_primary']['speed_ratio'] < 1.05
                   for stage in ('formal', 'holdout'))
    snapshot = {'baseline': baseline, 'candidate': candidate,
                'previous_phase_preserved': starting['baseline'],
                'user_workspace_preserved': starting['user_workspace']}
    snapshot_path = RAW / 'final/v1/final_source_snapshot.json.gz'
    if snapshot_path.exists():
        previous = json.loads(gzip.decompress(snapshot_path.read_bytes()))
        assert previous['candidate']['source_digest'] == candidate['source_digest']
        assert previous['baseline'] == baseline
    else:
        with snapshot_path.open('xb') as stream:
            stream.write(gzip.compress((json.dumps(snapshot, indent=2, sort_keys=True) + '\n').encode(), mtime=0))
    artifacts = [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size}
                 for p in sorted(RAW.rglob('*.gz'))]
    manifest = {
        'schema': 'csplendor.phase4c.evidence.v1',
        'phase_start_commit': starting['baseline']['head'],
        'phase_start_source_digest': starting['baseline']['source_digest'],
        'paired_baseline_commit': BASE_COMMIT, 'paired_baseline_source_digest': BASE_DIGEST,
        'candidate_parent_commit': candidate['head'], 'candidate_source_digest': candidate['source_digest'],
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + path + LF; safe src/scripts/tests/csplendor source extensions plus CMakeLists.txt',
        'source_file_manifest': str(snapshot_path.relative_to(ROOT)),
        'branch': 'perf/mcts-concurrency', 'original_dirty_worktree_preserved': True,
        'decisions': {**{x: 'REJECT_AND_REVERT' for x in ('4C-1', '4C-2', '4C-3')},
            'depth_diagnostics': 'RETAIN_PERF_ONLY', '4A-1': 'DEFERRED_BY_USER', '4A-2': 'DEFERRED_BY_USER',
            '5B-R': 'PRIOR_REJECTION_PRESERVED', '4B-1': 'PRIOR_ADOPTION_PRESERVED'},
        'runtime_scope': 'Native 48-action shared MCTS / synthetic evaluator / observable determinization; no live NN, V3, Python Genbu or playing-strength gain claim',
        'primary_gate': 'Each independent ticket: >=1.05 speed ratio, 95% block CI lower >1, independent holdout; unchanged after data',
        'primary': 'hidden_reserve, 20000 completed sims, sharded shared throughput, 8 threads, batch16, latency0, master seed42, fixture seed20260726',
        'shipped_speed_optimization': False, 'final_binary_sha256': final_binary,
        'reverted_binary_identity': binary_identity,
        'validation': {'native_full': '39/39', 'python': '565 passed, 1 skipped, 4 deselected',
            'python_performance': '4 passed', 'py_compile': 'PASS',
            'asan_ubsan_perf_on': '8/8 relevant MCTS and instrumentation suites',
            'tsan_perf_on': '4/4 relevant MCTS and instrumentation suites',
            'prototype_release_perf_reference': '9 suites per profile for 4C-1, 4C-2, 4C-3 v2/v3',
            'hardware_perf': 'N/A: perf_event_paranoid=4; no permission changes',
            'rejected_prototypes_full_matrix_python_sanitizers': 'NOT_RUN: failed primary performance gate'},
        'limitations': ['PERF depth attribution covers traversal only, not coordinator commit/cleanup',
            'Instrumented waits sum across threads and change scheduling; not deployment cost fractions',
            'Short asynchronous release runs are noisy; rejected without lowering threshold or trimming outliers',
            'C3 v3 serial medians regress ~2.3%, but holdout CI includes one; do not claim strict regression bound',
            'Native rss_kind=current_resident_set, not peak RSS; runner RSS is a distinct measurement',
            'C2 lower allocation does not establish large RSS savings; retained capacity/pool adoption not claimed',
            'No all-backend/thread/batch Cartesian sweep; no next implementation ticket started'],
        'next_single_ticket': '5D V3 payment rank/unrank static composition DP; propose only, confirm active V3 caller before implementation',
        'measurements': measurements, 'diagnostics': diagnostics,
        'nonzero_invocations': failures, 'artifacts': artifacts,
    }
    encoded = json.dumps(manifest, indent=2, ensure_ascii=False, sort_keys=True) + '\n'
    if output is None:
        print(encoded, end='')
    else:
        with Path(output).open('x') as stream:
            stream.write(encoded)
        print('Evidence verified:', len(measurements), 'paired records,', len(artifacts), 'compressed artifacts')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', help='New generated JSON manifest; existing output is never overwritten')
    main(parser.parse_args().output)
