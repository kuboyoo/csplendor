#!/usr/bin/env python3
"""Assemble permanent 3D-1 provenance and validate the existing harness output."""
import argparse
import csv
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3d1_record_20260905 import ROOT, RAW, BASE, BASE_SOURCE, RELEASE, CASES, save

BASE_COMMIT = 'ef312a15f451d566c428ce7b955a788ede6c533c'
USER_ROOT = Path('/home/kuboyu/workspace/repos/csplendor')
FINAL_PREFIX = 'candidate_v3/'


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
    previous = json.loads((ROOT / 'doc/performance_experiments/phase3dp2_evidence_20260905.json').read_text())
    assert baseline['source_digest'] == previous['candidate_source_digest']
    save('starting_worktrees.json', {'baseline': baseline, 'user_workspace': source(USER_ROOT)})


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def finish():
    starting = read('starting_worktrees')
    assert source(BASE_SOURCE) == starting['baseline']
    assert source(USER_ROOT) == starting['user_workspace']
    rejected = {'v1': read('prototype_v1_sources'), 'v2': read('prototype_v2_sources')}
    measurements = {}
    for path in sorted(RAW.rglob('*.json.gz')):
        record = json.loads(gzip.decompress(path.read_bytes()))
        if not isinstance(record, dict) or 'comparison' not in record:
            continue
        name = str(path.relative_to(RAW)).removesuffix('.json.gz')
        case_name = path.name.removesuffix('.json.gz')
        selected = name.startswith(FINAL_PREFIX)
        if case_name.startswith('smoke'):
            continue
        comparison = record['comparison'][0]
        assert len(record['pairs']) == (66 if case_name.startswith('native_holdout_') else 22), name
        assert all(c['identical_in_every_pair'] for c in comparison['counters'].values()
                   if c['classification'] == 'correctness'), name
        if case_name.startswith(('formal_', 'holdout_')):
            assert record['manifests']['A']['binary']['sha256'] == sha((BASE / 'benchmark_engine_hotpaths').read_bytes()), name
            old_variant = 'v2' if name.startswith('candidate_v2/') else 'v1'
            expected = sha((RELEASE / 'benchmark_engine_hotpaths').read_bytes()) if selected else rejected[old_variant]['binary_sha256']
            assert record['manifests']['B']['binary']['sha256'] == expected, name
        measurements[name] = {'raw': str(path.relative_to(ROOT)), 'comparison': comparison,
                              'settings': record['settings'], 'variant': 'selected_v3' if selected else ('rejected_v2' if name.startswith('candidate_v2/') else 'rejected_v1')}
    for name in ('formal_visible', 'holdout_visible'):
        ratio = measurements[FINAL_PREFIX + name]['comparison']['B_over_A']
        assert ratio['median'] >= 1.05 and ratio['crossover_block_bootstrap_ci95'][0] > 1, name
    proof_gates = []
    for proof in ('on', 'off'):
        name = FINAL_PREFIX + 'native_holdout_proof_batch_' + proof
        if name not in measurements:
            name = FINAL_PREFIX + 'native_proof_batch_' + proof
        assert measurements[name]['comparison']['B_over_A']['crossover_block_bootstrap_ci95'][0] >= 0.98, name
        proof_gates.append(name)
    diagnostics = {}
    for case in CASES:
        records = [read(FINAL_PREFIX + label + '_' + case) for label in ('reference-diagnostic', 'diagnostic', 'verify')]
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
                'mechanism_diagnostic', 'mechanism_reference-diagnostic', 'rollback_unit_verify',
                'final_unit_release', 'final_unit_verify', 'final_unit_asan', 'orchestration_compile']
    required = [FINAL_PREFIX + name for name in required]
    assert all(read(name)['exit_code'] == 0 for name in required)
    assert read(FINAL_PREFIX + 'deployment_benchmark_identity')['identical']
    assert read(FINAL_PREFIX + 'final_unit_binary_identity')['identical']
    special = [FINAL_PREFIX + 'verify_special_' + fixture + '_' + mode
               for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return')
               for mode in ('false', 'true')]
    assert all(read(name)['exit_code'] == 0 for name in special)
    candidate = source(ROOT)
    patch = cmd(['git', 'diff', '--binary', BASE_COMMIT, '--', 'src', 'scripts', 'tests', 'CMakeLists.txt'])['stdout'].encode()
    save('candidate_tracked_patch.diff', patch)
    save('candidate_new_header.h', (ROOT / 'src/solver_normal_rollback.h').read_bytes())
    save('candidate_fault_header.h', (ROOT / 'src/solver_rollback_faults.h').read_bytes())
    binaries = [BASE / 'benchmark_engine_hotpaths', RELEASE / 'benchmark_engine_hotpaths',
                *sorted((BASE_SOURCE / 'csplendor').glob('*312*.so')),
                *sorted((ROOT / 'csplendor').glob('*312*.so'))]
    supplementary = []
    for side, production in (('A', BASE), ('B', RELEASE)):
        directory = ROOT / ('build/3d1-native-proof-' + side)
        assert sha((directory / 'engine/libcsplendor_core.a').read_bytes()) == sha((production / 'libcsplendor_core.a').read_bytes())
        supplementary.extend([directory / 'native_proof_batch', directory / 'engine/libcsplendor_core.a'])
    binaries.extend(supplementary)
    flags = [directory / ('CMakeFiles/' + target + '.dir/' + file)
             for directory in (BASE, RELEASE) for target in ('benchmark_engine_hotpaths', '_csplendor')
             for file in ('flags.make', 'link.txt')]
    flags.extend(ROOT / ('build/3d1-native-proof-' + side) / ('CMakeFiles/native_proof_batch.dir/' + file)
                 for side in ('A', 'B') for file in ('flags.make', 'link.txt'))
    reveal_objects = [directory / 'CMakeFiles/csplendor_core.dir/src/reveal_verified_solver.cpp.o'
                      for directory in (BASE, RELEASE)]
    reveal_object_hashes = {str(path): sha(path.read_bytes()) for path in reveal_objects}
    assert len(set(reveal_object_hashes.values())) == 1, 'Retained reveal implementation changed'
    assert (ROOT / 'src/reveal_verified_solver.cpp').read_bytes() == (BASE_SOURCE / 'src/reveal_verified_solver.cpp').read_bytes()
    save('final_build_provenance.json', {
        'binaries': {str(p): {'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size} for p in binaries},
        'flags': {str(p): p.read_text() for p in flags}, 'size': cmd(['size', *map(str, binaries[:2])]),
        'host': [cmd(['uname', '-a']), cmd(['lscpu']), cmd(['c++', '--version'])],
        'reveal_object_identical': True, 'reveal_object_sha256': reveal_object_hashes})
    artifacts = []
    for path in sorted(RAW.rglob('*.gz')):
        data = path.read_bytes()
        original = gzip.decompress(data)
        artifacts.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(data), 'bytes': len(data),
                          'original_sha256': sha(original), 'original_bytes': len(original)})
    evidence = {
        'schema': 'csplendor.phase3d1.evidence.v1', 'decision': 'ACCEPT',
        'baseline_commit': BASE_COMMIT, 'baseline_source_digest': starting['baseline']['source_digest'],
        'candidate_source_digest': candidate['source_digest'], 'candidate_source_files': candidate['files'],
        'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF',
        'tracked_patch_sha256': sha(patch), 'starting_worktrees_unchanged': True,
        'scope': '3D-1 only: visible-only ordinary solver rollback. Reveal solver source is unchanged from 3D-P2; its proposed compact rollback was rejected. No TT/MCTS changes.',
        'production_option': 'CSPLENDOR_SOLVER_NORMAL_ROLLBACK=ON',
        'reference_option': 'CSPLENDOR_SOLVER_NORMAL_ROLLBACK=OFF; formal reference is unchanged 3D-P2 binary',
        'primary': 'visible_solver five_moves fixed100k nodes; formal and independent holdout',
        'rejected_prototypes': {variant: {'decision': data['decision'], 'source_digest': data['source']['source_digest'],
                                'binary_sha256': data['binary_sha256'], 'raw': 'raw/phase3d1/prototype_' + variant + '_sources.json.gz'}
                                for variant, data in rejected.items()},
        'proof_regression_gate_series': proof_gates,
        'measurements': measurements, 'diagnostics': diagnostics,
        'gate_results': {name: read(name) for name in required}, 'additional_verify_cases': special,
        'rollback_contract': {
            'record_bytes': 128, 'guard_bytes_release': 416,
            'fields': 'see phase3d1_plan_20260905.md complete field table',
            'restoration': 'append-only lengths, scalar/arrays, deck counts+old tops, raw hash cache and modes. Reveal sidecar remains in existing full Board guard, untouched.',
            'fallback': 'invalid visible Editor invariants use full Board; reveal solver keeps the existing full snapshot on every branch',
            'verification': 'child/parent field oracle plus real transition fault hooks in VERIFY only',
            'excluded': 'all reveal-solver compact integration (rejected), Game undo, TT, P2 scratch'},
        'proof_guard_limit': 'Native single-call microseconds are noisy; both formal/holdout retained. Supplemental existing native timer aggregation uses identical core static libraries and its own driver code layout. Final independent 66-pair proof on/off CI lower bounds exceed 0.98; v1/v2 missed the same gate and were rejected.',
        'failure_classification': {
            'baseline_failures_observed': [], 'remaining_candidate_test_failures': 0,
            'fixed_fixture_failure': 'Initial test used 30 tokens and exhausted the action cap before purchase; corrected to a <=10-token reserved purchase and reran VERIFY (original log retained).',
            'fixed_candidate_failure': 'Python extension missed the newly-used state_invariants.cpp dependency; added it to shared core sources and reran deployment full suite, keeping native benchmark byte-identical.',
            'environment_not_available': ['hardware perf: perf_event_paranoid=4, unchanged',
                                          'generated/mate_puzzles2 absent: optional Python test skipped']},
        'not_claimed': ['MCTS/NN speedup', 'rejected v1/v2 warm/deep gains as selected results', 'depth7 benchmark solves a mate (node-limited UNKNOWN)',
                        'multiplication with previous phase speedups', 'TSan rerun (no parallel code changes)'],
        'next_phase_started': False, 'artifacts': artifacts,
    }
    report_dir = ROOT / 'doc/performance_experiments'
    with (report_dir / 'phase3d1_evidence_20260905.json').open('x') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write('\n')
    with (report_dir / 'phase3d1_paired_20260905.csv').open('x') as stream:
        writer = csv.writer(stream, lineterminator='\n')
        writer.writerow(['case', 'A_rate_median', 'B_rate_median', 'paired_block_speedup', 'ci95_low', 'ci95_high'])
        for name, data in measurements.items():
            c = data['comparison']; r = c['B_over_A']; a = c['absolute']
            writer.writerow([name, a['A_rate_per_second']['median'], a['B_rate_per_second']['median'],
                             r['median'], *r['crossover_block_bootstrap_ci95']])
    manifest = ROOT / 'doc/performance_experiments/raw/manifest.tsv'
    assert '\nphase3d1\t' not in manifest.read_text()
    with manifest.open('a') as stream:
        writer = csv.writer(stream, delimiter='\t', lineterminator='\n')
        for a in artifacts:
            original_path = a['path'][:-3]
            writer.writerow(['phase3d1', Path(original_path).name, original_path, a['original_bytes'],
                             a['original_sha256'], a['path'], a['bytes'], a['sha256']])
    print('Validated gates; artifacts', len(artifacts), 'source', candidate['source_digest'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['start', 'finish'])
    args = parser.parse_args()
    {'start': start, 'finish': finish}[args.stage]()
