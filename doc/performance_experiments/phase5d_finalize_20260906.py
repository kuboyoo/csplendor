#!/usr/bin/env python3
"""Audit 5D source preservation, paired measurements and rejection gates."""
import argparse
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase5d'
BASE = ROOT.parent / 'csplendor-v3-payment-baseline'
PRIOR = ROOT.parent / 'csplendor-mcts-concurrency'
USER = ROOT.parent / 'csplendor'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def main(output):
    initial = read('foundation/starting_sources')
    baseline, candidate = source(BASE), source(ROOT)
    assert source(PRIOR) == initial['baseline']
    assert source(USER) == initial['user_workspace']
    assert baseline == read('v1/starting_sources')['baseline']
    assert baseline['head'] == '3c3cb3e74d2efabbc0182cd1e32518a36100bd88'
    assert not subprocess.check_output(['git', 'diff', initial['baseline']['head'], '--', 'src', 'CMakeLists.txt'], cwd=ROOT)
    assert not (ROOT / 'src/v3_payment_dp.h').exists()
    assert 'CSPLENDOR_V3_PAYMENT_DP' not in (ROOT / 'CMakeLists.txt').read_text()
    identity = read('final/reverted_binary_identity')
    assert identity['identical'] and read('final/deployment_binary_identity')['identical']
    final_sha = sha((ROOT / 'build/5d-release/benchmark_engine_hotpaths').read_bytes())
    assert final_sha == identity['binary_sha256']['baseline'] == read('final/deployment_binary_identity')['after']
    for variant in ('v1', 'v2'):
        for profile in ('release', 'reference', 'diagnostic'):
            assert read(variant + '/unit_' + profile)['exit_code'] == 0
    for name in ('unit_release', 'unit_diagnostic', 'native_full', 'python_full',
                 'python_performance', 'python_compile', 'unit_asan', 'orchestration_compile'):
        assert read('final/' + name)['exit_code'] == 0, name

    measurements, diagnostics, failures = {}, {}, []
    prototypes = {v: read(v + '/prototype_sources') for v in ('v1', 'v2')}
    for path in sorted(RAW.rglob('*.json.gz')):
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        data = json.loads(gzip.decompress(path.read_bytes()))
        if data.get('exit_code', 0):
            assert name == 'v2/perf_environment', name
            failures.append({'raw': str(path.relative_to(ROOT)), 'exit_code': data['exit_code'],
                             'classification': 'environment_unavailable_perf_event_paranoid_4'})
        if '/profile_' in name:
            r = json.loads(data['stdout'])
            assert r['semantics']['correct']
            diagnostics[name] = {'raw': str(path.relative_to(ROOT)), 'operations': r['operations'],
                'digest': r['digest'], 'current_rss_kib': r['rss_kib'],
                'counters': r['counters'], 'timing_not_an_adoption_gate': True}
        if 'comparison' not in data:
            continue
        row = data['comparison'][0]
        ratio = row['B_over_A']
        screening = '/smoke_' in name
        assert len(data['pairs']) == (4 if screening else 22)
        samples = [p[s]['records'][0] for p in data['pairs'] for s in ('A', 'B')]
        assert all(r['semantics']['correct'] for r in samples)
        assert len({r['digest'] for r in samples}) == 1, name
        assert all(c['identical_in_every_pair'] for c in row['counters'].values()
                   if c['classification'] == 'correctness'), name
        variant = name.split('/')[0]
        assert data['manifests']['A']['binary']['sha256'] == final_sha
        assert data['manifests']['B']['binary']['sha256'] == prototypes[variant]['binary_sha256']
        measurements[name] = {'raw': str(path.relative_to(ROOT)), 'workload': row['workload'],
            'fixture': row['fixture'], 'operations': samples[0]['operations'], 'contract': 'S0',
            'screening_only': screening, 'speed_ratio': ratio['median'],
            'ci95': ratio['crossover_block_bootstrap_ci95'], 'pairs': len(data['pairs']),
            'crossover_blocks': ratio['crossover_blocks'], 'statistical_unit': ratio['bootstrap_unit'],
            'cpu_set': data['settings']['cpu_set'], 'digest': samples[0]['digest'],
            'absolute_medians': {k: v['median'] for k, v in row['absolute'].items()},
        }
    for name in ('primary', 'encode', 'decode', 'selfplay'):
        rows = [json.loads(read('v1/profile_' + label + '_' + name)['stdout'])
                for label in ('reference', 'candidate', 'diagnostic')]
        assert len({r['digest'] for r in rows}) == 1
        assert all(r['semantics'] == rows[0]['semantics'] for r in rows)
    for variant in ('v1', 'v2'):
        for stage in ('formal', 'holdout'):
            guard = measurements[variant + '/' + stage + '_selfplay']
            assert guard['speed_ratio'] < 0.98 and guard['ci95'][1] < 1
    for stage in ('formal', 'holdout'):
        primary = measurements['v2/' + stage + '_primary']
        assert primary['speed_ratio'] >= 1.03 and primary['ci95'][0] > 1

    snapshot = {'baseline': baseline, 'candidate': candidate, 'phase_start': initial['baseline'],
                'user_workspace_preserved': initial['user_workspace']}
    snapshot_path = RAW / 'final/source_snapshot.json.gz'
    if snapshot_path.exists():
        old = json.loads(gzip.decompress(snapshot_path.read_bytes()))
        assert old['candidate']['source_digest'] == candidate['source_digest'] and old['baseline'] == baseline
    else:
        with snapshot_path.open('xb') as stream:
            stream.write(gzip.compress((json.dumps(snapshot, indent=2, sort_keys=True) + '\n').encode(), mtime=0))
    manifest = {
        'schema': 'csplendor.phase5d.evidence.v1', 'decision': 'REJECT_AND_REVERT',
        'phase_start_commit': initial['baseline']['head'],
        'phase_start_source_digest': initial['baseline']['source_digest'],
        'paired_baseline_commit': baseline['head'], 'paired_baseline_source_digest': baseline['source_digest'],
        'candidate_parent_commit': candidate['head'], 'candidate_source_digest': candidate['source_digest'],
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + path + LF; safe src/scripts/tests/csplendor extensions and CMakeLists.txt',
        'source_file_manifest': str(snapshot_path.relative_to(ROOT)),
        'branch': 'perf/v3-payment-dp', 'original_dirty_worktree_preserved': True,
        'runtime_scope': 'Public V3 3133-action mask/codec and deterministic random V3 mask-decode_and_match-apply runner; no NN/training or native48 speedup claim',
        'consumer_paths': ['csplendor/api/ai_manager.py', 'src/bindings_encoding.cpp', 'src/action_encoder_v3.h'],
        'primary': 'v3_action_mask / gold_payment / 50000 calls / full payment mode / master seed42',
        'success_gate': '>=1.03 primary, block CI lower>1, independent holdout; no repeated >2% major-guard regression',
        'rejection_reason': 'Second DP variant (v2) passes primary but fails the prespecified V3 selfplay guard in formal and holdout. No metric/threshold pivot.',
        'prototypes': {v: {'source_digest': p['source']['source_digest'], 'binary_sha256': p['binary_sha256'],
            'archive': 'doc/performance_experiments/raw/phase5d/' + v + '/prototype_sources.json.gz', 'decision': p['decision']}
            for v, p in prototypes.items()},
        'production_src_and_cmake_unchanged': True, 'final_binary_identity': identity,
        'retained': ['4 public-path native workloads', 'native enumeration/uint8/full-ID/mask oracle', '3 Python binding regression tests', 'documentation and raw measurements'],
        'preserved_decisions': {'4A-1': 'DEFERRED_BY_USER', '4A-2': 'DEFERRED_BY_USER',
            '5B-R': 'REJECTED', '4C-1/2/3': 'REJECTED', '4B-1': 'ADOPTED'},
        'validation': {'prototype_native': '4 suites in each Release/reference/PERF profile, v1 and v2',
            'final_native': '40/40', 'final_python': '568 passed, 1 skipped, 4 deselected',
            'final_python_performance': '4 passed', 'final_py_compile': 'PASS',
            'final_asan_ubsan': '4/4 relevant codec/schema/rule/MCTS suites',
            'prototype_full_python_and_sanitizers': 'NOT_RUN: regression gate failed',
            'tsan': 'NOT_RUN: no new shared mutable state; production code reverted',
            'hardware_perf': 'N/A: perf_event_paranoid=4; no permission changes'},
        'limitations': ['Full thread/backend/fixture matrix not run after decisive guard regression',
            'v2 codec micro only screened; v1 codec micro has formal results, not independent holdout',
            'Random V3 selfplay is not an NN policy/training throughput or playing-strength evaluation',
            'Selfplay holdout noise is retained; v2 CI does not bound the regression strictly beyond 2%',
            'Generated-code size change is observed, but remaining selfplay regression cause is not proven',
            'Native RSS is current_resident_set, not peak RSS; table was 6480 immutable bytes',
            'No multiplication with historical phase speedups'],
        'next_single_ticket': '3E visible-only take-child representative grouping; proposal only, re-profile before implementation',
        'measurements': measurements, 'diagnostics': diagnostics, 'nonzero_invocations': failures,
        'artifacts': [{'path': str(p.relative_to(ROOT)), 'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size}
                      for p in sorted(RAW.rglob('*.gz'))],
    }
    encoded = json.dumps(manifest, indent=2, ensure_ascii=False, sort_keys=True) + '\n'
    if output:
        with Path(output).open('x') as stream: stream.write(encoded)
        print('5D evidence verified:', len(measurements), 'paired records;', len(manifest['artifacts']), 'artifacts')
    else:
        print(encoded, end='')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', help='Create new generated evidence JSON; never overwrite')
    main(parser.parse_args().output)
