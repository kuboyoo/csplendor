#!/usr/bin/env python3
"""R0 evidence capture; delegates all timing/statistics to the existing harness.

No engine patch, new benchmark implementation, checkout, or remote operation.
Run capture, then measure. Outputs stay beside this file under raw/r0_20260905.
"""
from __future__ import annotations

import argparse
import difflib
import gzip
import hashlib
import io
import json
import shlex
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path('/tmp/csplendor-codex56-phase0')
OUT = Path(__file__).resolve().parent / 'raw/r0_20260905'
RELEASE = Path('/tmp/csplendor-phase3c-stage1-on')
DIAGNOSTIC = Path('/tmp/csplendor-phase3c-stage1-profile')
SAFE_ROOTS = {'src', 'csplendor', 'scripts', 'tests'}
SAFE_SUFFIXES = {'.h', '.cpp', '.py', '.json', '.toml', '.txt', '.cmake', '.yaml', '.yml'}
TOP_FILES = {'CMakeLists.txt', 'pyproject.toml', 'setup.py', 'setup.cfg', 'MANIFEST.in', 'AGENTS.md'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def output(name, data):
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / name
    if path.exists():
        raise FileExistsError(path)
    if not isinstance(data, bytes):
        data = (json.dumps(data, indent=2, sort_keys=True) + '\n').encode()
    path.write_bytes(gzip.compress(data, compresslevel=9, mtime=0))
    return path


def command(args, cwd=SOURCE, timeout=180):
    done = subprocess.run(args, cwd=cwd, capture_output=True, timeout=timeout)
    return {'command': args, 'cwd': str(cwd), 'exit_code': done.returncode,
            'stdout': done.stdout.decode(errors='replace'),
            'stderr': done.stderr.decode(errors='replace')}


def source_files(root):
    result = command(['git', 'ls-files', '-co', '--exclude-standard'], root)
    assert result['exit_code'] == 0
    names = []
    for name in sorted(set(result['stdout'].splitlines())):
        path = Path(name)
        if any(part.startswith('.') or part in {'build', 'venv', '__pycache__'}
               or part.endswith('.egg-info') for part in path.parts):
            continue
        if name in TOP_FILES or (path.parts[0] in SAFE_ROOTS and path.suffix in SAFE_SUFFIXES):
            if (root / name).is_file() and not (root / name).is_symlink():
                names.append(name)
    return names


def capture():
    inventory = {}
    for label, root in [('post3c', SOURCE), ('user_workspace', ROOT)]:
        names = source_files(root)
        entries = [{'path': name, 'sha256': sha((root / name).read_bytes()),
                    'bytes': (root / name).stat().st_size} for name in names]
        lines = ''.join(f"{entry['sha256']}  {entry['path']}\n" for entry in entries).encode()
        diff = command(['git', 'diff', '--binary', '--no-ext-diff', 'HEAD', '--', *names], root)
        assert diff['exit_code'] == 0
        output(label + '_source_diff.patch.gz', diff['stdout'].encode())
        inventory[label] = {
            'root': str(root), 'head': command(['git', 'rev-parse', 'HEAD'], root)['stdout'].strip(),
            'status': command(['git', 'status', '--short', '--branch'], root)['stdout'],
            'source_digest_algorithm': 'sha256 of sorted sha256 + two spaces + relative path + LF',
            'source_digest': sha(lines), 'files': entries,
            'tracked_source_diff_sha256': sha(diff['stdout'].encode()),
            'untracked_sources_in_content_manifest': True,
        }
    # Preserve uncommitted 3C as actual contents, not a HEAD-only worktree.
    archive = io.BytesIO()
    reports = sorted(SOURCE.glob('doc/performance_experiments/*.md'))
    reports += sorted(SOURCE.glob('doc/performance_experiments/phase3[bc]*.json'))
    reports += sorted(SOURCE.glob('doc/performance_experiments/phase3[bc]*.csv'))
    reports += sorted(SOURCE.glob('doc/performance_experiments/raw/phase3[bc]/*'))
    names = sorted(set(source_files(SOURCE) + [str(path.relative_to(SOURCE)) for path in reports]))
    with tarfile.open(fileobj=archive, mode='w') as tar:
        for name in names:
            data = (SOURCE / name).read_bytes()
            info = tarfile.TarInfo(name)
            info.size, info.mode, info.mtime = len(data), 0o644, 0
            tar.addfile(info, io.BytesIO(data))
    output('post3c_source_and_prior_evidence.tar.gz', archive.getvalue())
    inventory['requests'] = {name: sha((ROOT / 'doc' / name).read_bytes()) for name in (
        'REQUEST.md', 'csplendor_codex6_post3c_optimization_request.md',
        'csplendor_codex56_engine_optimization_request.md')}
    inventory['archive'] = 'post3c_source_and_prior_evidence.tar.gz'
    output('source_inventory.json.gz', inventory)
    print(json.dumps({k: {'head': v['head'], 'source_digest': v['source_digest'],
                          'files': len(v['files'])} for k, v in inventory.items()
                      if isinstance(v, dict) and 'head' in v}, indent=2), flush=True)


def measure():
    # Fixed current-state slices; no optimization A/B or exhaustive matrix.
    cases = [
        ('exact_shallow', 'exact_reveal', 'five_moves', 500000, ['--depth', '3']),
        ('exact_deep', 'exact_reveal', 'hidden_reserve', 1000000, ['--depth', '7']),
        ('exact_warm', 'exact_reveal', 'five_moves', 500000, ['--depth', '7', '--persistent-reuse', 'true']),
        ('visible_normal', 'visible_solver', 'five_moves', 100000, []),
        ('visible_cycle', 'visible_solver', 'forced_pass', 1000000, []),
        ('clone', 'clone_light', 'hidden_reserve', 1000000, []),
        ('copy_restore', 'board_copy_restore', 'hidden_reserve', 200000, []),
        ('native_legacy_1t', 'legacy_mcts', 'midgame_250', 20000, ['--threads', '1']),
        ('native_root_1t', 'root_parallel', 'hidden_reserve', 20000, ['--threads', '1', '--determinization', 'true']),
        ('native_root_8t', 'root_parallel', 'hidden_reserve', 20000, ['--threads', '8', '--determinization', 'true']),
        ('native_shared_8t', 'parallel_scheduler', 'hidden_reserve', 20000, ['--threads', '8', '--determinization', 'true']),
    ]
    for name, workload, fixture, iterations, extra in cases:
        run_args = [str(RELEASE / 'benchmark_engine_hotpaths'), '--workload', workload,
                    '--fixture', fixture, '--iterations', str(iterations), '--warmup', '100',
                    '--batch-size', '16', '--seed', '42', *extra]
        output_path = OUT / (name + '.json')
        args = [sys.executable, str(SOURCE / 'scripts/run_paired_benchmarks.py'), 'collect',
                '--command', shlex.join(run_args), '--runs', '5', '--warmups', '2',
                '--cpu-set', '4-11' if name.endswith('8t') else '4',
                '--label', 'R0-current-post3c-' + name, '--repo-root', str(SOURCE),
                '--cmake-cache', str(RELEASE / 'CMakeCache.txt'), '--timeout', '45',
                '--output', str(output_path)]
        result = command(args, timeout=300)
        output(name + '_invocation.json.gz', result)
        if output_path.exists():
            output(name + '.json.gz', output_path.read_bytes())
            output_path.unlink()  # This invocation's archived output only.
        print(name, 'exit', result['exit_code'], result['stderr'][-500:], flush=True)
    # A/A harness sanity, not evidence of speedup. Four crossover blocks only.
    same = shlex.join([str(RELEASE / 'benchmark_engine_hotpaths'), '--workload', 'exact_reveal',
                      '--fixture', 'five_moves', '--iterations', '500000', '--depth', '7', '--warmup', '100'])
    paired_path = OUT / 'aa_exact.json'
    result = command([sys.executable, str(SOURCE / 'scripts/run_paired_benchmarks.py'), 'paired',
                      '--baseline-command', same, '--candidate-command', same,
                      '--pairs', '8', '--warmups', '2', '--bootstrap-iterations', '10000',
                      '--rotate-binary-slots', '--cpu-set', '4',
                      '--baseline-repo-root', str(SOURCE), '--candidate-repo-root', str(SOURCE),
                      '--baseline-cmake-cache', str(RELEASE / 'CMakeCache.txt'),
                      '--candidate-cmake-cache', str(RELEASE / 'CMakeCache.txt'),
                      '--timeout', '45', '--output', str(paired_path)], timeout=300)
    output('aa_exact_invocation.json.gz', result)
    if paired_path.exists():
        output('aa_exact.json.gz', paired_path.read_bytes())
        paired_path.unlink()
    print('aa_exact exit', result['exit_code'], flush=True)
    for name, workload, fixture, iterations, extra in cases[:7]:
        result = command(['taskset', '-c', '4', str(DIAGNOSTIC / 'benchmark_engine_hotpaths'),
                          '--workload', workload, '--fixture', fixture,
                          '--iterations', str(iterations), '--warmup', '100', *extra])
        output('diagnostic_' + name + '.json.gz', result)
        print('diagnostic_' + name, 'exit', result['exit_code'], flush=True)
    output('diagnostic_layout.json.gz', command(['taskset', '-c', '4',
           str(DIAGNOSTIC / 'benchmark_engine_hotpaths'), '--workload', 'layout_probe',
           '--fixture', 'hidden_reserve', '--iterations', '1', '--warmup', '0']))


def supplement():
    diagnostic_source = Path('/tmp/csplendor-r0-diagnostic.nAUqSB')
    changed = ['src/perf_counters.h', 'src/perf_counters.cpp', 'src/reveal_verified_solver.cpp']
    diff = ''.join(''.join(difflib.unified_diff(
        (SOURCE / name).read_text().splitlines(keepends=True),
        (diagnostic_source / name).read_text().splitlines(keepends=True),
        fromfile='a/' + name, tofile='b/' + name)) for name in changed)
    output('scoring_counter_only.patch.gz', diff.encode())
    for name, fixture, budget, depth, extra in [
        ('shallow', 'five_moves', 500000, 3, []),
        ('deep', 'hidden_reserve', 1000000, 7, []),
        ('warm', 'five_moves', 500000, 7, ['--persistent-reuse', 'true']),
    ]:
        output('scoring_' + name + '.json.gz', command([
            'taskset', '-c', '4', str(diagnostic_source / 'build/benchmark_engine_hotpaths'),
            '--workload', 'exact_reveal', '--fixture', fixture, '--iterations', str(budget),
            '--depth', str(depth), '--warmup', '100', *extra]))
    # Existing deterministic and failure/capacity contracts, not a new full gate.
    output('native_contracts.json.gz', command(['ctest', '--test-dir', str(RELEASE),
        '-R', 'solver_components_unit|state_copy_unit|mcts_parallel_replay|mcts_scheduler_limits',
        '--output-on-failure']))
    output('hardware_perf.json.gz', command(['perf', 'stat', '-e', 'cycles,instructions',
        '--', 'taskset', '-c', '4', str(RELEASE / 'benchmark_engine_hotpaths'),
        '--workload', 'clone_light', '--fixture', 'hidden_reserve', '--iterations', '100000']))
    # Preserve exact commands, current layout/flags and extension audit separately.
    output('host_build.json.gz', {
        'commands': [command(cmd) for cmd in [
            ['uname', '-a'], ['lscpu'], ['c++', '--version'], ['cmake', '--version'],
            [sys.executable, '--version'], ['size', str(RELEASE / 'benchmark_engine_hotpaths')]]],
        'binaries': {str(p): {'sha256': sha(p.read_bytes()), 'bytes': p.stat().st_size}
                     for p in [RELEASE / 'benchmark_engine_hotpaths',
                               DIAGNOSTIC / 'benchmark_engine_hotpaths',
                               diagnostic_source / 'build/benchmark_engine_hotpaths']},
        'flags': {str(p): p.read_text() for p in [
            RELEASE / 'CMakeFiles/benchmark_engine_hotpaths.dir/flags.make',
            RELEASE / 'CMakeFiles/benchmark_engine_hotpaths.dir/link.txt',
            DIAGNOSTIC / 'CMakeFiles/benchmark_engine_hotpaths.dir/flags.make',
            diagnostic_source / 'build/CMakeFiles/benchmark_engine_hotpaths.dir/flags.make']},
        'governor': Path('/sys/devices/system/cpu/cpu4/cpufreq/scaling_governor').read_text().strip(),
        'perf_event_paranoid': Path('/proc/sys/kernel/perf_event_paranoid').read_text().strip(),
        'scoring_diagnostic_diff_sha256': sha(diff.encode()),
        'scoring_diagnostic_files': {n: sha((diagnostic_source / n).read_bytes()) for n in changed},
    })
    print('supplement saved', flush=True)


def bindings():
    package = Path('/tmp/csplendor-r0-python/csplendor')
    for path in (SOURCE / 'csplendor').glob('*.py'):
        shutil.copy2(path, package / path.name)
    launcher = [sys.executable, '-c',
        "import sys,runpy;sys.path.insert(0,'/tmp/csplendor-r0-python');"
        "runpy.run_path(sys.argv[1],run_name='__main__')",
        str(Path(__file__).with_name('r0_python_probe_20260905.py'))]
    output('python_encoding.json.gz', command(['taskset', '-c', '4', *launcher], ROOT))
    tests = [str(SOURCE / 'tests' / name) for name in (
        'test_reveal_verified_solver.py', 'test_encoding_schema.py', 'test_encoders.py')]
    selection = ('known_five_move or seven_move_mate_at_exact_depth or '
                 'reuses_descendant or bounds_persistent_cache or '
                 'resumes_incomplete or complete_proof_dag or '
                 'encoding_schema or v3')
    output('python_contracts.json.gz', command([sys.executable, '-c',
        "import sys;sys.path.insert(0,'/tmp/csplendor-r0-python');"
        "import csplendor._csplendor as n;print('R0 module:',n.__file__,flush=True);"
        "import pytest;raise SystemExit(pytest.main(sys.argv[1:]))",
        '-q', *tests, '-k', selection], ROOT, timeout=180))
    build = Path('/tmp/csplendor-r0-python-build')
    output('python_build.json.gz', {
        'interpreter': sys.executable,
        'note': 'CMake initially auto-selected Python3.8; explicitly reconfigured to Python3.12.1 before measurement.',
        'files': {str(path): path.read_text() for path in (
            build / 'CMakeFiles/_csplendor.dir/flags.make',
            build / 'CMakeFiles/_csplendor.dir/link.txt', build / 'CMakeCache.txt')},
        'extension': {str(path): {'sha256': sha(path.read_bytes()), 'bytes': path.stat().st_size}
                      for path in package.glob('*312*.so')},
    })
    for workload in ('clone_light', 'board_copy_restore', 'layout_probe'):
        output('populated_' + workload + '.json.gz', command([
            'taskset', '-c', '4', str(DIAGNOSTIC / 'benchmark_engine_hotpaths'),
            '--workload', workload, '--fixture', 'reveal_heavy',
            '--iterations', '100000' if workload != 'layout_probe' else '1', '--warmup', '0']))
    output('proof_anchor.json.gz', command([
        'taskset', '-c', '4', str(RELEASE / 'benchmark_engine_hotpaths'),
        '--workload', 'exact_reveal', '--fixture', 'reveal_heavy', '--depth', '1',
        '--iterations', '200000', '--warmup', '100', '--proof-dag', 'true']))
    print('bindings saved', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['capture', 'measure', 'supplement', 'bindings'])
    args = parser.parse_args()
    {'capture': capture, 'measure': measure, 'supplement': supplement, 'bindings': bindings}[args.stage]()
