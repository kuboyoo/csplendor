#!/usr/bin/env python3
"""Recorded GCC-only experiment. Refuse stale source/compiler/profile identity."""
import hashlib
import json
import subprocess
import sys
import os
import shlex
from pathlib import Path
import phase6_record_20260906 as r

REVISION = os.environ.get('CSPLENDOR_PHASE6_PGO_REVISION', '')
if REVISION:
    assert REVISION == 'v2'
    original_directory = r.directory
    r.directory = lambda profile: original_directory(profile + '-' + REVISION)
    r.RAW = r.RAW / REVISION
    r.prior.RAW = r.RAW

PROFILE = r.directory('profiles')
CONTRACT = r.RAW/'pgo_contract.json.gz'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity():
    paths = sorted((r.ROOT/'src').glob('*.h')) + sorted((r.ROOT/'src').glob('*.cpp'))
    paths += [r.ROOT/'CMakeLists.txt', r.ROOT/'scripts/benchmark_engine_hotpaths.cpp']
    return {'source': {str(p.relative_to(r.ROOT)): sha(p) for p in paths},
            'compiler': subprocess.check_output(['c++', '--version']).decode(),
            'compiler_sha256': sha(Path('/usr/bin/c++').resolve()),
            'flags': 'Release -O3 -DNDEBUG portable no-native-IPO no-fast-math GCC atomic generate/use prefix-path'}


def configure(mode):
    assert 'set(CSPLENDOR_PGO_MODE ' in (r.ROOT/'CMakeLists.txt').read_text(), (
        'PGO was rejected. Reproduce only in an isolated experiment tree with '
        'the archived rejected_build_profiles.json.gz CMake source; never silently build without PGO')
    import pybind11
    d = r.directory(mode)
    r.run('configure_pgo_' + mode, ['cmake', '-S', r.ROOT, '-B', d,
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON',
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=ON', '-Dpybind11_DIR='+pybind11.get_cmake_dir(),
        '-DCSPLENDOR_PGO_MODE='+mode, '-DCSPLENDOR_PGO_DIR='+str(PROFILE)])
    r.run('build_pgo_' + mode, ['cmake', '--build', d, '-j4', '--verbose', '--target',
                              'benchmark_engine_hotpaths', '_csplendor'])
    r.audit(mode)


def train():
    assert not PROFILE.exists() and not CONTRACT.exists(), 'Never silently merge old training profiles'
    PROFILE.mkdir(parents=True)
    start = identity()
    configure('generate')
    if REVISION == 'v2':
        # Archives omit entirely unused TUs, whose absent profiles must NOT be
        # ignored at profile-use. Register all core counters during training.
        # This affects the diagnostic trainer only, not the deployment link.
        d = r.directory('generate')
        args = shlex.split((d/'CMakeFiles/benchmark_engine_hotpaths.dir/link.txt').read_text())
        i = args.index('libcsplendor_core.a')
        args[i:i+1] = ['-Wl,--whole-archive','libcsplendor_core.a','-Wl,--no-whole-archive']
        r.run('pgo_training_whole_archive', ['cmake','-E','chdir',d,*args])
    cases = [
        ('legal_count,legal_codes,random_selfplay_apply', 'initial', 20000, []),
        ('legacy_mcts,parallel_scheduler', 'midgame_250', 10000, ['--threads','1']),
        ('legacy_mcts,parallel_scheduler', 'midgame_250', 10000, ['--threads','4','--determinization','true']),
        ('exact_reveal', 'reveal_heavy', 100000, ['--depth','3']),
        ('exact_reveal', 'editor_fallback', 10000, ['--depth','3']),
        ('exact_reveal,visible_solver', 'forced_pass', 1, ['--depth','7']),
        ('exact_reveal', 'reveal_heavy', 20000, ['--depth','1','--proof-dag','true']),
    ]
    for i, (workload, fixture, n, extra) in enumerate(cases):
        r.run('pgo_train_' + str(i), [r.directory('generate')/'benchmark_engine_hotpaths',
            '--workload', workload, '--fixture', fixture, '--seed','17','--warmup','100',
            '--iterations',str(n), *extra])
    extension = next(r.directory('generate').glob('_csplendor*.so'))
    r.run('pgo_train_python', [sys.executable, r.ROOT/'doc/performance_experiments/phase6_python_probe_20260906.py', extension])
    assert identity() == start
    profiles = {str(p.relative_to(PROFILE)): sha(p) for p in sorted(PROFILE.rglob('*.gcda'))}
    assert len(profiles) >= 14
    r.save('pgo_contract.json', {'identity': start, 'profiles': profiles,
        'training_seed': 17, 'training_fixtures': ['initial','midgame_250','reveal_heavy','editor_fallback','forced_pass'],
        'holdout_seed':42, 'holdout_fixtures':['hidden_reserve','five_moves'],
        'atomic': True, 'generate_directory':str(r.directory('generate')), 'profile_directory':str(PROFILE)})


def use():
    import gzip
    saved = json.loads(gzip.decompress(CONTRACT.read_bytes()))
    assert saved['identity'] == identity(), 'stale source/compiler/flags'
    assert saved['profiles'] == {str(p.relative_to(PROFILE)): sha(p) for p in sorted(PROFILE.rglob('*.gcda'))}, 'stale profile'
    configure('use')


if __name__ == '__main__':
    {'train': train, 'use': use}[sys.argv[1]]()
