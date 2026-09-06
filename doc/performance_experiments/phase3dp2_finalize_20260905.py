#!/usr/bin/env python3
"""Assemble permanent 3D-P2 provenance and validate the existing harness output."""
import argparse
import csv
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3dp2_record_20260905 import ROOT, RAW, BASE, BASE_SOURCE, RELEASE, CASES, save

BASE_COMMIT = 'ff8b1f646b91ae094e47fc6898fff214f9e596ee'
USER_ROOT = Path('/home/kuboyu/workspace/repos/csplendor')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def cmd(args, root=ROOT):
    done = subprocess.run(args, cwd=root, capture_output=True, text=True, check=True)
    return {'command': args, 'cwd': str(root), 'stdout': done.stdout, 'stderr': done.stderr}


def source(root):
    names = cmd(['git', 'ls-files', '-co', '--exclude-standard'], root)['stdout'].splitlines()
    names = sorted({name for name in names if name == 'CMakeLists.txt' or
                    (Path(name).parts[0] in {'src', 'scripts', 'tests', 'csplendor'} and
                     Path(name).suffix in {'.h', '.cpp', '.py', '.txt', '.json'})})
    files = [{'path': name, 'sha256': sha((root / name).read_bytes())} for name in names]
    return {'files': files, 'source_digest': sha(''.join(f"{i['sha256']}  {i['path']}\n" for i in files).encode()),
            'head': cmd(['git', 'rev-parse', 'HEAD'], root)['stdout'].strip(),
            'status': cmd(['git', 'status', '--short', '--branch'], root)['stdout']}


