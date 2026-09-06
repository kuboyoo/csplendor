#!/usr/bin/env python3
"""Append-only records for the approved five-test lint correction."""
import ast
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import f2_f3_record_20260906 as prior

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/f3_lint_20260906'
BASE = 'e486e27cbabdec4387f9d183a758720e1cf2caee'
TESTS = ['tests/test_' + name + '.py' for name in (
    'build_profiles', 'compact_phase0_evidence', 'return_rank_selection',
    'state_feature_numpy', 'v3_payment_codec')]
RELATED = ['tests/test_' + name + '.py' for name in (
    'engine_benchmark_tools', 'ml', 'encoders', 'encoding_schema')]
PYTHON = str(ROOT / 'build/f2-env/bin/python')
LINT = ['python', '-m', 'ruff', 'check', '--target-version', 'py38',
        '--select', 'E4,E7,E9,F,W,I', 'csplendor', 'tests']
prior.RAW = RAW
save = prior.save


def run(name, command):
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1', PYTHONPATH=str(ROOT),
               PYTHONPYCACHEPREFIX=str(ROOT / 'build/f3-lint-bytecode'),
               RUFF_CACHE_DIR=str(ROOT / 'build/f3-lint-ruff-cache'),
               COVERAGE_FILE=str(ROOT / 'build/f3-lint-coverage'))
    start = time.monotonic()
    done = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True, timeout=300)
    save(name, {'command': command, 'cwd': str(ROOT), 'exit_code': done.returncode,
                'elapsed_seconds': time.monotonic()-start, 'stdout': done.stdout, 'stderr': done.stderr})
    print(name, done.returncode, done.stdout[-8000:], done.stderr[-2000:], flush=True)
    return done


def read(name):
    return json.loads(gzip.decompress((RAW / (name+'.json.gz')).read_bytes()))


def invariants():
    result = {}
    for name in TESTS:
        before = subprocess.check_output(['git', 'show', BASE+':'+name], cwd=ROOT, text=True)
        after = (ROOT / name).read_text()
        old, new = ast.parse(before), ast.parse(after)
        # Every function/class, decorator, assert, parameterization, fixture
        # helper and skip marker remains exactly the same AST.
        def definitions(tree):
            return [ast.dump(node, include_attributes=False) for node in tree.body
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef))]
        assert definitions(old) == definitions(new), name
        if name != TESTS[0]:
            def imports_and_rest(tree):
                imports, rest = [], []
                for node in tree.body:
                    if isinstance(node, ast.ImportFrom):
                        imports.extend(('from', node.level, node.module, item.name, item.asname)
                                       for item in node.names)
                    elif isinstance(node, ast.Import):
                        imports.extend(('import', item.name, item.asname) for item in node.names)
                    else:
                        rest.append(ast.dump(node, include_attributes=False))
                return sorted(imports, key=repr), rest
            assert imports_and_rest(old) == imports_and_rest(new), name
        else:
            # Only the two imports become standard import_module assignments;
            # preserve sys.path setup and every other non-import statement.
            def rest(tree, updated):
                nodes = []
                for node in tree.body:
                    if isinstance(node, (ast.Import, ast.ImportFrom)):
                        continue
                    if updated and isinstance(node, ast.Assign) and any(
                        isinstance(t, ast.Name) and t.id in {'manifest', 'runner'} for t in node.targets):
                        target = node.targets[0].id
                        expected = {'manifest': 'benchmark_manifest', 'runner': 'run_paired_benchmarks'}[target]
                        assert ast.dump(node.value) == ast.dump(ast.parse(
                            'importlib.import_module('+repr(expected)+')', mode='eval').body)
                        continue
                    nodes.append(ast.dump(node, include_attributes=False))
                return nodes
            assert rest(old, False) == rest(new, True)
        assert after.count('noqa') == before.count('noqa') == 0
        result[name] = {'before_sha256': hashlib.sha256(before.encode()).hexdigest(),
                       'after_sha256': prior.sha(ROOT / name), 'test_definitions_ast_identical': True}
    return result


def probe_imports():
    import tests.test_build_profiles as test
    from csplendor import _csplendor

    modules = {}
    for alias, name in [('manifest', 'benchmark_manifest'), ('runner', 'run_paired_benchmarks')]:
        module = getattr(test, alias)
        path = Path(module.__file__).resolve()
        assert module is sys.modules[name]
        assert path == ROOT / 'scripts' / (name + '.py')
        modules[name] = {'path': str(path), 'sys_modules_identity': True}
    extension = Path(_csplendor.__file__).resolve()
    expected = read('start')['f1']['python_build_reused']
    assert str(extension) == expected['path']
    assert prior.sha(extension) == expected['sha256']
    print(json.dumps({'modules': modules, 'extension': expected,
                      'python': sys.version, 'executable': sys.executable}, sort_keys=True))


