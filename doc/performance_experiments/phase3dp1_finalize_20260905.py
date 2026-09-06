#!/usr/bin/env python3
"""Validate recorded gates and assemble the Phase3D-P1 evidence manifest."""
import csv
import gzip
import hashlib
import json
import subprocess
from pathlib import Path

from phase3dp1_record_20260905 import ROOT, RAW, BASE, BASE_SOURCE, RELEASE, CASES, save


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def native(name):
    record = read(name)
    assert record['exit_code'] == 0, name
    return json.loads(record['stdout'])


def cmd(args, cwd=ROOT):
    done = subprocess.run(args, cwd=cwd, capture_output=True, text=True, check=True)
    return {'command': args, 'cwd': str(cwd), 'stdout': done.stdout, 'stderr': done.stderr}


def main():
    r0 = json.loads((ROOT / 'doc/performance_experiments/post3c_review_baseline_20260905.json').read_text())
    preserved = {}
    for name, root, fields in [
        ('original_post3c', BASE_SOURCE, r0['baseline']['files']),
        ('user_workspace', Path('/home/kuboyu/workspace/repos/csplendor'), r0['user_workspace_at_start']['files']),
    ]:
        for item in fields:
            assert sha((root / item['path']).read_bytes()) == item['sha256'], (name, item['path'])
        preserved[name] = {'source_files_unchanged': len(fields)}

    measurements = {}
    for name in [*('formal_' + case for case in CASES), 'holdout_deep', 'holdout_proof_off',
                 'python_proof_batch_off', 'python_proof_batch_on']:
        record = read(name)
        comparison = record['comparison'][0]
        assert len(record['pairs']) == 22, name
        assert all(c['identical_in_every_pair'] for c in comparison['counters'].values()
                   if c['classification'] == 'correctness'), name
        measurements[name] = {'raw': 'raw/phase3dp1/' + name + '.json.gz',
                              'comparison': comparison, 'settings': record['settings']}
    for name in ('formal_deep', 'holdout_deep'):
        ratio = measurements[name]['comparison']['B_over_A']
        assert ratio['median'] > 1.03 and ratio['crossover_block_bootstrap_ci95'][0] > 1
    assert read('deployment_benchmark_identity_v2')['identical']

    diagnostics = {}
    for case in CASES:
        old = native('reference-diagnostic_' + case)
        new = native('diagnostic_' + case)
        verified = native('verify_' + case)
        assert old['digest'] == new['digest'] == verified['digest'], case
        assert old['semantics'] == new['semantics'] == verified['semantics'], case
        for key in ('nodes', 'memo_hits', 'terminal_nodes', 'legal_moves', 'memoized_states',
                    'persistent_memo_hits', 'deck_reserve_candidates', 'deck_reserve_branches'):
            assert old['counters'].get(key) == new['counters'].get(key) == verified['counters'].get(key), (case, key)
        c = new['counters']
        assert c['solver_visible_refill_score_calls'] == c['solver_visible_refill_sort_candidates']
        assert c['solver_defender_reserve_score_calls'] == c['solver_defender_reserve_sort_candidates']
        diagnostics[case] = {'reference': old, 'candidate': new, 'verify_digest': verified['digest']}

    required = ['native_full_v2', 'python_full_v2', 'python_performance_v2', 'python_compile_v2',
                'verify_python_solver', 'asan_native_full', 'benchmark_tool_tests',
                'mechanism_diagnostic', 'mechanism_reference-diagnostic']
    assert all(read(name)['exit_code'] == 0 for name in required)
    special = ['verify_special_' + fixture + '_' + mode
               for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return')
               for mode in ('false', 'true')]
    assert all(read(name)['exit_code'] == 0 for name in special)

    source_names = cmd(['git', 'ls-files', '-co', '--exclude-standard'])['stdout'].splitlines()
    names = sorted({name for name in source_names if name == 'CMakeLists.txt' or
                    (Path(name).parts[0] in {'src', 'scripts', 'tests', 'csplendor'} and
                     Path(name).suffix in {'.h', '.cpp', '.py', '.txt', '.json'})})
    files = [{'path': name, 'sha256': sha((ROOT / name).read_bytes())} for name in names]
    source_digest = sha(''.join(f"{item['sha256']}  {item['path']}\n" for item in files).encode())
    patch = cmd(['git', 'diff', '--binary', '86b7473', '--', 'src', 'scripts', 'tests', 'CMakeLists.txt'])['stdout'].encode()
    save('candidate_tracked_patch.diff', patch)
    save('candidate_new_header.h', (ROOT / 'src/solver_reveal_order.h').read_bytes())

    binaries = [BASE / 'benchmark_engine_hotpaths', RELEASE / 'benchmark_engine_hotpaths',
                *sorted((ROOT / 'csplendor').glob('*312*.so')),
                *sorted(Path('/tmp/csplendor-r0-python/csplendor').glob('*312*.so'))]
    flags = [BASE / 'CMakeFiles/benchmark_engine_hotpaths.dir/flags.make',
             RELEASE / 'CMakeFiles/benchmark_engine_hotpaths.dir/flags.make',
             RELEASE / 'CMakeFiles/_csplendor.dir/flags.make',
             RELEASE / 'CMakeFiles/_csplendor.dir/link.txt',
             Path('/tmp/csplendor-r0-python-build/CMakeFiles/_csplendor.dir/flags.make'),
             Path('/tmp/csplendor-r0-python-build/CMakeFiles/_csplendor.dir/link.txt')]
    provenance = {'binaries': {str(path): {'sha256': sha(path.read_bytes()), 'bytes': path.stat().st_size}
                              for path in binaries},
                  'flags': {str(path): path.read_text() for path in flags},
                  'size': cmd(['size', *map(str, binaries[:2])]),
                  'host': [cmd(['uname', '-a']), cmd(['lscpu']), cmd(['c++', '--version'])]}
    save('final_build_provenance.json', provenance)
    evidence = {
        'schema': 'csplendor.phase3dp1.evidence.v1', 'decision': 'ACCEPT',
        'baseline_commit': '35048e7', 'starting_commit_including_R0': '86b7473',
        'baseline_source_digest': r0['baseline']['source_digest'],
        'candidate_source_digest': source_digest,
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF',
        'candidate_source_files': files, 'tracked_patch_sha256': sha(patch),
        'original_worktrees': preserved,
        'scope': '3D-P1 only: pure action-local integer score-once sorting; no TT/rollback/global-cache/numerical reorder',
        'production_option': 'CSPLENDOR_CACHE_REVEAL_SCORES=ON',
        'oracle_option': 'CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON; excluded from timing builds',
        'reference_option': 'CSPLENDOR_CACHE_REVEAL_SCORES=OFF; formal reference is original post3C binary',
        'scratch': {'fixed_capacity': 40, 'fixed_payload_bytes': 320,
                    'large_input_fallback': 'local vector; checked before writes', 'recursive_shared_state': False},
        'primary': 'formal_deep and independent holdout_deep: hidden_reserve d7 fixed1M nodes',
        'measurements': measurements, 'diagnostics': diagnostics,
        'gate_results': {name: read(name) for name in required},
        'additional_verify_cases': special,
        'proof_guard_limit': 'Native37-node ~50us off measurement remains noisy: both series retained. Supplemental2000-call existing _sample includes Python binding and equality, not native-only time. Launcher inodes rotate; shared-library inodes do not. Native proof-on guard and all proof semantics pass.',
        'failure_classification': {
            'candidate_failure_fixed': 'Internal header initially added to public matrix; Python test detected it. Moved to separate internal standalone target, full suite passed.',
            'setup_failures_fixed': ['New make target required CMake regeneration',
                'Supplemental ELF adapter required its own CMake build provenance',
                'Supplemental float seconds to integer ns conversion required a consistent derived rate'],
            'baseline_failures_observed': [],
            'environment_not_available': ['hardware perf: prior R0 perf_event_paranoid=4, not changed',
                                          'generated/mate_puzzles2 absent: one optional Python test skipped'],
            'remaining_candidate_test_failures': 0,
        },
        'not_claimed': ['guaranteed mate at benchmark depth7 (node-limited UNKNOWN)',
                        '11x engine speedup', 'NN MCTS speedup', 'new deeper mate capability guarantee',
                        'precise native-only speed for the tiny proof-off fixture', 'TSan rerun (no parallel code changes)'],
        'next_phase_started': False,
    }
    artifacts = []
    for path in sorted(RAW.glob('*.gz')):
        data = path.read_bytes()
        original = gzip.decompress(data)
        artifacts.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(data), 'bytes': len(data),
                          'original_sha256': sha(original), 'original_bytes': len(original)})
    evidence['artifacts'] = artifacts
    report_dir = ROOT / 'doc/performance_experiments'
    with (report_dir / 'phase3dp1_evidence_20260905.json').open('x') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write('\n')
    with (report_dir / 'phase3dp1_paired_20260905.csv').open('x') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(['case', 'A_rate_median', 'B_rate_median', 'paired_block_speedup', 'ci95_low', 'ci95_high'])
        for name, data in measurements.items():
            c = data['comparison']; r = c['B_over_A']; a = c['absolute']
            writer.writerow([name, a['A_rate_per_second']['median'], a['B_rate_per_second']['median'],
                             r['median'], *r['crossover_block_bootstrap_ci95']])
    manifest = ROOT / 'doc/performance_experiments/raw/manifest.tsv'
    assert '\nphase3dp1\t' not in manifest.read_text()
    with manifest.open('a') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
        for a in artifacts:
            original_path = a['path'][:-3]
            writer.writerow(['phase3dp1', Path(original_path).name, original_path, a['original_bytes'],
                             a['original_sha256'], a['path'], a['bytes'], a['sha256']])
    print('Validated gates and saved', len(artifacts), 'raw artifacts; source', source_digest)


if __name__ == '__main__':
    main()