def start():
    baseline = source(BASE_SOURCE)
    assert baseline['head'] == BASE_COMMIT
    previous = json.loads((ROOT / 'doc/performance_experiments/phase3dp1_evidence_20260905.json').read_text())
    assert baseline['source_digest'] == previous['candidate_source_digest']
    save('starting_worktrees.json', {'baseline': baseline, 'user_workspace': source(USER_ROOT)})


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def finish():
    starting = read('starting_worktrees')
    assert source(BASE_SOURCE) == starting['baseline']
    assert source(USER_ROOT) == starting['user_workspace']
    measurements = {}
    for path in sorted(RAW.glob('*.json.gz')):
        record = json.loads(gzip.decompress(path.read_bytes()))
        if not isinstance(record, dict) or 'comparison' not in record:
            continue
        name = path.name.removesuffix('.json.gz')
        if name.startswith('smoke'):
            continue
        comparison = record['comparison'][0]
        assert len(record['pairs']) == 22, name
        assert all(c['identical_in_every_pair'] for c in comparison['counters'].values()
                   if c['classification'] == 'correctness'), name
        if name.startswith(('formal_', 'holdout_')):
            assert record['manifests']['A']['binary']['sha256'] == sha((BASE / 'benchmark_engine_hotpaths').read_bytes()), name
            assert record['manifests']['B']['binary']['sha256'] == sha((RELEASE / 'benchmark_engine_hotpaths').read_bytes()), name
        measurements[name] = {'raw': str(path.relative_to(ROOT)), 'comparison': comparison,
                              'settings': record['settings']}
    for name in ('formal_deep', 'holdout_deep'):
        ratio = measurements[name]['comparison']['B_over_A']
        assert ratio['median'] >= 1.05 and ratio['crossover_block_bootstrap_ci95'][0] > 1, name
    for name in ('native_proof_batch_on', 'native_proof_batch_off', 'python_proof_batch_on', 'python_proof_batch_off'):
        assert measurements[name]['comparison']['B_over_A']['crossover_block_bootstrap_ci95'][0] >= 0.98, name
    diagnostics = {}
    for case in CASES:
        records = [read(label + '_' + case) for label in ('reference-diagnostic', 'diagnostic', 'verify')]
        assert all(r['exit_code'] == 0 for r in records), case
        old, new, verified = [json.loads(r['stdout']) for r in records]
        assert old['digest'] == new['digest'] == verified['digest'], case
        assert old['semantics'] == new['semantics'] == verified['semantics'], case
        for key in ('nodes', 'memo_hits', 'terminal_nodes', 'legal_moves', 'memoized_states',
                    'persistent_memo_hits', 'deck_reserve_candidates', 'deck_reserve_branches'):
            assert old['counters'].get(key) == new['counters'].get(key) == verified['counters'].get(key), (case, key)
        diagnostics[case] = {'reference': old, 'candidate': new, 'verify_digest': verified['digest']}
    required = ['native_full', 'python_full', 'python_performance', 'python_compile',
                'verify_python_solver', 'asan_native_full', 'benchmark_tool_tests',
                'mechanism_diagnostic', 'mechanism_reference-diagnostic']
    assert all(read(name)['exit_code'] == 0 for name in required)
    assert read('deployment_benchmark_identity')['identical']
    special = ['verify_special_' + fixture + '_' + mode
               for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return')
               for mode in ('false', 'true')]
    assert all(read(name)['exit_code'] == 0 for name in special)
    candidate = source(ROOT)
    patch = cmd(['git', 'diff', '--binary', BASE_COMMIT, '--', 'src', 'scripts', 'tests', 'CMakeLists.txt'])['stdout'].encode()
    save('candidate_tracked_patch.diff', patch)
    save('candidate_new_header.h', (ROOT / 'src/solver_search_scratch.h').read_bytes())
    binaries = [BASE / 'benchmark_engine_hotpaths', RELEASE / 'benchmark_engine_hotpaths',
                *sorted((BASE_SOURCE / 'csplendor').glob('*312*.so')),
                *sorted((ROOT / 'csplendor').glob('*312*.so'))]
    supplementary = [ROOT / 'build/3dp2-proof-driver/proof_launcher_A',
                     ROOT / 'build/3dp2-proof-driver/proof_launcher_B']
    for side, production in (('A', BASE), ('B', RELEASE)):
        directory = ROOT / ('build/3dp2-native-proof-' + side)
        assert sha((directory / 'engine/libcsplendor_core.a').read_bytes()) == sha((production / 'libcsplendor_core.a').read_bytes())
        supplementary.extend([directory / 'native_proof_batch', directory / 'engine/libcsplendor_core.a'])
    binaries.extend(supplementary)
    flags = [directory / ('CMakeFiles/' + target + '.dir/' + file)
             for directory in (BASE, RELEASE) for target in ('benchmark_engine_hotpaths', '_csplendor')
             for file in ('flags.make', 'link.txt')]
    flags.extend(ROOT / ('build/3dp2-native-proof-' + side) / ('CMakeFiles/native_proof_batch.dir/' + file)
                 for side in ('A', 'B') for file in ('flags.make', 'link.txt'))
    save('final_build_provenance.json', {
        'binaries': {str(p): {'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size} for p in binaries},
        'flags': {str(p): p.read_text() for p in flags}, 'size': cmd(['size', *map(str, binaries[:2])]),
        'host': [cmd(['uname', '-a']), cmd(['lscpu']), cmd(['c++', '--version'])]})
    artifacts = []
    for path in sorted(RAW.glob('*.gz')):
        data = path.read_bytes()
        original = gzip.decompress(data)
        artifacts.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(data), 'bytes': len(data),
                          'original_sha256': sha(original), 'original_bytes': len(original)})
    evidence = {
        'schema': 'csplendor.phase3dp2.evidence.v1', 'decision': 'ACCEPT',
        'baseline_commit': BASE_COMMIT, 'baseline_source_digest': starting['baseline']['source_digest'],
        'candidate_source_digest': candidate['source_digest'], 'candidate_source_files': candidate['files'],
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF',
        'tracked_patch_sha256': sha(patch), 'starting_worktrees_unchanged': True,
        'scope': '3D-P2 only: invocation-local vectors, no rollback/TT/MCTS/score formula changes',
        'production_option': 'CSPLENDOR_REUSE_SEARCH_SCRATCH=ON',
        'reference_option': 'CSPLENDOR_REUSE_SEARCH_SCRATCH=OFF; formal reference is unchanged 3D-P1 binary',
        'primary': 'hidden_reserve d7 fixed1M nodes; formal and independent holdout',
        'measurements': measurements, 'diagnostics': diagnostics,
        'gate_results': {name: read(name) for name in required}, 'additional_verify_cases': special,
        'scratch_contract': {
            'ownership': 'solver-owned vector<unique_ptr<Frame>>, RAII LIFO leases by live invocation, unbounded growth',
            'frame_payload_bytes': 48, 'ordered_action_bytes': 40, 'shared_across_solver_copies': False,
            'metric_scope': 'retained payload at search boundary, not allocator high-water; pointer slots included',
            'proof_frontier_outputs': 'caller-owned, no scratch references escape',
            'score_scratch': 'existing P1 bounded 40 * 8B local array unchanged',
            'unused_paths': 'visible-only solver, final-round/local proof action vectors and legacy filtering outputs unchanged'},
        'proof_guard_limit': 'Native single-call proof-on formal 0.9758 and holdout 0.9708 retained (CI overlaps 0.98). Supplement repeated2000 native timers on identical static core libraries: off1.0086/on1.0243, both lower CI>0.98. Python API repeat also passes. Adapter code layout differs; single-call cold overhead is not proven absent.',
        'failure_classification': {
            'baseline_failures_observed': [], 'remaining_candidate_test_failures': 0,
            'environment_not_available': ['hardware perf: perf_event_paranoid=4, unchanged',
                                          'generated/mate_puzzles2 absent: optional Python test skipped']},
        'not_claimed': ['MCTS/NN speedup', 'depth7 benchmark solves a mate (node-limited UNKNOWN)',
                        'multiplication with previous phase speedups', 'TSan rerun (no parallel code changes)'],
        'next_phase_started': False, 'artifacts': artifacts,
    }
    report_dir = ROOT / 'doc/performance_experiments'
    with (report_dir / 'phase3dp2_evidence_20260905.json').open('x') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write('\n')
    with (report_dir / 'phase3dp2_paired_20260905.csv').open('x') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(['case', 'A_rate_median', 'B_rate_median', 'paired_block_speedup', 'ci95_low', 'ci95_high'])
        for name, data in measurements.items():
            c = data['comparison']; r = c['B_over_A']; a = c['absolute']
            writer.writerow([name, a['A_rate_per_second']['median'], a['B_rate_per_second']['median'],
                             r['median'], *r['crossover_block_bootstrap_ci95']])
    manifest = ROOT / 'doc/performance_experiments/raw/manifest.tsv'
    assert '\nphase3dp2\t' not in manifest.read_text()
    with manifest.open('a') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
        for a in artifacts:
            original_path = a['path'][:-3]
            writer.writerow(['phase3dp2', Path(original_path).name, original_path, a['original_bytes'],
                             a['original_sha256'], a['path'], a['bytes'], a['sha256']])
    print('Validated gates; artifacts', len(artifacts), 'source', candidate['source_digest'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['start', 'finish'])
    args = parser.parse_args()
    {'start': start, 'finish': finish}[args.stage]()
