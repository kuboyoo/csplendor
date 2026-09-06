#!/usr/bin/env python3
"""Summarize existing paired results without redoing timing/bootstrap or audits."""
import csv
import gzip
import hashlib
import json
import statistics
import subprocess
from pathlib import Path
import final_integration_record_20260906 as r


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(name):
    return json.loads(gzip.decompress((r.RAW/(name+'.json.gz')).read_bytes()))


def main():
    frozen = read('frozen_candidate')
    candidate, baseline = r.source(r.ROOT), r.source(r.BASE)
    assert candidate['source_digest'] == frozen['candidate']['source_digest']
    assert baseline['source_digest'] == frozen['main']['source_digest']
    assert frozen['candidate']['head'] == 'b202e6a0cbb2eded9bc2ee5e59f750428e73ca49'
    required = ['ctest_main','ctest_candidate','python_tests_A','python_tests_B',
                'ctest_reference','python_reference','ctest_asan','python_performance_A',
                'python_performance_B','python_compile']
    for name in required:
        assert read(name)['exit_code'] == 0, name
    # Every native production input must match the frozen tree. One early
    # benchmark audit preceded the test-only operator== correction; no engine
    # input differs. Preserve that chronology instead of claiming full identity.
    binaries, audit_differences = {}, {}
    for label, root in [('main',r.BASE),('candidate',r.ROOT),('lto',r.ROOT)]:
        audit = read('audit_common_'+label)
        expected = {i['path']:i['sha256'] for i in (baseline if label=='main' else candidate)['files']}
        recorded = {i['path']:i['sha256'] for i in audit['engine_source']['files']}
        changed = [p for p in sorted(set(expected)|set(recorded)) if expected.get(p)!=recorded.get(p)]
        assert set(changed) <= {'tests/solver_components_unit.cpp'}, (label, changed)
        audit_differences[label] = changed
        binary = root/'build'/('final-common-'+label)/'final_common_benchmark'
        assert sha(binary) == audit['binary_sha256']
        binaries[label] = {'path':str(binary),'sha256':sha(binary),'bytes':binary.stat().st_size}
    # Python launchers use native CMake manifests for their build provenance.
    # Separately verify the actual extension compile flags/LTO and imported .so;
    # do not pretend the launcher/native cache describes extension flags.
    python_identity = {s:read('python_binary_'+s) for s in ('A','B')}
    py_flags = []
    for side, obj in python_identity.items():
        assert sha(Path(obj['path'])) == obj['sha256']
        flags = obj['flags']['CMakeFiles/_csplendor.dir/flags.make']
        flag_line = next(line for line in flags.splitlines() if line.startswith('CXX_FLAGS ='))
        py_flags.append(flag_line)
        assert '-flto=auto' in flag_line and '-ffast-math' not in flag_line
        assert obj['path'] in read('python_identity_'+side)['stdout']
    assert py_flags[0] == py_flags[1]

    measurements, failures = {}, []
    for path in sorted(r.RAW.glob('*.json.gz')):
        name = path.name.removesuffix('.json.gz')
        data = read(name)
        if data.get('exit_code', 0):
            assert name == 'build_candidate', ('Unclassified failure', name)
            failures.append({'artifact':name, 'classification':'FIXED_CANDIDATE_TEST_BUILD',
                             'reason':'New test used unavailable ActionOrderKey operator==; fieldwise equality fixed, full native rerun passed.'})
        if 'comparison' not in data:
            continue
        row = data['comparison'][0]
        assert len(data['pairs']) == (4 if '_smoke_' in name else 22), name
        assert all(v['identical_in_every_pair'] for v in row['counters'].values()
                   if v['classification'] in ('configuration','correctness')), name
        async_mode = row['workload']=='parallel_scheduler' and data['pairs'][0]['A']['records'][0]['semantics']['threads']>1
        if not async_mode:
            assert len({p[s]['records'][0]['digest'] for p in data['pairs'] for s in ('A','B')})==1, name
        ratio = row['B_over_A']
        absolute = {key:val['median'] for key,val in row['absolute'].items()}
        record = {'artifact':str(path.relative_to(r.ROOT)), 'classification':'S1' if async_mode else 'S0',
                  'workload':row['workload'],'fixture':row['fixture'],'pairs':len(data['pairs']),
                  'blocks':ratio['crossover_blocks'],'speedup':ratio['median'],
                  'ci95':ratio['crossover_block_bootstrap_ci95'], 'absolute_medians':absolute,
                  'settings':data['settings'],'binary_sha256':{s:data['manifests'][s]['binary']['sha256'] for s in ('A','B')},
                  'counter_equality_passed':True,
                  'text_busy_retries':sum(p[s]['binary_slot'].get('text_busy_retries_before_timing',0) for p in data['pairs'] for s in ('A','B'))}
        samples = {s:[p[s]['records'][0] for p in data['pairs']] for s in ('A','B')}
        record['semantics_example'] = samples['A'][0]['semantics']
        record['logical_counters_example'] = {s:samples[s][0]['counters'] for s in ('A','B')}
        if row['workload'] in ('parallel_scheduler','root_parallel'):
            record['throughput'] = {}
            for side, rows in samples.items():
                for sample in rows:
                    c, sem = sample['counters'],sample['semantics']
                    assert c['issued']==c['completed']+c['cancelled']+c['failed']==sample['operations']
                    assert c['virtual_loss_added']==c['virtual_loss_released']==c['reservations_committed']+c['reservations_aborted']
                    assert c['integrity_errors']==c['stale_result']==c['duplicate_result']==c['invalid_replay']==0
                    assert sem['ledger_integrity_ok'] and sem['virtual_loss_balanced'] and not sem['partial']
                ratios = [x['counters']['evaluation_owners']/x['counters']['inference_waiters'] for x in rows if x['counters']['inference_waiters']]
                record['throughput'][side] = {
                    **{label:statistics.median(x['counters'][counter]*1e9/x['elapsed_ns'] for x in rows)
                       for label,counter in [('completed_per_second','completed'),('unique_evaluations_per_second','evaluated_boards'),('path_steps_per_second','selected')]},
                    'owner_waiter_ratio_nonzero_waiter_runs':statistics.median(ratios) if ratios else None,
                    'zero_waiter_runs':sum(x['counters']['inference_waiters']==0 for x in rows),
                    'stop_reasons':sorted({x['semantics']['stop_reason'] for x in rows})}
        measurements[name] = record
    for case in list(r.CASES)+['features','pipeline']:
        assert 'code_formal_'+case in measurements, case
    for key in ('code_holdout_primary','code_holdout_features','code_holdout_pipeline',
                'lto_formal_primary','lto_holdout_primary','deployment_formal_primary'):
        assert key in measurements, key
    for case in ('primary','features'):
        for stage in ('formal','holdout'):
            x = measurements['code_'+stage+'_'+case]
            assert x['speedup']>=1.03 and x['ci95'][0]>1, (case,stage)
    regression_flags = [name for name,x in measurements.items()
                        if name.startswith('code_formal_') and x['speedup']<.98]
    confirmed_regressions = []
    for name in regression_flags:
        case = name.removeprefix('code_formal_')
        assert 'code_guard_recheck_'+case in measurements, ('Needs regression recheck',case)
        if (measurements[name]['ci95'][1]<.98 and
                measurements['code_guard_recheck_'+case]['ci95'][1]<.98):
            confirmed_regressions.append(case)

    table = r.ROOT/'doc/performance_experiments/final_main_vs_candidate_20260906.csv'
    with table.open('x',newline='') as stream:
        columns = ['measurement','classification','workload','fixture','pairs','speedup','ci95_low','ci95_high',
                   'A_ms','B_ms','A_rate_per_second','B_rate_per_second','A_current_rss_kib','B_current_rss_kib',
                   'A_runner_peak_rss_kib','B_runner_peak_rss_kib','raw']
        writer = csv.DictWriter(stream,fieldnames=columns,lineterminator='\n')
        writer.writeheader()
        for name,x in measurements.items():
            a = x['absolute_medians']
            writer.writerow({'measurement':name,'classification':x['classification'],'workload':x['workload'],
                'fixture':x['fixture'],'pairs':x['pairs'],'speedup':x['speedup'],
                'ci95_low':x['ci95'][0],'ci95_high':x['ci95'][1],
                'A_ms':a['A_elapsed_ns']/1e6,'B_ms':a['B_elapsed_ns']/1e6,
                **{s+'_rate_per_second':a[s+'_rate_per_second'] for s in ('A','B')},
                **{s+'_current_rss_kib':a.get(s+'_native_rss_kib') for s in ('A','B')},
                **{s+'_runner_peak_rss_kib':a.get(s+'_runner_rss_kib') for s in ('A','B')},
                'raw':x['artifact']})
    original = r.source(r.ROOT.parent/'csplendor')
    start = read('start')['user_workspace']
    assert original==start, 'User tree changed: record external advancement, never restore it'
    manifest = {'schema':'csplendor.final.integration.v1',
        'status':'BLOCKED' if confirmed_regressions else 'READY_FOR_REVIEW',
        'scope':'F0 approved integration + F1 cumulative A/B; no main writes/push/F2-F4',
        'BASE_MAIN_SHA':baseline['head'],'CANDIDATE_SHA':frozen['candidate']['head'],
        'candidate_git_tree':subprocess.check_output(['git','show','-s','--format=%T',frozen['candidate']['head']],cwd=r.ROOT,text=True).strip(),
        'source_digests':{'main':baseline['source_digest'],'candidate':candidate['source_digest']},
        'frozen_source_manifest':'doc/performance_experiments/raw/final_integration_20260906/frozen_candidate.json.gz',
        'merge_parents':subprocess.check_output(['git','show','-s','--format=%P',frozen['candidate']['head']],cwd=r.ROOT,text=True).strip().split(),
        'report_parent_commit':candidate['head'],'candidate_engine_unchanged_since_freeze':True,
        'user_workspace_preserved':True,'binary_identity':binaries,'python_binary_identity':python_identity,
        'early_binary_audit_test_only_differences':audit_differences,
        'python_provenance_contract':'Paired launcher uses native common-cache manifest; real extension identity, version, compile/link flags are independently recorded and verified above. Both retain pybind11 LTO.',
        'phase6_evidence_reuse':'F0 report and Phase 6 common audit retained; no repeated 2162-artifact audit. Parallel C++ files unchanged from Phase 6; prior TSan evidence reused.',
        'integration_regression':{'main_native':33,'candidate_native':44,'main_python':'531 passed, 1 skipped, 4 deselected',
            'candidate_python':'595 passed, 1 skipped, 4 deselected','reference_verify_native_suites':4,
            'reference_verify_python':38,'asan_ubsan_suites':3,'performance_python_each':4},
        'failures':failures,'formal_regression_flags':regression_flags,
        'confirmed_regressions':confirmed_regressions,
        'lto_effect_confirmed_in_both_series':all(measurements['lto_'+stage+'_primary']['ci95'][0]>1 for stage in ('formal','holdout')),
        'lto_default':'OFF; retain pre-existing opt-in, do not treat uncertain repeat as a new adoption result',
        'measurements':measurements,'csv':{'path':str(table.relative_to(r.ROOT)),'sha256':sha(table)},
        'artifacts':[{'path':str(p.relative_to(r.ROOT)),'sha256':sha(p),'bytes':p.stat().st_size} for p in sorted(r.RAW.glob('*.gz'))],
        'limitations':['Fixed-node depth7 UNKNOWN is not a completed seven-move proof',
            'No real NN/Genbu/GUI acceptance (F2 excluded)','Apple/Windows and Python 3.8 runtime not available in this run',
            'Synthetic zero-latency evaluator; no strength claim','Hardware perf N/A inherited, no permissions changed',
            'LTO measured for native primary only; not a new Python LTO gain nor a general MCTS recommendation',
            'No strict 2% noninferiority claim outside measured slices','4A remains deferred; rejected proposals remain rejected'],
        'next_step':'Report and stop for review; no automatic F2/F3/F4/push'}
    target = r.ROOT/'doc/performance_experiments/final_main_vs_candidate_manifest_20260906.json'
    with target.open('x') as stream:
        json.dump(manifest,stream,sort_keys=True,indent=2)
        stream.write('\n')
    print('Verified',len(measurements),'paired measurements;',len(manifest['artifacts']),'artifacts',flush=True)


if __name__ == '__main__':
    main()
