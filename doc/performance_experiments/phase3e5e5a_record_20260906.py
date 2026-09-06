#!/usr/bin/env python3
"""Reuse the existing paired runner; one independently named ticket per run."""
import os
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
TICKET = os.environ.get('CSPLENDOR_SELECTION_TICKET', '3e')
assert TICKET.replace('_', '').isalnum()
RAW = ROOT / 'doc/performance_experiments/raw/phase3e5e5a' / TICKET
BASE_SOURCE = Path(os.environ.get('CSPLENDOR_SELECTION_BASE', str(ROOT.parent / 'csplendor-v3-payment-dp')))
BASE = Path(os.environ.get('CSPLENDOR_SELECTION_BASE_BUILD', str(BASE_SOURCE / 'build/5d-release')))
RELEASE = ROOT / ('build/' + TICKET + '-release')
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run, paired = prior.save, prior.run, prior.paired


def checked(name, args, timeout=1200):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-6000:], flush=True)
        raise SystemExit(result.returncode)
    return result


def build(label, flags=(), targets=('benchmark_engine_hotpaths', 'action_selection_unit', 'solver_components_unit')):
    directory = ROOT / ('build/' + label)
    checked('configure_' + label, ['cmake', '-S', str(ROOT), '-B', str(directory),
        '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
        '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
    checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets])
    checked('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
                            '-R', '^(' + '|'.join(targets[1:]) + ')$'])


CASES = {
    '3e': {
        'primary': ('visible_solver', 'five_moves', 100000, []),
        'returns': ('visible_solver', 'token_return', 20000, []),
        'cycle': ('visible_solver', 'forced_pass', 100000, []),
        'solver': ('exact_reveal', 'hidden_reserve', 100000, ['--depth', '3']),
        'mcts': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    },
    '5e': {
        'primary': ('random_selfplay_apply', 'initial', 20000, []),
        'simple': ('random_selfplay_apply', 'initial', 20000, ['--simple-payment', 'true']),
        'count': ('legal_count', 'token_return', 100000, []),
        'codes': ('legal_codes', 'token_return', 100000, []),
        'editor': ('legal_codes', 'editor_fallback', 10000, []),
        'solver': ('visible_solver', 'five_moves', 100000, []),
        'mcts': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
    },
    '5a': {
        'primary': ('decode_apply', 'midgame_250', 200000, []),
        'opening': ('decode_apply', 'initial', 200000, []),
        'purchase': ('decode_apply', 'gold_payment', 200000, []),
        'hidden': ('decode_apply', 'hidden_reserve', 200000, []),
        'mask': ('action_mask', 'midgame_250', 200000, []),
        'mcts': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
        'solver': ('visible_solver', 'five_moves', 100000, []),
    },
}


def measure(stage, names=None):
    cases = CASES[TICKET.split('_')[0]]
    for name in names or (('primary', 'mcts') if stage == 'smoke' else cases):
        paired(stage + '_' + name, *cases[name], pairs=4 if stage == 'smoke' else 22)


def final_builds():
    build('final-release')
    build('final-reference', ['-DCSPLENDOR_GROUP_TAKE_CANDIDATES=OFF',
                             '-DCSPLENDOR_RETURN_RANK_SELECTION=OFF'])
    build('final-diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON'])
    build('final-asan', ['-DCSPLENDOR_SANITIZER=address-undefined'],
          ('action_selection_unit', 'solver_components_unit', 'solver_normal_rollback_unit',
           'rule_query_unit', 'mcts_optimization_unit'))
    # build() uses targets[1:] as the test list; include the first native target.
    checked('unit_final_asan_selection', ['ctest', '--test-dir', str(ROOT / 'build/final-asan'),
                                        '--output-on-failure', '-R', '^action_selection_unit$'])


def deploy():
    import phase4c_record_20260905 as previous
    prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
    previous.ROOT, previous.RELEASE = ROOT, RELEASE
    previous.checked, previous.save = checked, save
    previous.deploy()


def final_measurements():
    # Direct comparison with the phase-start engine, not multiplied ticket gains.
    for name, case in {
        'visible': CASES['3e']['primary'], 'random': CASES['5e']['primary'],
        'mcts': CASES['5a']['mcts'], 'decode': CASES['5a']['primary'],
        'v3': ('v3_selfplay', 'initial', 10000, []),
    }.items():
        paired('common_' + name, *case, pairs=22)
    # The new diagnostic slice uses identical source/harness with both new
    # optimization flags OFF as its reference. The mask/count APIs are unchanged.
    prior.BASE_SOURCE, prior.BASE = ROOT, ROOT / 'build/final-reference'
    for workload in ('legal_count', 'legal_select'):
        paired('component_' + workload, workload, 'token_return', 100000, [], pairs=22)
    for profile in ('release', 'reference', 'diagnostic'):
        binary = ROOT / ('build/final-' + profile) / 'benchmark_engine_hotpaths'
        for workload, fixture, budget in (
            ('legal_count', 'token_return', 100000),
            ('legal_select', 'token_return', 100000),
            ('visible_solver', 'five_moves', 100000),
        ):
            checked('profile_' + profile + '_' + workload,
                    [str(binary), '--workload', workload, '--fixture', fixture,
                     '--iterations', str(budget), '--seed', '42', '--warmup', '100'])


if __name__ == '__main__':
    if sys.argv[1] == 'builds':
        final_builds()
    elif sys.argv[1] == 'deploy':
        deploy()
    elif sys.argv[1] == 'final_measurements':
        final_measurements()
    else:
        measure(sys.argv[1], sys.argv[2:] or None)
