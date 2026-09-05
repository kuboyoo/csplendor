#!/usr/bin/env python3
"""Assemble R0's audit manifest from existing raw evidence; no engine changes."""
import gzip
import io
import json
import statistics
import tarfile
from pathlib import Path

from r0_record_20260905 import OUT, ROOT, SOURCE, command, output, sha, source_files


def read(name):
    return json.loads(gzip.decompress((OUT / (name + '.json.gz')).read_bytes()))


def native(name):
    result = read(name)
    assert result['exit_code'] == 0, (name, result)
    return json.loads(result['stdout'])


def preserve(name, data):
    path = OUT / name
    if path.exists():
        assert gzip.decompress(path.read_bytes()) == data, name
    else:
        output(name, data)


def main():
    inventory = read('source_inventory')
    preserved = {}
    for label, root in [('post3c', SOURCE), ('user_workspace', ROOT)]:
        current = {name: sha((root / name).read_bytes()) for name in source_files(root)}
        assert current == {item['path']: item['sha256'] for item in inventory[label]['files']}
        diff = command(['git', 'diff', '--binary', '--no-ext-diff', 'HEAD'], root)
        assert diff['exit_code'] == 0
        preserve(label + '_full_tracked_diff.patch.gz', diff['stdout'].encode())
        preserved[label] = {'source_unchanged': True, 'full_tracked_diff_sha256': sha(diff['stdout'].encode())}

    # The original source digest deliberately scoped native/package inputs.
    # Preserve the additional top-level packaging helper without rewriting it.
    extra = SOURCE / '_build_support.py'
    archive = io.BytesIO()
    with tarfile.open(fileobj=archive, mode='w') as tar:
        data = extra.read_bytes()
        info = tarfile.TarInfo('_build_support.py')
        info.size, info.mode, info.mtime = len(data), 0o644, 0
        tar.addfile(info, io.BytesIO(data))
    preserve('post3c_packaging_helper.tar.gz', archive.getvalue())
    preserved['packaging_helper'] = {'path': '_build_support.py', 'sha256': sha(extra.read_bytes()),
                                   'outside_original_213_file_digest': True}

    cases = ['exact_shallow', 'exact_deep', 'exact_warm', 'visible_normal', 'visible_cycle',
             'clone', 'copy_restore', 'native_legacy_1t', 'native_root_1t', 'native_root_8t',
             'native_shared_8t']
    measurements = {}
    for name in cases:
        collected = read(name)
        assert read(name + '_invocation')['exit_code'] == 0
        records = [sample['records'][0] for sample in collected['samples']]
        assert all(record['semantics']['correct'] for record in records), name
        summary = collected['summary'][0]
        deterministic = name != 'native_shared_8t'
        if deterministic:
            assert len({record['digest'] for record in records}) == 1, name
        else:
            for record in records:
                c, s = record['counters'], record['semantics']
                assert s['ledger_integrity_ok'] and s['virtual_loss_balanced']
                assert c['issued'] == c['completed'] + c['cancelled'] + c['failed']
                assert c['virtual_loss_added'] == c['virtual_loss_released']
                assert c['selected'] == c['reservations_committed'] + c['reservations_aborted']
        measurements[name] = {
            'raw': name + '.json.gz', 'command': collected['samples'][0]['execution_command'],
            'runs': len(records), 'deterministic_within_same_configuration': deterministic,
            'operations': records[0]['operations'], 'median_rate_per_second': summary['rate_per_second']['median'],
            'median_elapsed_ms': summary['elapsed_ns']['median'] / 1e6,
            'median_runner_peak_rss_kib': summary['runner_rss_kib']['median'],
            'median_native_current_rss_kib': summary['native_rss_kib']['median'],
            'first_sample_semantics': records[0]['semantics'],
            'digest': records[0]['digest'] if deterministic else None,
            'first_sample_public_counters': records[0]['counters'],
        }
        if not deterministic:
            measurements[name]['throughput'] = {
                key + '_per_second': statistics.median(
                    r['counters'][key] * 1e9 / r['elapsed_ns'] for r in records)
                for key in ('completed', 'evaluated_boards', 'selected')}
            measurements[name]['schedule_dependent_counters_not_equality_gated'] = [
                'root_visit_digest', 'root_q_digest', 'tree_size', 'expansion_waited']

    scoring = {}
    for name in ('shallow', 'deep', 'warm'):
        record = native('scoring_' + name)
        expected = read('exact_' + name)['samples'][0]['records'][0]
        assert record['digest'] == expected['digest']
        assert record['semantics'] == expected['semantics']
        assert all(record['counters'][k] == v for k, v in expected['counters'].items()
                   if k != 'instrumentation_enabled')
        counters = record['counters']
        scoring[name] = {key: value for key, value in counters.items()
                         if key.startswith('r0_') or key in ('global_allocation_calls',
                             'global_allocation_bytes', 'clone_light_calls', 'board_snapshot_copies',
                             'board_restores', 'solver_temporary_vector_allocations',
                             'solver_state_key_calls', 'solver_path_depth_max',
                             'solver_tt_probes', 'solver_tt_hits', 'solver_reveal_candidates',
                             'deck_reserve_candidates', 'deck_reserve_branches')}
        scoring[name]['visible_score_calls_per_candidate'] = (
            counters['r0_visible_score_calls'] / counters['r0_visible_sort_candidates'])
        scoring[name]['production_semantics_and_public_counters_match'] = True

    proof = native('proof_anchor')
    previous_proof = json.loads(gzip.decompress((SOURCE / 'doc/performance_experiments/raw/phase3c/'
        'stage1_proof_dag_reveal_heavy_20260905.json.gz').read_bytes()))
    assert proof['digest'] == previous_proof['digest']
    assert {k: v for k, v in proof['semantics'].items() if k != 'warmup_operations'} == {
        k: v for k, v in previous_proof['semantics'].items() if k != 'warmup_operations'}
    assert proof['counters'] == previous_proof['counters']
    assert read('native_contracts')['exit_code'] == read('python_contracts')['exit_code'] == 0

    adjacent = Path('/home/kuboyu/workspace/repos/dlsplendor')
    names = ['dlsplendor/training/self_play.py', 'dlsplendor/search/mcts.py',
             'dlsplendor/search/genbu_adapter.py', 'dlsplendor/network/action_encoder.py',
             'dlsplendor/config.py']
    routes = {'repo': str(adjacent), 'head': command(['git', 'rev-parse', 'HEAD'], adjacent)['stdout'].strip(),
              'status': command(['git', 'status', '--short'], adjacent)['stdout'],
              'reviewed_file_sha256': {name: sha((adjacent / name).read_bytes()) for name in names}}

    decisions = [
        ('0', 'measurement harness', 'ACCEPT', 'baseline_20260902.md'),
        ('1A', 'incremental exact hash', 'ACCEPT', 'phase1a_exact_hash_20260902.md'),
        ('1B', 'incremental observable hash', 'REJECT', 'phase1b_observable_hash_20260902.md'),
        ('2A-1', 'noble eligibility mask', 'ACCEPT', 'phase2a_noble_mask_20260902.md'),
        ('2A-2', 'packed resource delta update', 'REJECT', 'phase2a_packed_resources_rejected_20260902.md'),
        ('2B-H1', 'single-pass legal codes', 'ACCEPT', 'phase2b_single_pass_codes_20260902.md'),
        ('2B-H2', 'closed-form return count', 'ACCEPT', 'phase2b_return_count_20260902.md'),
        ('2B-H3', 'runtime purchase payment count DP', 'REJECT', 'phase2b_purchase_count_dp_rejected_20260902.md'),
        ('2B-H4a', 'return pattern table', 'ACCEPT', 'phase2b_return_pattern_table_20260902.md'),
        ('2B-H4b', 'purchase pattern table filter', 'REJECT', 'phase2b_payment_pattern_table_rejected_20260902.md'),
        ('2B-H5', 'packed code sink', 'ACCEPT', 'phase2b_packed_code_sink_20260902.md'),
        ('3A', 'bounded path stack, card classes, compact reasons/forced actions', 'ACCEPT', 'phase3a_solver_containers_20260902.md'),
        ('3A-extra', 'single map lookup prototype', 'REJECT', 'phase3a_solver_containers_20260902.md'),
        ('3B', 'incremental reveal sidecar', 'ACCEPT', 'phase3b_incremental_reveal_state_20260905.md'),
        ('3C-1', 'compact key/entry in std::unordered_map', 'ACCEPT', 'phase3c_solver_tt_compaction_20260905.md'),
        ('3C-2', 'custom flat TT', 'NOT_IMPLEMENTED', 'phase3c_solver_tt_compaction_20260905.md'),
    ]
    audit = {
        'schema': 'csplendor.post3c.r0.v1', 'date': '2026-09-05', 'scope': 'R0 only',
        'report': 'post3c_review_baseline_20260905.md',
        'baseline': inventory['post3c'], 'user_workspace_at_start': inventory['user_workspace'],
        'public_reference_not_baseline': '0c5eba654ab4536c70947e725872cf5790db5e92',
        'source_preservation': preserved, 'request_hashes': inventory['requests'],
        'prior_evidence_archive': 'raw/r0_20260905/post3c_source_and_prior_evidence.tar.gz',
        'decisions': [{'phase': p, 'proposal': n, 'decision': d, 'report_in_archive': 'doc/performance_experiments/' + f}
                      for p, n, d, f in decisions],
        'phase3c_stage2_note': 'NOT_ENTERED: Stage1 profile gate unmet, not an implemented-and-reverted candidate',
        'duplicate_screen': {
            'single_pass_codes_and_direct_packing': 'SKIP_ALREADY_DONE',
            'incremental_reveal_key': 'SKIP_ALREADY_DONE',
            'existing_full_board_sidecar_scope_guard': 'SKIP_ALREADY_DONE; not delta rollback',
            'compact_tt_and_card_class_reason_path': 'SKIP_ALREADY_DONE',
            '3D-P1_score_once': 'NOT_IMPLEMENTED', '3D-P2_depth_scratch': 'NOT_IMPLEMENTED',
            '3D_delta_rollback': 'NOT_IMPLEMENTED; UndoRecord is diagnostic only',
            '4A_access_epoch_elimination': 'NOT_IMPLEMENTED; writes present, no observed consumer',
            '5B-R_reusable_determinization_board': 'NOT_IMPLEMENTED; clones still used',
            '5D_V3_static_count_codec': 'NOT_IMPLEMENTED; distinct from rejected runtime payment generator DP',
        },
        'contracts': {
            'restoration_field_table': 'post3c_review_baseline_20260905.md#3-3dへの接続契約',
            'sidecar_type': 'solver_internal::RevealSearchState',
            'owner': 'RevealVerifiedSolver::Impl, plus root_reveal_state_ snapshot',
            'sidecar_fields': ['remaining_by_level_', 'remaining_all_', 'acquired_hidden_', 'claimed_',
                               'rule_hash_', 'deck_order_hash_', 'active_'],
            'lifecycle': ['begin_search', 'initialize', 'apply_tracked', 'observe_before', 'observe_after',
                          'move_deck_card_to_back', 'ScopedBranchRollback::restore'],
            'fallback': 'noncanonical root or unsupported transition deactivates sidecar, uses legacy scan; VERIFY aborts inconsistency',
            'tt': 'std::unordered_map, full stored-key equality; rehash preserves element references, not iterators',
            'persistent_capacity': 'trim before/after search, NOT insertion-time hard cap; reserve ceiling2M; clear retains buckets',
            'persistent_mode': 'attacker fixed per solver; Python session clears on simple_payment_mode change; native caller must clear',
            'reference': 'full Board+sidecar restore, incremental reveal flag OFF scan, compact TT flag OFF original types; public Game::undo full Board',
            'limits': 'node budget every node; cancel/time poll every64 nodes including0; UNKNOWN is not refutation',
        },
        'runner_audit': routes,
        'runtime_paths': {
            'puzzle_generation': 'PuzzleGenbuMCTS -> Python GenbuMCTS -> legacy GenbuAdapter PyTorch CPU',
            'dlsplendor_selfplay': 'Python MCTS -> V3 (3133 IDs) -> direct PyTorch network or inference_client/Ray',
            'native_mcts': '48-action API; legacy/root/shared benchmark slices are library characterization, not observed training backend',
            'neural_network_throughput': 'UNVERIFIED: no identified live launch configuration/checkpoint; no weights loaded',
        },
        'measurements': measurements, 'scoring_diagnostics': scoring,
        'v3': native('python_encoding'), 'layout': native('diagnostic_layout')['semantics'],
        'populated_clone': native('populated_layout_probe')['semantics'],
        'aa_only_not_speedup': read('aa_exact')['comparison'][0]['B_over_A'],
        'failure_classification': {
            'environment_unavailable': ['hardware perf: exit1, perf_event_paranoid=4; settings unchanged'],
            'baseline_failures': [],
            'expected_budget_results': ['exact_deep', 'exact_warm', 'visible_normal', 'visible_cycle'],
            'candidate_failures': 'N/A: no production candidate',
            'setup_corrected': 'CMake Python3.8 auto-selection overridden with Python3.12.1 before import and measurement',
            'not_run': ['full Python/sanitizer suites', 'real NN MCTS throughput', 'full backend/thread/batch/fixture matrix'],
        },
        'tests': {'native': '4 passed', 'python': '15 passed, including known5/7 mate, reuse/cap, proof, encoders',
                  'proof_anchor_matches_prior3c': True, 'diagnostic_matches_production': True,
                  'proof_anchor_configuration_difference': 'warmup100 in R0 versus1000 in prior3C; excluded from semantic comparison',
                  'standalone_plan_probes_used_as_engine_gate': False},
        'next_ticket': {
            'id': '3D-P1', 'status': 'PROPOSED_ONLY_NOT_STARTED',
            'change': 'action-local score-once candidates; preserve score descending, card ID ascending',
            'primary_metric': 'exact_deep hidden_reserve d7 fixed1M nodes walltime; same work rate_B/rate_A',
            'supporting_metrics': ['exact_shallow', 'exact_warm', 'score calls per candidate', 'RSS'],
            'reference': 'this archived post3c working tree; old comparators with score formulas unchanged',
            'performance_gate': '22pairs/11crossoverblocks + independent holdout; reproducible2-3% primary, CI excludes1; major guards no confirmed>2% regression',
            'semantic_gate': 'all candidate scores/ordered IDs and fixed-work counters/digests/PL/proof/frontier equal; editor fallback, return colors, <=1 candidate, failed blank probe, terminal/noble wait, limits/exception/cache reuse',
            'do_not_mix': ['delta rollback', 'new TT', 'global score cache', 'floating-point reorder', 'scratch frames'],
        },
        'optimization_speedup': None, 'stop_after_R0': True, 'push': False,
    }
    audit['raw_artifacts'] = [{'path': 'raw/r0_20260905/' + path.name,
                               'bytes': path.stat().st_size, 'sha256': sha(path.read_bytes()),
                               'uncompressed_sha256': sha(gzip.decompress(path.read_bytes()))}
                              for path in sorted(OUT.glob('*.gz'))]
    target = Path(__file__).with_name('post3c_review_baseline_20260905.json')
    with target.open('x') as stream:
        json.dump(audit, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print('Saved', target, 'raw archives', len(audit['raw_artifacts']))


if __name__ == '__main__':
    main()
