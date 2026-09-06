#!/usr/bin/env python3
"""Read-only provenance and isolated acceptance recorder; never overwrites raw."""
import gzip
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/f2_f3_20260906'
ENGINE = 'b202e6a0cbb2eded9bc2ee5e59f750428e73ca49'
F1 = '49878b661298bf45e39c5f5ca5afa6d0e363736a'
CONSUMERS = [ROOT.parent / 'dlsplendor', ROOT.parent / 'splendorgui']


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def git(root, *args):
    return subprocess.check_output(['git', *args], cwd=root, text=True).strip()


def save(name, data):
    RAW.mkdir(parents=True, exist_ok=True)
    with (RAW / (name + '.json.gz')).open('xb') as stream:
        stream.write(gzip.compress(json.dumps(data, ensure_ascii=False,
                                            sort_keys=True, indent=2).encode(), mtime=0))


def consumer(root):
    # Only source/config/document hashes, not contents, models or environment.
    names = git(root, 'ls-files', '-co', '--exclude-standard').splitlines()
    files = []
    for name in sorted(set(names)):
        p = Path(name)
        if (p.parts[0] in {'dlsplendor', 'src', 'scripts', 'tests', 'configs'}
                and p.suffix in {'.py', '.ts', '.tsx', '.yaml'}):
            files.append({'path': name, 'sha256': sha(root / name)})
    digest = hashlib.sha256(''.join(f"{x['sha256']}  {x['path']}\n" for x in files).encode()).hexdigest()
    return {'root': str(root), 'head': git(root, 'rev-parse', 'HEAD'),
            'status': git(root, 'status', '--short', '--branch'),
            'source_digest': digest, 'files': files}


def verify_f1():
    manifest_path = ROOT / 'doc/performance_experiments/final_main_vs_candidate_manifest_20260906.json'
    manifest = json.loads(manifest_path.read_text())
    current = source(ROOT)
    assert current['source_digest'] == manifest['source_digests']['candidate']
    assert git(ROOT, 'diff', ENGINE, F1, '--', 'src', 'csplendor', 'scripts', 'tests',
               'CMakeLists.txt', 'setup.py', 'setup.cfg', 'pyproject.toml', '.github') == ''
    assert git(ROOT, 'diff', ENGINE, 'HEAD', '--', 'src', 'csplendor', 'scripts', 'tests',
               'CMakeLists.txt', 'setup.py', 'setup.cfg', 'pyproject.toml', '.github') == ''
    for item in [manifest['csv'], *manifest['artifacts']]:
        assert sha(ROOT / item['path']) == item['sha256'], item['path']
    tests = {}
    for name in ['ctest_main', 'ctest_candidate', 'python_tests_A', 'python_tests_B',
                 'ctest_reference', 'python_reference', 'ctest_asan',
                 'python_performance_A', 'python_performance_B', 'python_compile']:
        obj = json.loads(gzip.decompress((ROOT / 'doc/performance_experiments/raw/final_integration_20260906' / (name+'.json.gz')).read_bytes()))
        assert obj['exit_code'] == 0, name
        tests[name] = {k: obj[k] for k in ('command', 'exit_code', 'stdout', 'stderr')}
    extension = manifest['python_binary_identity']['B']
    assert sha(extension['path']) == extension['sha256']
    return {'engine_commit': ENGINE, 'f1_record_commit': F1, 'current': current,
            'manifest_sha256': sha(manifest_path), 'raw_hashes_verified': len(manifest['artifacts']),
            'csv_sha256': manifest['csv']['sha256'], 'tests_reused': tests,
            'python_build_reused': extension,
            'build_files': {name: sha(ROOT / name) for name in
                            ('CMakeLists.txt', 'setup.py', 'pyproject.toml') if (ROOT / name).is_file()}}


def start():
    save('start', {'f1_verification': verify_f1(),
                  'consumers': [consumer(p) for p in CONSUMERS],
                  'original': source(ROOT.parent / 'csplendor'),
                  'local_refs': git(ROOT, 'rev-parse', 'HEAD', 'main', 'origin/main'),
                  'remote_main': git(ROOT, 'ls-remote', 'origin', 'refs/heads/main')})


def run(name, args):
    # Do not print/record the inherited environment. Override only public,
    # task-specific controls; disable bytecode and outside-repo cache writes.
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1',
               PYTHONPATH=os.pathsep.join(map(str, [ROOT, *CONSUMERS])),
               DLSPLENDOR_GUI_DEVICE='cpu', DLSPLENDOR_GUI_TORCH_THREADS='2',
               OMP_NUM_THREADS='2', MPLCONFIGDIR=str(ROOT / 'build/f2-mpl-cache'))
    started = time.monotonic()
    try:
        done = subprocess.run(args, cwd=ROOT, env=env, capture_output=True,
                              text=True, timeout=300)
        result = dict(exit_code=done.returncode, stdout=done.stdout, stderr=done.stderr)
    except subprocess.TimeoutExpired as error:
        result = dict(exit_code=None, timeout=True,
                      stdout=(error.stdout or b'').decode(errors='replace'),
                      stderr=(error.stderr or b'').decode(errors='replace'))
    result.update(command=args, cwd=str(ROOT), elapsed_seconds=time.monotonic()-started)
    save(name, result)
    print(name, 'exit_code=', result['exit_code'],
          'elapsed_seconds=', round(result['elapsed_seconds'], 3), flush=True)
    if result['exit_code'] != 0:
        print(result['stderr'][-4000:], flush=True)
    return result


if __name__ == '__main__':
    if sys.argv[1] == 'start':
        start()
        print('F1 source, 100 raw hashes, CSV and recorded tests verified')
    elif sys.argv[1] == 'run':
        sys.exit(0 if run(sys.argv[2], sys.argv[3:])['exit_code'] == 0 else 1)
    else:
        raise ValueError('start or run required')
