#!/usr/bin/env python3
"""Phase 6 orchestration; timing and semantics use the existing paired runner."""
import gzip
import hashlib
import json
import shlex
import subprocess
import sys
import time
from pathlib import Path
import phase3d1_record_20260905 as prior
from phase3d1_finalize_20260905 import source

ROOT = Path(__file__).resolve().parents[2]
RAW = ROOT / 'doc/performance_experiments/raw/phase6'
prior.ROOT, prior.RAW = ROOT, RAW
save = prior.save


def run(name, args, timeout=1800, check=True):
    start = time.monotonic()
    done = subprocess.run(list(map(str, args)), cwd=ROOT, capture_output=True, timeout=timeout)
    save(name + '.json', {'command': list(map(str, args)), 'cwd': str(ROOT),
        'elapsed_seconds': time.monotonic()-start, 'exit_code': done.returncode,
        'stdout': done.stdout.decode(errors='replace'), 'stderr': done.stderr.decode(errors='replace')})
    print(name, done.returncode, round(time.monotonic()-start, 2), flush=True)
    if check and done.returncode:
        print(done.stdout.decode(errors='replace')[-3000:], done.stderr.decode(errors='replace')[-3000:])
        raise RuntimeError(name)
    return done


def directory(profile):
    return ROOT / 'build' / ('phase6-' + profile)


def build(profile, flags=(), targets=('benchmark_engine_hotpaths', 'state_feature_table_unit', 'mcts_optimization_unit', 'solver_components_unit')):
    build_dir = directory(profile)
    run('configure_' + profile, ['cmake', '-S', ROOT, '-B', build_dir,
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
        '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON', *flags])
    run('build_' + profile, ['cmake', '--build', build_dir, '-j4', '--verbose', '--target', *targets])
    audit(profile)
    if profile != 'generate':
        run('unit_' + profile, ['ctest', '--test-dir', build_dir, '--output-on-failure', '-R',
            '^(' + '|'.join(t for t in targets if t != 'benchmark_engine_hotpaths') + ')$'])


def audit(profile):
    d = directory(profile)
    files = [d/'compile_commands.json', *sorted(d.rglob('link.txt'))]
    files = [p for p in files if p.is_file()]
    save('audit_' + profile + '.json', {'source': source(ROOT),
        'commands': {str(p.relative_to(d)): p.read_text() for p in files},
        'binaries': [{'path': str(p), 'bytes': p.stat().st_size,
                     'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                    for p in [d/'benchmark_engine_hotpaths', *d.glob('_csplendor*.so')] if p.is_file()]})


CASES = {
    'primary': ('exact_reveal', 'hidden_reserve', 1000000, ['--depth', '7']),
    'mcts': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    'random': ('random_selfplay_apply', 'initial', 20000, []),
    'visible': ('visible_solver', 'five_moves', 100000, []),
    'warm': ('exact_reveal', 'five_moves', 500000, ['--depth', '7', '--persistent-reuse', 'true']),
    'legacy': ('legacy_mcts', 'hidden_reserve', 10000, []),
    'shared': ('parallel_scheduler', 'hidden_reserve', 20000, ['--threads','8','--determinization','true']),
    'root': ('root_parallel', 'hidden_reserve', 10000, ['--threads','4','--determinization','true']),
    'v3': ('v3_selfplay', 'initial', 10000, []),
    'proof': ('exact_reveal', 'reveal_heavy', 200000, ['--depth','1','--proof-dag','true']),
}


def measure(profile, stage, names):
    for name in names:
        workload, fixture, iterations, extra = CASES[name]
        threads = int(extra[extra.index('--threads')+1]) if '--threads' in extra else 1
        common = ['--workload', workload, '--fixture', fixture, '--iterations', str(iterations),
                  '--warmup', '100', '--seed', '42', *extra]
        out = RAW / (profile + '_' + stage + '_' + name + '.json')
        assert not out.exists()
        args = [sys.executable, ROOT/'scripts/run_paired_benchmarks.py', 'paired',
            '--baseline-command', shlex.join([str(directory('portable')/'benchmark_engine_hotpaths'), *common]),
            '--candidate-command', shlex.join([str(directory(profile)/'benchmark_engine_hotpaths'), *common]),
            '--baseline-repo-root', ROOT, '--candidate-repo-root', ROOT,
            '--baseline-cmake-cache', directory('portable')/'CMakeCache.txt',
            '--candidate-cmake-cache', directory(profile)/'CMakeCache.txt',
            '--build-profile-axis', {'native':'cpu','lto':'lto','use':'pgo'}[profile],
            '--pairs', '4' if stage == 'smoke' else '22', '--warmups', '2',
            '--bootstrap-iterations', '10000', '--rotate-binary-slots', '--cpu-set',
            '4' if threads == 1 else '4-' + str(threads+3),
            '--timeout', '60', '--output', out]
        done = run(out.stem + '_invocation', args, check=False)
        if out.exists():
            data = json.loads(out.read_text())
            save(out.name, out.read_bytes())
            out.unlink()  # this run's already archived plain JSON only
            row = data['comparison'][0]['B_over_A']
            print(out.stem, row['median'], row['crossover_block_bootstrap_ci95'], flush=True)
        if done.returncode:
            print(done.stderr.decode(errors='replace')[-3000:], flush=True)
            raise RuntimeError(out.stem)


if __name__ == '__main__':
    command = sys.argv[1]
    if command == 'start':
        save('starting_sources.json', {'baseline': source(ROOT.parent/'csplendor-feature-table'),
            'user_workspace': source(ROOT.parent/'csplendor')})
        run('toolchain', ['bash', '-c', 'c++ --version && ld --version && cmake --version && lscpu && python -c "import pybind11; print(pybind11.__version__)"'])
    elif command == 'build':
        for profile in sys.argv[2:]:
            build(profile, {'portable': [], 'native': ['-DCSPLENDOR_CPU_TARGET=native'],
                            'lto': ['-DCSPLENDOR_ENABLE_LTO=ON']}[profile])
    elif command == 'measure':
        measure(sys.argv[2], sys.argv[3], sys.argv[4:] or ['primary', 'mcts', 'random', 'visible'])
