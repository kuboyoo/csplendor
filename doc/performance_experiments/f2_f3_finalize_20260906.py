#!/usr/bin/env python3
"""Validate saved acceptance, classify the frozen diff, and add new evidence."""
import csv
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys

import f2_f3_record_20260906 as r

BASE = 'f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc'
DOC = r.ROOT / 'doc/performance_experiments'


def read(name):
    return json.loads(gzip.decompress((r.RAW / (name + '.json.gz')).read_bytes()))


def result(name):
    item = read(name)
    assert item['exit_code'] == 0, name
    return json.loads(item['stdout'].splitlines()[-1])


def validate_saved_positions():
    # Actual file reload in a separate process from producer, using its same .so.
    sys.path.insert(0, str(r.ROOT))
    import csplendor as cs
    from csplendor import _csplendor as core
    import numpy as np
    expected = read('start')['f1_verification']['python_build_reused']
    assert str(Path(core.__file__).resolve()) == expected['path']
    assert r.sha(core.__file__) == expected['sha256']
    models = {}
    for name in ('real_model_v2', 'recommended_model'):
        sample = result(name)['model']
        assert r.sha(sample['model']) == sample['model_sha256']
        assert r.sha(sample['config']) == sample['config_sha256']
        for row in sample['rows']:
            snapshot = bytes.fromhex(row['input_snapshot_hex'])
            game = cs.Game.deserialize_snapshot(snapshot)
            assert game.serialize_snapshot() == snapshot
            player = game.current_player
            features = np.concatenate((
                np.asarray(cs.StateEncoder.encode_canonical(game, player, player), dtype=np.float32),
                np.asarray(cs.StateEncoder.encode_public_card_statistics(game, player, player), dtype=np.float32)))
            assert hashlib.sha256(features.tobytes()).hexdigest() == row['features']['sha256']
            action = cs.ActionEncoderV3.decode_and_match(row['action_id'], game)
            assert action.pack() == row['action_code'] and game.is_legal(action)
            assert game.apply(action, False)
            assert game.serialize_snapshot().hex() == row['output_snapshot_hex']
        models[name] = sample
    frontier = result('frontier_transport_identity')['frontier']
    assert frontier['loaded_worker_extension'] == {expected['path']: expected['sha256']}
    for record in frontier['records']:
        for layer in record['layers']:
            edge = layer['chosen_edge']
            game = cs.decode_mate_frontier_state(edge['child_state'])
            assert game.current_player == edge['child_player']
            assert cs.decode_mate_frontier_state(cs.encode_mate_frontier_state(game)).serialize_snapshot() == game.serialize_snapshot()
        assert game.is_game_over() and game.winner == record['winner'] == 0
    return {'models': models, 'frontier': frontier,
            'extension_path': str(core.__file__), 'extension_sha256': r.sha(core.__file__),
            'saved_snapshot_replay': 'PASS'}


def category(name):
    if name.startswith('tests/') or name == 'doc/refactoring_contracts.json':
        return 'tests', 'Retained independent oracle/regression or test contract'
    if name in {'src/perf_counters.h', 'src/perf_counters.cpp',
                'src/mcts_bounded_queue.h', 'src/mcts_concurrent_tree.h',
                'src/mcts_parallel_searcher.h'}:
        return 'measurement_support', 'PERF-only counters/queue-lock-ledger diagnostics; not adoption of rejected 4C prototypes'
    if name.startswith(('src/', 'csplendor/')) or name == 'CMakeLists.txt':
        return 'adopted_code', 'Retained adoption and reference/fallback paths; F1 source/binary evidence reused'
    if name.startswith('scripts/'):
        return 'measurement_support', 'Paired, baseline, differential or benchmark tooling'
    if name.startswith('doc/performance_experiments/') and not name.endswith('.md'):
        return 'measurement_support', 'Evidence/manifest/CSV, isolated driver or verification helper; not production patch'
    if name.startswith('doc/') or name in {'README.md', 'README.en.md', 'CHANGELOG.md'}:
        return 'documentation', 'Adoption/rejection history, API docs, review or release preparation'
    return 'unrelated', 'Requires manual classification before integration'


