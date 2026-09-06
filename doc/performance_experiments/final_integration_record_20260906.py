#!/usr/bin/env python3
"""Final integration orchestration; reuse the existing recorder and A/B runner."""
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
BASE = ROOT.parent / 'csplendor-final-main'
RAW = ROOT / 'doc/performance_experiments/raw/final_integration_20260906'
prior.RAW = RAW
save = prior.save


def run(name, args, root=ROOT, check=True, timeout=1800):
    start = time.monotonic()
    args = list(map(str, args))
    done = subprocess.run(args, cwd=root, capture_output=True, timeout=timeout)
    save(name+'.json', {'command': args, 'cwd': str(root), 'exit_code': done.returncode,
         'elapsed_seconds': time.monotonic()-start,
         'stdout': done.stdout.decode(errors='replace'), 'stderr': done.stderr.decode(errors='replace')})
    print(name, done.returncode, round(time.monotonic()-start, 2), flush=True)
    if check and done.returncode:
        print(done.stdout.decode(errors='replace')[-4000:], done.stderr.decode(errors='replace')[-4000:], flush=True)
        raise RuntimeError(name)
    return done


def native(profile, root=ROOT, extra=()):
    directory = root/'build'/('final-'+profile)
    run('configure_'+profile, ['cmake','-S',root,'-B',directory,
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
        '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF','-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', *extra])
    run('build_'+profile, ['cmake','--build',directory,'-j4','--verbose'])
    run('ctest_'+profile, ['ctest','--test-dir',directory,'--output-on-failure','-j2'])


def adapter():
    # Mechanical extraction from the existing harness, identical on A/B/C.
    # Omit only the non-public TT/state-key/layout microbenchmarks absent on
    # main, not engine code, fixture setup, timing, digests or semantic gates.
    text = (ROOT/'scripts/benchmark_engine_hotpaths.cpp').read_text()
    text = text.replace('#include "solver_tt_types.h"\n', '')
    for first, last in [('using SolverBenchmarkKey =', 'uint64_t digest_visible_result('),
                        ('struct MoveListBoundaryObservables {', 'void append_perf_counters(')]:
        start, end = text.index(first), text.index(last)
        text = text[:start] + text[end:]
    for workload in ('solver_state_key', 'solver_tt', 'layout_probe'):
        start = text.index('  if (workload == "'+workload+'")')
        end = text.index(';', start) + 1
        text = text[:start] + ('  if (workload == "'+workload+'")\n'
            '    throw std::invalid_argument("Internal micro excluded from common adapter");') + text[end:]
    directory = ROOT/'build/final-common-source'
    directory.mkdir(parents=True)
    (directory/'compat').mkdir()
    # This header has compile-time no-op counters with PERF OFF. It is supplied
    # only to the benchmark translation unit; no engine implementation is copied.
    for name, data in [('benchmark.cpp', text.encode()), ('compat/perf_counters.h',
                       (ROOT/'src/perf_counters.h').read_bytes())]:
        with (directory/name).open('xb') as stream:
            stream.write(data)
    save('common_adapter.json', {'source_harness_sha256': hashlib.sha256(
        (ROOT/'scripts/benchmark_engine_hotpaths.cpp').read_bytes()).hexdigest(),
        'generated_sha256': hashlib.sha256(text.encode()).hexdigest(),
        'removed_workloads': ['solver_state_key','solver_tt','layout_probe'],
        'engine_sources_copied': False, 'generated_source': text})


def common_build(profile, root, lto=False):
    directory = root/'build'/('final-common-'+profile)
    common = ROOT/'build/final-common-source'
    run('configure_common_'+profile, ['cmake','-S',ROOT/'doc/performance_experiments/final_common_adapter',
        '-B',directory,'-DENGINE_ROOT='+str(root), '-DCOMMON_BENCHMARK_SOURCE='+str(common/'benchmark.cpp'),
        '-DCOMMON_BENCHMARK_COMPAT='+str(common/'compat'), '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON','-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF',
        '-DCSPLENDOR_BUILD_NATIVE_TESTS=OFF','-DCSPLENDOR_ENABLE_LTO='+('ON' if lto else 'OFF')])
    run('build_common_'+profile,['cmake','--build',directory,'-j4','--verbose'])
    save('audit_common_'+profile+'.json', {
        'engine_source': source(root),
        'compile_commands': (directory/'compile_commands.json').read_text(),
        'links': {str(p.relative_to(directory)):p.read_text() for p in directory.rglob('link.txt')},
        'binary_sha256': hashlib.sha256((directory/'final_common_benchmark').read_bytes()).hexdigest()})


CASES = {
    'primary': ('exact_reveal','hidden_reserve',1000000,['--depth','7']),
    'shallow': ('exact_reveal','five_moves',500000,['--depth','3']),
    'warm': ('exact_reveal','five_moves',500000,['--depth','7','--persistent-reuse','true']),
    'visible': ('visible_solver','five_moves',100000,[]),
    'count': ('legal_count','midgame_250',200000,[]),
    'codes': ('legal_codes','midgame_250',200000,[]),
    'actions': ('legal_actions','midgame_250',200000,[]),
    'random': ('random_selfplay_apply','initial',100000,[]),
    'legacy': ('legacy_mcts','hidden_reserve',10000,[]),
    'serial': ('parallel_scheduler','five_moves',20000,['--threads','1']),
    'shared': ('parallel_scheduler','hidden_reserve',20000,['--threads','8','--determinization','true']),
    'root': ('root_parallel','hidden_reserve',10000,['--threads','4','--determinization','true']),
    'v3': ('v3_selfplay','initial',10000,[]),
}


