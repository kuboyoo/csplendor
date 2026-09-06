#!/usr/bin/env python3
"""Fail closed on missing gates, changed protected sources or unknown failures."""
import gzip
import hashlib
import json
import statistics
import subprocess
from pathlib import Path
import phase6_record_20260906 as r


def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def read(name):
    return json.loads(gzip.decompress((r.RAW/(name+'.json.gz')).read_bytes()))


def main():
    start = read('starting_sources')
    assert r.source(r.ROOT.parent/'csplendor-feature-table') == start['baseline']
    external = read('external_workspace_observed_change')
    assert external['start'] == start['user_workspace']
    assert subprocess.run(['git','merge-base','--is-ancestor',start['user_workspace']['head'],
                           external['observed']['head']],cwd=r.ROOT.parent/'csplendor').returncode == 0
    assert not subprocess.check_output(['git','diff','HEAD','--','src','csplendor','setup.py','_build_support.py'],cwd=r.ROOT)
    final = r.source(r.ROOT)
    assert final['source_digest'] == read('final_source')['source']['source_digest']
    required = ['full_native_portable','full_native_lto','full_python','python_performance',
        'python_compile','unit_asan','unit_tsan','unit_oracle','unit_diagnostic','slot_retry_tests',
        'portable_wheel','wheel_import','profile_tests_fixed','v2/build_pgo_use']
    for name in required:
        assert read(name)['exit_code'] == 0, name
    assert 'warning:' not in read('v2/build_pgo_use')['stderr'], 'do not ignore PGO warnings'
    expected = {
        'profile_tests': 'FIXED_CANDIDATE_HARNESS: missing run_paired keyword, no engine failure',
        'build_pgo_use': 'FIXED_TRAINING: unused archive TUs absent; v2 registers all core profiles, no warning suppression',
        'lto_extended_root_invocation': 'BASELINE_REPRODUCED_HARNESS: ETXTBSY outside timing',
        'lto_extended_retry_root_invocation': 'BASELINE_REPRODUCED_HARNESS: ETXTBSY outside timing',
        'lto_baseline_aa_root_invocation': 'BASELINE_AA_REPRODUCTION: ETXTBSY, fixed bounded retry',
        'perf_environment': 'ENVIRONMENT_NA: paranoid=4, no permission changes',
        'linux_cross_native_guard': 'EXPECTED_REJECTION: native cross build',
        'native_wheel_guard': 'EXPECTED_REJECTION: CPU-specific distribution',
        'skip_build_wheel_guard': 'EXPECTED_REJECTION: stale distribution binary risk',
        'pgo_stale_guard': 'EXPECTED_REJECTION: source/compiler/flags contract mismatch',
        'pgo_existing_profile_guard': 'EXPECTED_REJECTION: refuse silent reuse/merge of old training',
    }
    measurements, failures = {}, []
    for p in sorted(r.RAW.rglob('*.json.gz')):
        name = str(p.relative_to(r.RAW)).removesuffix('.json.gz')
        data = json.loads(gzip.decompress(p.read_bytes()))
        if not isinstance(data, dict):
            continue
        if data.get('exit_code', 0):
            assert name in expected, name
            failures.append({'raw':str(p.relative_to(r.ROOT)), 'classification':expected[name]})
        if 'comparison' not in data:
            continue
        assert len(data['pairs']) == (4 if '_smoke_' in name else 22), name
        row = data['comparison'][0]
        samples = [pair[s]['records'][0] for pair in data['pairs'] for s in ('A','B')]
        assert all(item['semantics']['correct'] for item in samples), name
        async_mode = row['workload'] == 'parallel_scheduler' and samples[0]['semantics']['threads'] > 1
        if not async_mode:
            assert len({item['digest'] for item in samples}) == 1, name
        else:
            for item in samples:
                c = item['counters']
                assert c['issued'] == c['completed']+c['failed']+c['cancelled'] == item['operations']
                assert c['virtual_loss_added'] == c['virtual_loss_released']
                assert c['integrity_errors'] == 0 and not item['semantics']['partial']
        assert all(v['identical_in_every_pair'] for v in row['counters'].values()
                   if v['classification'] == 'correctness'), name
        ratio = row['B_over_A']
        measurements[name] = {'raw':str(p.relative_to(r.ROOT)), 'classification':'S1' if async_mode else 'S0',
            'workload':row['workload'],'fixture':row['fixture'],'speedup':ratio['median'],
            'ci95':ratio['crossover_block_bootstrap_ci95'],'absolute':row['absolute'],
            'settings':data['settings'],'pairs':len(data['pairs']), 'blocks':ratio['crossover_blocks'],
            'binary_sha256':{s:data['manifests'][s]['binary']['sha256'] for s in ('A','B')},
            'text_busy_retries':sum(pair[s]['binary_slot'].get('text_busy_retries_before_timing',0)
                                    for pair in data['pairs'] for s in ('A','B'))}
        if async_mode:
            measurements[name]['throughput'] = {}
            for side in ('A','B'):
                records = [pair[side]['records'][0] for pair in data['pairs']]
                measurements[name]['throughput'][side] = {
                    **{label:statistics.median(item['counters'][counter]*1e9/item['elapsed_ns'] for item in records)
                       for label,counter in [('completed_sims_per_second','completed'),('unique_evaluated_leaves_per_second','evaluated_boards'),
                            ('path_steps_per_second','selected'),('waiters_per_second','inference_waiters')]},
                    'owner_waiter_ratio_median':statistics.median(item['counters']['evaluation_owners']/max(1,item['counters']['inference_waiters']) for item in records),
                    'zero_waiter_runs':sum(item['counters']['inference_waiters']==0 for item in records),
                    'stop_reasons':sorted({item['semantics']['stop_reason'] for item in records})}
    for stage in ('formal','holdout','final'):
        m = measurements['lto_'+stage+'_primary']
        assert m['speedup'] >= 1.03 and m['ci95'][0] > 1
    for stage in ('formal','holdout'):
        assert measurements['native_'+stage+'_primary']['speedup'] < 1.03
        assert measurements['native_'+stage+'_visible']['ci95'][1] < .98
        assert measurements['use_'+stage+'_random']['ci95'][1] < .98
    assert measurements['lto_guard_recheck_mcts']['speedup'] >= .98
    identity = {}
    for name in ('portable','lto','native','use-v2'):
        binary = r.directory(name)/'benchmark_engine_hotpaths'
        identity[name] = {'path':str(binary),'bytes':binary.stat().st_size,'sha256':sha(binary)}
    assert identity['portable']['sha256'] == sha(r.ROOT.parent/'csplendor-feature-table/build/5cb-buffer/benchmark_engine_hotpaths')
    assert identity['lto']['sha256'] == read('audit_lto')['binaries'][0]['sha256']
    out = r.ROOT/'doc/performance_experiments/phase6_evidence_20260906.json'
    payload = {'schema':'csplendor.phase6.evidence.v1',
        'baseline_commit':start['baseline']['head'],'baseline_source_digest':start['baseline']['source_digest'],
        'candidate_parent_commit':final['head'],'candidate_source_digest':final['source_digest'],
        'source_manifest':'doc/performance_experiments/raw/phase6/final_source.json.gz',
        'original_workspace_untouched_by_this_task':True,
        'original_workspace_start_head':start['user_workspace']['head'],
        'original_workspace_independently_advanced_to':external['observed']['head'],
        'decisions':{'release_lto':'ACCEPT_OPT_IN','linux_native':'REJECT_AND_REVERT','pgo':'REJECT_AND_REVERT',
                     'python_lto':'SKIP_ALREADY_DONE','4A_1_2':'USER_DEFERRED'},
        'binary_identity':identity,'portable_binary_identical_to_5cb':True,
        'prior_artifacts_verified':read('prior_artifact_audit')['count'],
        'correctness':{'native_portable':42,'native_lto':42,'python':'586 passed, 1 skipped, 4 deselected',
                       'python_performance':4,'asan_ubsan_suites':8,'tsan_suites':4,'strong_oracle_suites':3,
                       'wheel':'built portable and explicitly loaded wheel extension; native/skip-build rejected'},
        'mcts_guard_caveat':'Serial MCTS showed 0.9892/0.9850/0.9776/0.9834 across ALL formal/holdout/final/recheck series. No MCTS speedup; strict 2% noninferiority not established. LTO stays opt-in, recommended for measured solver workload only.',
        'scope':'Build-profile effects on final adopted engine; no algorithm changes, no multiplied historical speedups, no Python/real-NN speedup claim',
        'limitations':['hardware perf N/A paranoid=4','Apple/Windows/cross-host runtime untested','real NN/selfplay quality untested',
                       'primary depth7 ends at node limit UNKNOWN, not a completed 7-move proof','PGO rejected; no full PGO Python/sanitizer adoption gate',
                       'original workspace separate mate-frontier fixes not integrated','4A remains deferred'],
        'failures':failures,'measurements':measurements,
        'artifacts':[{'path':str(p.relative_to(r.ROOT)),'bytes':p.stat().st_size,'sha256':sha(p)} for p in sorted(r.RAW.rglob('*.gz'))],
        'next_single_ticket':'NONE; requested final review completed, stop'}
    with out.open('x') as stream:
        json.dump(payload,stream,indent=2,sort_keys=True)
        stream.write('\n')
    print('verified',out,'source',final['source_digest'])


if __name__ == '__main__':
    main()
