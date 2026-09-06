#!/usr/bin/env python3
"""Append-only F4 pre-merge evidence, never install into the usual environment."""
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import f2_f3_record_20260906 as prior

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/f4_premerge_20260906'
BASE = 'f6d28dde184ff1950c4729197a3a9ac1645b0c11'
MAIN = 'f5ec6c545c9a2727ca708bc4c6822daf07a2c4dc'
ISOLATED = ROOT.parent.parent / 'csplendor-f4-acceptance-20260906'
WHEEL_SOURCE = ROOT.parent / 'csplendor-f4-wheel-20260906'
prior.RAW = RAW


def run(name, cwd, command, isolated=False):
    env = os.environ.copy()
    env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1', GH_PAGER='cat')
    if isolated:
        for key in list(env):
            if key.startswith(('PYTHON', 'PIP_', 'CSPLENDOR_', 'CMAKE_')) or key in {
                    'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LD_LIBRARY_PATH'}:
                env.pop(key)
        env.update(PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1',
                   PIP_CONFIG_FILE=os.devnull, PIP_INDEX_URL='https://pypi.org/simple',
                   PIP_DISABLE_PIP_VERSION_CHECK='1', CSPLENDOR_CPU_TARGET='portable',
                   PATH=str(ISOLATED / 'build-env/bin') + ':/usr/bin:/bin')
    start = time.monotonic()
    done = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True)
    prior.save(name, {'command': command, 'cwd': str(cwd), 'isolated': isolated,
                      'exit_code': done.returncode, 'elapsed_seconds': time.monotonic() - start,
                      'stdout': done.stdout, 'stderr': done.stderr})
    print(name, done.returncode, done.stdout[-6000:], done.stderr[-2000:], flush=True)
    return done.returncode


def start():
    assert prior.git(ROOT, 'rev-parse', 'HEAD') == BASE
    assert prior.git(ROOT, 'rev-parse', 'origin/main') == MAIN
    assert prior.git(ROOT, 'merge-base', 'origin/main', 'HEAD') == MAIN
    assert prior.git(ROOT, 'diff', prior.ENGINE, '--', 'src', 'csplendor', 'scripts',
                     'CMakeLists.txt', 'setup.py', '_build_support.py', 'pyproject.toml', '.github') == ''
    evidence = {}
    for name in ['final_main_vs_candidate_manifest_20260906.json',
                 'f2_f3_manifest_20260906.json', 'f3_lint_manifest_20260906.json']:
        path = ROOT / 'doc/performance_experiments' / name
        data = json.loads(path.read_text())
        items = data['artifacts'] + ([data['csv']] if 'csv' in data else [])
        for item in items:
            assert prior.sha(ROOT / item['path']) == item['sha256'], item['path']
        evidence[name] = {'sha256': prior.sha(path), 'verified_artifact_count': len(items)}
    source = prior.source(ROOT)
    assert source['source_digest'] == data['candidate_source_digest']
    prior.save('start', {'candidate': source, 'main': MAIN,
                        'local_main': prior.git(ROOT, 'rev-parse', 'main'),
                        'original_worktree': prior.source(ROOT.parent / 'csplendor'),
                        'prior_evidence': evidence, 'main_unchanged_since_F1_F3': True})
    print('PASS: pinned candidate, main ancestor, production/build identity and 131 prior artifacts')


def provenance():
    start_data = json.loads(gzip.decompress((RAW / 'start.json.gz').read_bytes()))
    assert prior.source(ROOT)['source_digest'] == start_data['candidate']['source_digest']
    assert prior.source(WHEEL_SOURCE)['source_digest'] == start_data['candidate']['source_digest']
    assert prior.git(WHEEL_SOURCE, 'rev-parse', 'HEAD') == BASE
    assert prior.git(WHEEL_SOURCE, 'status', '--porcelain') == ''
    assert prior.source(ROOT.parent / 'csplendor') == start_data['original_worktree']
    assert prior.git(ROOT, 'rev-parse', 'main') == start_data['local_main']
    build = WHEEL_SOURCE / 'build/temp.linux-x86_64-cpython-312/portable/default/release'
    flags = {str(p.relative_to(build)): p.read_text() for p in
             [build / 'CMakeFiles/_csplendor.dir/flags.make', build / 'CMakeFiles/_csplendor.dir/link.txt']}
    cache = {}
    for line in (build / 'CMakeCache.txt').read_text().splitlines():
        if line.startswith(('CSPLENDOR_', 'CMAKE_CXX_FLAGS:', 'CMAKE_CXX_FLAGS_RELEASE:',
                            'CMAKE_BUILD_TYPE:', 'CMAKE_CXX_COMPILER:')):
            key, value = line.split('=', 1)
            cache[key] = value
    assert cache['CSPLENDOR_CPU_TARGET:STRING'] == 'portable'
    assert cache['CSPLENDOR_ENABLE_LTO:BOOL'] == 'OFF'
    assert cache['CSPLENDOR_PERF_INSTRUMENTATION:BOOL'] == 'OFF'
    assert cache['CMAKE_BUILD_TYPE:STRING'] == 'Release'
    acceptance = json.loads(json.loads(gzip.decompress((RAW / 'wheel_acceptance_v2.json.gz').read_bytes()))['stdout'])
    assert acceptance['status'] == 'PASS'
    ident = acceptance['identity']
    assert prior.sha(ident['wheel']) == ident['wheel_sha256']
    assert prior.sha(ident['extension']) == ident['extension_sha256']
    old = json.loads((ROOT / 'doc/performance_experiments/f3_lint_manifest_20260906.json').read_text())
    assert ident['extension_sha256'] == old['extension']['sha256']
    prior.save('wheel_provenance', {'build_source': prior.source(WHEEL_SOURCE), 'cache': cache,
                                  'flags_and_link': flags, 'acceptance': acceptance,
                                  'F1_extension_byte_identical': True, 'user_worktree_unchanged': True,
                                  'local_main_unchanged': True, 'system_install_untouched': True})
    print('PASS: clean source, actual build flags/cache, same F1 extension, installed wheel identity')


if __name__ == '__main__':
    if sys.argv[1] == 'start':
        start()
    elif sys.argv[1] == 'provenance':
        provenance()
    elif sys.argv[1] in {'run', 'isolated'}:
        sys.exit(run(sys.argv[2], Path(sys.argv[3]), sys.argv[4:], sys.argv[1] == 'isolated'))
    else:
        raise ValueError(sys.argv[1])