def launcher_build():
    cpp = ROOT/'doc/performance_experiments/phase5cb_boundary_launcher_20260906.cpp'
    bench = ROOT/'doc/performance_experiments/phase5cb_boundary_bench_20260906.py'
    for label, root, profile in [('A',BASE,'main'),('B',ROOT,'candidate')]:
        py = ROOT/'build'/('final-env-'+label)/'bin/python'
        directory = root/'build'/('final-common-'+profile)
        run('launcher_'+label,['c++','-std=c++17','-O2',cpp,
            '-DCSPLENDOR_PYTHON_BINARY="'+str(py)+'"',
            '-DCSPLENDOR_BOUNDARY_SCRIPT="'+str(bench)+'"',
            '-DCSPLENDOR_BOUNDARY_REPO="'+str(root)+'"','-o',directory/'boundary'])
        extension = next((root/'csplendor').glob('_csplendor*.so'))
        cache = next((root/'build').glob('temp.*/portable/default/release/CMakeCache.txt'))
        run('python_flags_'+label,['cmake','--build',cache.parent,'--verbose'])
        save('python_binary_'+label+'.json', {'path':str(extension),
            'sha256':hashlib.sha256(extension.read_bytes()).hexdigest(),
            'interpreter':str(py), 'cache':str(cache),
            'link':{str(p.relative_to(cache.parent)):p.read_text() for p in cache.parent.rglob('link.txt')},
            'flags':{str(p.relative_to(cache.parent)):p.read_text() for p in cache.parent.rglob('flags.make')}})


def measure(axis, stage, names):
    if axis == 'code':
        left_root, left_profile, right_profile = BASE, 'main', 'candidate'
    elif axis == 'lto':
        left_root, left_profile, right_profile = ROOT, 'candidate', 'lto'
    elif axis == 'deployment':
        left_root, left_profile, right_profile = BASE, 'main', 'lto'
    else:
        raise ValueError(axis)
    for name in names:
        python_case = name in ('features','pipeline')
        if python_case:
            assert axis == 'code'
            common = ['--iterations','50000'] + (['--pipeline'] if name == 'pipeline' else [])
            threads = 1
            binary_name = 'boundary'
        else:
            workload,fixture,count,extra = CASES[name]
            common = ['--workload',workload,'--fixture',fixture,'--iterations',str(count),
                      '--warmup','100','--seed','42',*extra]
            threads = int(extra[extra.index('--threads')+1]) if '--threads' in extra else 1
            binary_name = 'final_common_benchmark'
        left = left_root/'build'/('final-common-'+left_profile)
        right = ROOT/'build'/('final-common-'+right_profile)
        stem = axis+'_'+stage+'_'+name
        output = RAW/(stem+'.json')
        assert not output.exists() and not output.with_suffix('.json.gz').exists()
        args = [sys.executable,ROOT/'scripts/run_paired_benchmarks.py','paired',
            '--baseline-command',shlex.join([str(left/binary_name),*common]),
            '--candidate-command',shlex.join([str(right/binary_name),*common]),
            '--baseline-repo-root',left_root,'--candidate-repo-root',ROOT,
            '--baseline-cmake-cache',left/'CMakeCache.txt',
            '--candidate-cmake-cache',right/'CMakeCache.txt',
            '--pairs','4' if stage == 'smoke' else '22','--warmups','2',
            '--bootstrap-iterations','10000','--rotate-binary-slots',
            '--cpu-set','4' if threads == 1 else '4-'+str(threads+3),
            '--timeout','120','--output',output]
        if axis != 'code':
            args += ['--build-profile-axis','lto']
        done = run(stem+'_invocation',args,check=False,timeout=3600)
        if output.exists():
            blob = output.read_bytes()
            save(stem+'.json',blob)
            output.unlink()  # Only this run's durably archived plain JSON.
            result = json.loads(blob)['comparison'][0]['B_over_A']
            print(stem,result['median'],result['crossover_block_bootstrap_ci95'],flush=True)
        if done.returncode:
            print(done.stderr.decode(errors='replace')[-3000:],flush=True)
            raise RuntimeError(stem)


if __name__ == '__main__':
    command = sys.argv[1]
    if command == 'start':
        save('start.json', {name: source(root) for name, root in [('main',BASE),('candidate',ROOT),
            ('user_workspace', ROOT.parent/'csplendor'),('phase6', ROOT.parent/'csplendor-build-profiles')]})
    elif command == 'native':
        native('main', BASE)
        native('candidate')
    elif command == 'adapter':
        adapter()
        common_build('main', BASE)
        common_build('candidate', ROOT)
        common_build('lto', ROOT, lto=True)
    elif command == 'launchers':
        launcher_build()
    elif command == 'measure':
        measure(sys.argv[2],sys.argv[3],sys.argv[4:] or list(CASES))
