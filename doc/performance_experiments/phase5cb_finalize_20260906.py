#!/usr/bin/env python3
"""Audit 5C-B independent gates, preserved sources and immutable artifacts."""
import argparse
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase5cb'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def main(output):
    start = read('table/starting_sources')
    assert source(ROOT.parent / 'csplendor-action-selection') == start['baseline']
    assert source(ROOT.parent / 'csplendor') == start['user_workspace']
    baseline = start['baseline']['head']
    assert baseline == '1980b541bfde2db4d7f8fdba368b12013354881c'
    assert subprocess.check_output(['git', 'diff', '--name-only', baseline, '--', 'src'], cwd=ROOT).decode().splitlines() == ['src/bindings_encoding.cpp']
    assert not subprocess.check_output(['git', 'diff', baseline, '--', 'CMakeLists.txt'], cwd=ROOT)
    identity = read('buffer/binary_identity')
    for item in identity.values():
        assert sha(Path(item['path']).read_bytes()) == item['sha256']
    assert identity['native_baseline']['sha256'] == identity['native_candidate']['sha256']
    for name in ('table/unit_table', 'table/unit_table-reference', 'table/unit_table-diagnostic',
                 'buffer/unit_buffer', 'buffer/python_focused_fixed', 'buffer/native_full',
                 'buffer/python_full', 'buffer/python_performance', 'buffer/python_compile',
                 'buffer/unit_final-diagnostic', 'buffer/unit_final-asan', 'buffer/orchestration_compile'):
        assert read(name)['exit_code'] == 0, name

    allowed_failures = {
        'buffer/python_focused': 'FIXED_TEST_FIXTURE: public editor rejects invalid card IDs before encoding; native invalid-card oracle already passes',
        'buffer/smoke_features_invocation': 'HARNESS_INPUT_REJECTED: --repo differs; no timing admitted. Fixed ELF launchers retain strict runner argument validation',
        'buffer/perf_environment': 'ENVIRONMENT_NA: hardware perf unavailable; no system permission change',
    }
    failures, measurements = [], {}
    for path in sorted(RAW.rglob('*.json.gz')):
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        data = json.loads(gzip.decompress(path.read_bytes()))
        if data.get('exit_code', 0):
            assert name in allowed_failures, name
            failures.append({'raw': str(path.relative_to(ROOT)), 'classification': allowed_failures[name]})
        if 'comparison' not in data:
            continue
        row = data['comparison'][0]
        samples = [p[s]['records'][0] for p in data['pairs'] for s in ('A', 'B')]
        assert all(r['semantics']['correct'] for r in samples), name
        assert len({r['digest'] for r in samples}) == 1, name
        assert all(c['identical_in_every_pair'] for c in row['counters'].values()
                   if c['classification'] == 'correctness'), name
        assert len(data['pairs']) == (4 if name.startswith('table/smoke_') else 22), name
        if name.startswith('table/'):
            assert data['manifests']['B']['binary']['sha256'] == read('table/rejected_prototype')['binary_sha256']
        ratio = row['B_over_A']
        measurements[name] = {'raw': str(path.relative_to(ROOT)), 'contract': 'S0',
            'workload': row['workload'], 'fixture': row['fixture'], 'ratio': ratio['median'],
            'ci95': ratio['crossover_block_bootstrap_ci95'], 'pairs': len(data['pairs']),
            'blocks': ratio['crossover_blocks'], 'digest': samples[0]['digest'],
            'absolute_medians': {k: v['median'] for k, v in row['absolute'].items()},
            'binary_sha256': {s: data['manifests'][s]['binary']['sha256'] for s in ('A', 'B')},
            'settings': data['settings']}
    for stage in ('formal', 'holdout'):
        assert measurements['table/' + stage + '_primary']['ratio'] < 1.03
        for kind in ('features', 'pipeline'):
            row = measurements['buffer/' + stage + '_' + kind]
            assert row['ratio'] >= 1.03 and row['ci95'][0] > 1
    assert measurements['buffer/formal_features_matched']['ci95'][0] > 1
    candidate = source(ROOT)
    snapshot_path = RAW / 'buffer/final_source.json.gz'
    snapshot = {'start': start, 'candidate': candidate}
    if not snapshot_path.exists():
        with snapshot_path.open('xb') as stream:
            stream.write(gzip.compress((json.dumps(snapshot, indent=2, sort_keys=True) + '\n').encode(), mtime=0))
    else:
        assert read('buffer/final_source')['candidate']['source_digest'] == candidate['source_digest']
    manifest = {
        'schema': 'csplendor.phase5cb.evidence.v1',
        'decisions': {'immutable_feature_table': 'REJECT_AND_REVERT',
                      'owning_feature_numpy_api': 'ADOPT',
                      'action_mask_numpy': 'SKIP_ALREADY_DONE',
                      'legal_code_buffers_and_fill': 'DEFER_NO_ACTUAL_CONSUMER'},
        'baseline_commit': baseline, 'baseline_source_digest': start['baseline']['source_digest'],
        'candidate_parent_commit': candidate['head'], 'candidate_source_digest': candidate['source_digest'],
        'source_file_manifest': str(snapshot_path.relative_to(ROOT)),
        'source_digest_algorithm': 'sha256(sorted file SHA256 + two spaces + relative path + LF), safe source extensions',
        'branch': 'perf/feature-table', 'original_dirty_worktree_preserved': True,
        'binary_identity': identity, 'native_engine_binary_identical_to_baseline': True,
        'api': {'new': 'StateEncoder.encode_numpy(game, observer=-1)',
                'consumer': 'csplendor.features.StateFeaturizer.featurize',
                'shape': [196], 'dtype': 'float32', 'C_contiguous': True,
                'ownership': 'independent owning writable ndarray; no stack/reusable-buffer alias',
                'GIL': 'retained', 'caller_out_buffer': 'not accepted; no partial writes',
                'unchanged': ['list encode', 'canonical encode', 'public-card-statistics', 'schema', 'observer semantics', 'float operation order']},
        'measurement_method': 'existing paired runner, CPU4, warmups2, 22 pairs/11 fixed ELF slot crossover blocks, bootstrap10000; table smoke4; Python fixed-launcher screening22',
        'launcher_scope': 'exec Python before timing, compile-time repo and legacy axis, identical CLI args; ELF launcher inode rotates, loaded extension does not. Extension SHA recorded separately; same-extension reference also measured',
        'performance_scope': 'Python feature consumer/pipeline, not NN throughput, game strength or native MCTS speedup',
        'hardware_perf': 'N/A', 'tsan': 'not applicable; no concurrency changes',
        'preserved': ['3E/5E adopted', '5A rejected', '3B/3C/3D/4B-1 contracts', '4A deferred', '4C/5B-R/5D rejected'],
        'validation': {'native': '42 tests', 'python': '581 passed, 1 skipped, 4 performance deselected',
                       'python_performance': '4 passed', 'asan_ubsan': '4 native suites; binding ownership checked in Python',
                       'oracle': 'all196 bitwise, all90 cards, empty, hidden, observer/canonical, random reachable, invalid native editor'},
        'failures': failures, 'measurements': measurements,
        'artifacts': [{'path': str(p.relative_to(ROOT)), 'bytes': p.stat().st_size, 'sha256': sha(p.read_bytes())}
                      for p in sorted(RAW.rglob('*.gz'))],
    }
    with output.open('x') as stream:
        json.dump(manifest, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print('manifest verified:', output, 'source:', candidate['source_digest'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    main(parser.parse_args().output)