def main():
    start = read('start')
    now = r.verify_f1()
    assert now['current']['source_digest'] == start['f1_verification']['current']['source_digest']
    assert now['build_files'] == start['f1_verification']['build_files']
    assert [r.consumer(p) for p in r.CONSUMERS] == start['consumers'], 'External tree advanced; record, never restore'
    assert r.source(r.ROOT.parent / 'csplendor') == start['original']
    remote = r.git(r.ROOT, 'ls-remote', 'origin', 'refs/heads/main')
    assert remote.split()[0] == BASE, 'Main advanced; reassess integration scope'
    acceptance = validate_saved_positions()
    for name in ('venv', 'session_cancel_resume', 'gui_bridge_tests', 'main_ci_lint'):
        assert read(name)['exit_code'] == 0, name
    lint = read('ci_lint_available_tool')
    assert lint['exit_code'] == 1 and 'Found 7 errors.' in lint['stdout']
    assert read('real_model')['exit_code'] == 1 and 'apply_action' in read('real_model')['stderr']
    assert read('ci_lint')['exit_code'] == 1 and 'FileNotFoundError' in read('ci_lint')['stderr']

    changed = {}
    for line in r.git(r.ROOT, 'diff', '--name-status', BASE, '--').splitlines():
        status, name = line.split('\t')
        changed[name] = status
    for name in r.git(r.ROOT, 'ls-files', '--others', '--exclude-standard').splitlines():
        changed[name] = 'A'
    inventory_path = DOC / 'f3_diff_inventory_20260906.csv'
    manifest_path = DOC / 'f2_f3_manifest_20260906.json'
    for path in (inventory_path, manifest_path):
        changed[str(path.relative_to(r.ROOT))] = 'A'
    # The exclusive-created closing raw record below is also in the review.
    changed[str((r.RAW / 'final_validation.json.gz').relative_to(r.ROOT))] = 'A'
    counts = {}
    rows = []
    for name, status in sorted(changed.items()):
        group, reason = category(name)
        counts[group] = counts.get(group, 0) + 1
        assert group != 'unrelated', name
        assert not name.startswith(('build/', 'venv/'))
        assert Path(name).suffix not in {'.pt', '.pth', '.so', '.o', '.whl'}
        rows.append({'status': status, 'path': name, 'classification': group, 'reason': reason})
    with inventory_path.open('x', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=['status', 'path', 'classification', 'reason'], lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)
    r.save('final_validation', {
        'source_digest': now['current']['source_digest'], 'build_files': now['build_files'],
        'remote_main': remote, 'local_refs': r.git(r.ROOT, 'rev-parse', 'main', 'origin/main'),
        'consumer_trees_unchanged': True, 'original_workspace_unchanged': True,
        'old_f1_raw_unchanged': True, 'saved_snapshot_replay': acceptance['saved_snapshot_replay'],
        'review_counts': counts,
        'model_sources_are_external_read_only_dirty_inputs': True,
        'phase6_full_audit_or_f1_benchmarks_rerun': False,
        'ruff_version': subprocess.check_output(['python', '-m', 'ruff', '--version'], text=True).strip()})
    artifacts = [{'path': str(p.relative_to(r.ROOT)), 'sha256': r.sha(p), 'bytes': p.stat().st_size}
                 for p in sorted(r.RAW.glob('*.json.gz'))]
    metadata = {'schema': 'csplendor.f2_f3.review.v1', 'status': 'BLOCKED',
        'reason': 'Candidate-only CI lint: 7 errors in 5 added test files; main passes. Frozen engine/tests not modified.',
        'f4': 'NOT_EXECUTED_AWAITING_APPROVAL', 'BASE_MAIN_SHA': BASE,
        'ENGINE_CODE_SHA': r.ENGINE, 'F1_RECORD_SHA': r.F1,
        'source_digest': now['current']['source_digest'], 'build_files': now['build_files'],
        'review_branch': r.git(r.ROOT, 'branch', '--show-current'),
        'review_record_parent_commit': r.git(r.ROOT, 'rev-parse', 'HEAD'),
        'record_commit_resolution': 'git log -1 --format=%H -- doc/performance_experiments/f2_f3_shipping_review_20260906.md',
        'integration_head_note': 'Full post-recording SHA is reported after local commit; re-resolve/freeze after approved lint fixes. Do not treat current BLOCKED tip as merge-ready.',
        'implementation_build_existing_tests_unchanged': True,
        'F1_evidence_reused': {'raw_hashes_verified': 100, 'csv_sha256': now['csv_sha256'],
                             'manifest_sha256': now['manifest_sha256']},
        'python_extension': {'path': acceptance['extension_path'], 'sha256': acceptance['extension_sha256'],
                             'flags_reference': 'start.json.gz:f1_verification.python_build_reused'},
        'consumers': [{k: c[k] for k in ('root', 'head', 'status', 'source_digest')} for c in start['consumers']],
        'consumer_changes_included_in_commit': False,
        'models': {name: {k: value[k] for k in ('model', 'model_sha256', 'config', 'config_sha256',
                    'backend', 'action_space', 'action_dim', 'payment_mode', 'device', 'precision',
                    'torch', 'torch_threads', 'model_load_ms', 'total_ms', 'effective_search_config')}
                   for name, value in acceptance['models'].items()},
        'acceptance': {'real_cpu_models': 'PASS_TWO_MODELS', 'gui_handlers': 'PASS_TWO_MODELS_TWO_MOVES_EACH',
            'gui_json_frontier_5_7': 'PASS_VISITED_DEFENDER_LAYERS_COMPLETE_NOT_ENTIRE_DAG',
            'mate_session_override_warm': 'PASS', 'session_cancel_resume_tests': 3,
            'gui_bridge_tests': 7, 'owning_numpy_torch_lifetime': 'PASS', 'saved_reload': 'PASS'},
        'not_executed': ['Browser/HTTP rendering and UI cancellation', 'GPU/CUDA/MPS',
            'Genbu: documented model and legacy module absent', 'Native48 real NN',
            'Real-model paired A/B / confidence interval / strength', 'Long games / all checkpoints',
            'New integrated wheel install', 'Hosted CI and branch-protection checks', 'F4 and publication'],
        'genbu_required_files': {str(p): p.is_file() for p in [
            r.ROOT.parent/'alphazero-general-ori/splendor/SplendorGame.py',
            r.ROOT.parent/'alphazero-general-ori/HeianKyo/genbu.pt']},
        'failures': [
            {'raw': 'real_model.json.gz', 'type': 'FIXED_PROBE_API_ERROR', 'resolution': 'real_model_v2 PASS; engine unchanged'},
            {'raw': 'ci_lint.json.gz', 'type': 'ENVIRONMENT_TOOL_WRAPPER', 'resolution': 'Existing system Ruff used read-only'},
            {'raw': 'ci_lint_available_tool.json.gz', 'type': 'UNRESOLVED_CANDIDATE_ONLY_CI', 'baseline': 'main_ci_lint PASS'}],
        'runtime_candidate_failures': [],
        'timing_scope': 'Single candidate-only observations; not F1-style controlled A/B, no CI, no speedup claim',
        'lto_default': 'OFF', 'parallel_mcts_cumulative_gain': 'UNCONFIRMED', 'lto_added_gain': 'UNCONFIRMED',
        'diff_inventory': {'path': str(inventory_path.relative_to(r.ROOT)), 'sha256': r.sha(inventory_path), 'counts': counts},
        'artifacts': artifacts,
        'next_step': 'Report BLOCKED and stop; approve minimal lint correction/revalidation before any F4 action'}
    with manifest_path.open('x') as stream:
        json.dump(metadata, stream, ensure_ascii=False, sort_keys=True, indent=2)
        stream.write('\n')
    print(json.dumps({'counts': counts, 'artifacts': len(artifacts), 'saved_reload': 'PASS',
                      'status': metadata['status']}, ensure_ascii=False))


if __name__ == '__main__':
    main()