def finish():
    start = read('start')
    current = prior.source(ROOT)
    old_files = {item['path']: item['sha256'] for item in start['source']['files']}
    new_files = {item['path']: item['sha256'] for item in current['files']}
    assert old_files.keys() == new_files.keys()
    changed = sorted(name for name in old_files if old_files[name] != new_files[name])
    assert changed == sorted(TESTS)
    protected = ['src', 'csplendor', 'scripts', 'CMakeLists.txt', 'setup.py',
                 'setup.cfg', 'pyproject.toml', '.github']
    assert prior.git(ROOT, 'diff', prior.ENGINE, '--', *protected) == ''
    assert prior.git(ROOT, 'diff', BASE, '--name-only', '--', 'tests').splitlines() == sorted(TESTS)
    assert prior.source(ROOT.parent / 'csplendor') == start['original']
    assert prior.git(ROOT, 'rev-parse', 'main', 'origin/main') == start['local_refs']
    build_files = {name: prior.sha(ROOT / name) for name in start['f1']['build_files']}
    assert build_files == start['f1']['build_files']
    before = [line for line in read('collection_before')['stdout'].splitlines() if line.startswith('tests/')]
    after = [line for line in read('collection_after')['stdout'].splitlines() if line.startswith('tests/')]
    assert before == after and len(after) == 73
    old_evidence = {}
    for name in ['final_main_vs_candidate_manifest_20260906.json', 'f2_f3_manifest_20260906.json']:
        path = ROOT / 'doc/performance_experiments' / name
        manifest = json.loads(path.read_text())
        expected = (start['f1']['manifest_sha256'] if name.startswith('final_')
                    else start['old_f2_manifest_sha256'])
        assert prior.sha(path) == expected
        artifacts = manifest['artifacts'] + ([manifest['csv']] if 'csv' in manifest else [])
        for item in artifacts:
            assert prior.sha(ROOT / item['path']) == item['sha256'], item['path']
        old_evidence[name] = {'manifest_sha256': expected, 'artifact_hashes_verified': len(artifacts)}
    results = {name: read(name) for name in ['lint_after', 'security_lint', 'compile',
               'targeted_tests', 'ci_python_coverage', 'import_identity', 'skip_reason']}
    assert all(item['exit_code'] == 0 for item in results.values())
    identity = json.loads(results['import_identity']['stdout'])
    assert prior.sha(identity['extension']['path']) == identity['extension']['sha256']
    save('final_validation', {
        'source': current, 'changed_source_files': changed, 'production_build_harness_diff': '',
        'protected_diff_paths': protected, 'build_files': build_files,
        'test_semantics': invariants(), 'collection_ids_identical': after,
        'old_evidence_unchanged': old_evidence, 'import_identity': identity,
        'original_worktree_unchanged': True, 'local_main_refs_unchanged': True,
        'results': results})
    artifacts = [{'path': str(path.relative_to(ROOT)), 'sha256': prior.sha(path),
                  'bytes': path.stat().st_size} for path in sorted(RAW.glob('*.json.gz'))]
    print(json.dumps({
        'schema': 'csplendor.f3-lint-correction.v1', 'decision': 'READY_FOR_REVIEW',
        'F1_engine_commit': prior.ENGINE, 'F1_record_commit': prior.F1,
        'pre_correction_record_commit': BASE, 'branch': prior.git(ROOT, 'branch', '--show-current'),
        'record_commit_resolver': 'git log -1 --format=%H -- doc/performance_experiments/f3_lint_correction_20260906.md',
        'F1_source_digest': start['source']['source_digest'],
        'candidate_source_digest': current['source_digest'],
        'source_digest_algorithm': 'F1 selection; SHA256 of sorted SHA256 + two spaces + relative path + LF; includes tests',
        'changed_source_files': changed, 'production_build_dependency_measurement_diff_from_F1': False,
        'build_files': build_files, 'extension': identity['extension'],
        'collected_ids_before_after': 73, 'collected_ids_identical': True,
        'all_test_definition_AST_identical': True, 'lint_suppression_added': False,
        'old_evidence_unchanged': old_evidence,
        'validation_exit_codes': {name: item['exit_code'] for name, item in results.items()},
        'python_coverage_summary': results['ci_python_coverage']['stdout'],
        'remote_CI_executed': False, 'F4': 'NOT_EXECUTED_AWAITING_APPROVAL',
        'artifacts': artifacts}, ensure_ascii=False, sort_keys=True, indent=2))


if __name__ == '__main__':
    mode = sys.argv[1]
    if mode == 'start':
        assert prior.git(ROOT, 'rev-parse', 'HEAD') == BASE
        save('start', {'source': prior.source(ROOT), 'f1': prior.verify_f1(),
                       'local_refs': prior.git(ROOT, 'rev-parse', 'main', 'origin/main'),
                       'original': prior.source(ROOT.parent / 'csplendor'),
                       'old_f2_manifest_sha256': prior.sha(ROOT / 'doc/performance_experiments/f2_f3_manifest_20260906.json')})
        run('tool_version', ['python', '-m', 'ruff', '--version'])
        assert run('lint_before', LINT).returncode == 1
        assert run('collection_before', [PYTHON, '-m', 'pytest', '-W', 'error', '-o', 'addopts=',
                   '--collect-only', '-q', '-p', 'no:cacheprovider', *TESTS, *RELATED]).returncode == 0
    elif mode == 'run':
        sys.exit(run(sys.argv[2], sys.argv[3:]).returncode)
    elif mode == 'invariants':
        save('test_semantics', invariants())
        print('Five test bodies/decorators/asserts/fixtures unchanged; no suppressions')
    elif mode == 'probe-imports':
        probe_imports()
    elif mode == 'finish':
        finish()
    else:
        raise ValueError(mode)
