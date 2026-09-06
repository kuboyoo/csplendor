#!/usr/bin/env python3
"""Verify/archive the rejected prototypes and the retained diagnostic-only tree."""
import gzip
import hashlib
import json
import subprocess
from pathlib import Path
from phase3d1_finalize_20260905 import source, cmd

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase3d2'
BASE_ROOT = ROOT.parent / 'csplendor-normal-rollback'
USER_ROOT = ROOT.parent / 'csplendor'
BASE = BASE_ROOT / 'build/3d1-release'
FINAL = ROOT / 'build/3d23-final-release'
BASE_COMMIT = '763a910b8acb2269db2d10364059a5daf2da23ea'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read(name):
    return json.loads(gzip.decompress((RAW / (name + '.json.gz')).read_bytes()))


def save(name, data):
    path = RAW / 'final' / (name + '.gz')
    if not isinstance(data, bytes):
        data = (json.dumps(data, indent=2, sort_keys=True) + '\n').encode()
    with path.open('xb') as stream:
        stream.write(gzip.compress(data, mtime=0))


def finish():
    starting = read('v1/starting_worktrees')
    baseline = source(BASE_ROOT)
    assert baseline == starting['baseline']
    assert source(USER_ROOT) == starting['user_workspace']
    assert baseline['head'] == BASE_COMMIT
    prototypes = {variant: read(variant + '/prototype_sources') for variant in ('v1', 'v2')}
    measurements = {}
    for path in sorted(RAW.rglob('*.json.gz')):
        record = json.loads(gzip.decompress(path.read_bytes()))
        if 'comparison' not in record:
            continue
        key = str(path.relative_to(RAW)).removesuffix('.json.gz')
        comparison = record['comparison'][0]
        checks = [c['identical_in_every_pair'] for c in comparison['counters'].values()
                  if c['classification'] == 'correctness']
        assert checks and all(checks), key
        assert len(record['pairs']) == (4 if '/smoke_' in key else 22), key
        assert record['manifests']['A']['binary']['sha256'] == sha((BASE / 'benchmark_engine_hotpaths').read_bytes())
        variant = key.split('/')[0]
        expected = prototypes[variant]['binary_sha256'] if variant != 'final' else sha((FINAL / 'benchmark_engine_hotpaths').read_bytes())
        assert record['manifests']['B']['binary']['sha256'] == expected, key
        ratio = comparison['B_over_A']
        measurements[key] = {
            'raw': str(path.relative_to(ROOT)), 'pairs': len(record['pairs']),
            'speedup': ratio['median'], 'ci95': ratio['crossover_block_bootstrap_ci95'],
            'ratio_method': ratio['method'], 'digest': comparison['digest'],
            'absolute_medians': {k: v['median'] for k, v in comparison['absolute'].items()},
            'correctness_counters_identical': True,
        }
    for key in ('v1/formal_deep', 'v2/formal_deep', 'v2/holdout_deep'):
        assert measurements[key]['speedup'] < 1.05
    diagnostics = {}
    cases = ('deep', 'warm', 'shallow', 'visible', 'cycle', 'editor', 'proof_on', 'proof_off', 'defender')
    for case in cases:
        groups = []
        for variant in ('v1', 'v2', 'final'):
            prefix = 'retained_' if variant == 'final' else ''
            records = [read(variant + '/' + prefix + label + '_' + case)
                       for label in ('reference-diagnostic', 'diagnostic', 'verify')]
            assert all(r['exit_code'] == 0 for r in records)
            results = [json.loads(r['stdout']) for r in records]
            assert all(r['digest'] == results[0]['digest'] and r['semantics'] == results[0]['semantics'] for r in results)
            groups.append(results)
        assert all(g[0]['digest'] == groups[0][0]['digest'] and g[0]['semantics'] == groups[0][0]['semantics'] for g in groups)
        diagnostics[case] = {'reference': groups[0][0], 'prototype_v2': groups[1][1], 'retained': groups[2][1]}
    required = ['retained_' + name for name in (
        'native_full', 'python_full', 'python_performance', 'python_compile',
        'verify_python_solver', 'asan_native_full')]
    required += ['benchmark_tool_tests']
    required += ['final_preprocessor_unit_' + label for label in ('release', 'diagnostic', 'reference-diagnostic', 'verify')]
    assert all(read('final/' + name)['exit_code'] == 0 for name in required)
    assert read('final/deployment_binary_identity')['identical']
    assert read('final/final_preprocessor_identity')['identical']
    special = ['retained_verify_special_' + fixture + '_' + mode
               for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return')
               for mode in ('false', 'true')]
    assert all(read('final/' + name)['exit_code'] == 0 for name in special)
    purchase_cost = [json.loads(read('final/purchase_cost_' + label + '_five_moves')['stdout'])
                     for label in ('diagnostic', 'verify')]
    assert purchase_cost[0]['digest'] == purchase_cost[1]['digest']
    for result in purchase_cost:
        c = result['counters']
        assert c['solver_visible_purchase_refills'] == 1
        assert c['solver_visible_purchase_generated'] == c['solver_visible_purchase_visited'] == 33
        assert c['solver_visible_purchase_apply_calls'] == c['board_restores'] == 33
    for case in ('deep', 'warm', 'shallow', 'defender'):
        c = diagnostics[case]['retained']['counters']
        assert c['solver_visible_purchase_refills'] == c['solver_visible_purchase_visited'] == c['solver_visible_purchase_visited_1']
        assert c['solver_visible_purchase_visited_2_to_4'] == c['solver_visible_purchase_visited_5_plus'] == 0
    unchanged_objects = {}
    for path in sorted((BASE / 'CMakeFiles/csplendor_core.dir/src').glob('*.o')):
        relative = path.relative_to(BASE)
        old, new = sha(path.read_bytes()), sha((FINAL / relative).read_bytes())
        if path.name != 'perf_counters.cpp.o':
            assert old == new, str(relative)
        unchanged_objects[str(relative)] = {'baseline': old, 'final': new, 'identical': old == new}
    candidate = source(ROOT)
    patch = cmd(['git', 'diff', '--binary', BASE_COMMIT, '--', 'src', 'scripts', 'tests', 'CMakeLists.txt'])['stdout'].encode()
    save('retained_patch.diff', patch)
    save('retained_source.json', candidate)
    save('final_build_provenance.json', {
        'binaries': {str(p): {'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size}
                     for p in (BASE / 'benchmark_engine_hotpaths', FINAL / 'benchmark_engine_hotpaths')},
        'core_objects': unchanged_objects,
        'flags': {str(p): p.read_text() for d in (BASE, FINAL)
                  for target in ('benchmark_engine_hotpaths', '_csplendor')
                  for f in ('flags.make', 'link.txt') for p in [d / ('CMakeFiles/' + target + '.dir/' + f)]},
        'size': cmd(['size', str(BASE / 'benchmark_engine_hotpaths'), str(FINAL / 'benchmark_engine_hotpaths')]),
        'host': [cmd(['uname', '-a']), cmd(['lscpu']), cmd(['c++', '--version'])],
    })
    known_na = ['hardware_perf_check'] + ['purchase_cost_' + label + '_' + fixture
                 for label in ('diagnostic', 'verify') for fixture in ('hidden_reserve', 'reveal_heavy')]
    failures = []
    for path in sorted(RAW.rglob('*.json.gz')):
        record = json.loads(gzip.decompress(path.read_bytes()))
        if record.get('exit_code', 0):
            name = path.name.removesuffix('.json.gz')
            assert name in known_na, 'Unclassified failure: ' + str(path)
            failures.append({'raw': str(path.relative_to(ROOT)), 'classification': 'ENVIRONMENT_UNAVAILABLE' if name == 'hardware_perf_check' else 'NOT_APPLICABLE_NO_VISIBLE_PURCHASE',
                             'reason': record['stderr']})
    artifacts = []
    for path in sorted(RAW.rglob('*.gz')):
        data = path.read_bytes()
        original = gzip.decompress(data)
        artifacts.append({'path': str(path.relative_to(ROOT)), 'sha256': sha(data), 'bytes': len(data),
                          'original_sha256': sha(original), 'original_bytes': len(original)})
    evidence = {
        'schema': 'csplendor.phase3d23.evidence.v1', 'baseline_commit': BASE_COMMIT,
        'baseline_source_digest': baseline['source_digest'], 'candidate_source_digest': candidate['source_digest'],
        'candidate_source_files': candidate['files'], 'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF',
        'decision': {'3D-2': 'REJECT_AND_REVERT', '3D-3': 'REJECT_AND_REVERT'},
        'phase3d3_implementation_attempted': False,
        'phase3d3_scope': 'Cost-model evaluation completed; prefix production implementation withheld because the prerequisite 3D-2 gain failed and all measured deep-search purchase scopes visited one reveal. Root materialization is explicitly different and does visit 33.',
        'retained_change': 'PERF-only purchase generated/visited/apply counters and tests; default solver object is byte-identical to 3D-1.',
        'prior_phases_preserved': 'All adopted 0–3D-1 paths, rollback, 3B sidecar, 3C TT, cache/limit/reference contracts and MCTS unchanged.',
        'protected_worktrees_unchanged': True, 'branch': cmd(['git', 'branch', '--show-current'])['stdout'].strip(),
        'primary': 'exact_reveal hidden_reserve depth7 fixed1M nodes; >=1.05, CI lower>1; independent holdout',
        'prototype_sources': {v: {'source_digest': p['source']['source_digest'], 'binary_sha256': p['binary_sha256'],
                                 'raw': 'doc/performance_experiments/raw/phase3d2/' + v + '/prototype_sources.json.gz'} for v, p in prototypes.items()},
        'measurements': measurements, 'diagnostics': diagnostics, 'root_materialization_cost': purchase_cost[0],
        'required_test_records': required + special, 'environment_and_nonapplicable_records': failures,
        'candidate_specific_engine_failures': [], 'baseline_engine_failures': [],
        'unmeasured': ['Retired prototypes full Python/ASan (performance gate failed first)',
                       'Proof timing CI is not a final acceptance gate for rejected prototypes; native 2000-repeat proof speed gate not run',
                       'No prefix prototype benchmark; no speedup claim for 3D-3', 'Real NN / asynchronous MCTS / TSan not rerun'],
        'next_single_ticket': '4A-1: remove write-only ConcurrentTree metadata after read/write audit; no implementation in this task',
        'artifacts': artifacts,
    }
    destination = ROOT / 'doc/performance_experiments/phase3d23_evidence_20260905.json'
    with destination.open('x') as stream:
        json.dump(evidence, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print(json.dumps({'source_digest': candidate['source_digest'], 'artifacts': len(artifacts), 'measurement_series': len(measurements), 'decisions': evidence['decision']}, ensure_ascii=False))


if __name__ == '__main__':
    finish()
