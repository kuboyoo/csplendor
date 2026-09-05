#!/usr/bin/env python3
"""5D uses the existing native benchmark and paired/digest/bootstrap runner."""
import argparse
import hashlib
import json
import os
import sys
from pathlib import Path
import phase3d1_record_20260905 as prior

ROOT = Path(__file__).resolve().parents[2]
VARIANT = os.environ.get('CSPLENDOR_5D_VARIANT', 'v1')
assert VARIANT.isalnum()
RAW = ROOT / 'doc/performance_experiments/raw/phase5d' / VARIANT
BASE_SOURCE = Path(os.environ.get('CSPLENDOR_5D_BASE_SOURCE', str(ROOT.parent / 'csplendor-v3-payment-baseline')))
BASE = BASE_SOURCE / 'build/5d-baseline'
RELEASE = ROOT / 'build/5d-release'
prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
save, run, paired = prior.save, prior.run, prior.paired


def checked(name, args, timeout=600):
    result = run(name, args, timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-4000:], flush=True)
        raise SystemExit(result.returncode)
    return result


CASES = {
    'primary': ('v3_action_mask', 'gold_payment', 50000, []),
    'encode': ('v3_payment_encode', 'initial', 1000000, []),
    'decode': ('v3_payment_decode', 'initial', 1000000, []),
    'midgame': ('v3_action_mask', 'midgame_250', 50000, []),
    'opening': ('v3_action_mask', 'initial', 50000, []),
    'returns': ('v3_action_mask', 'token_return', 50000, []),
    'editor': ('v3_action_mask', 'editor_fallback', 50000, []),
    'nobles': ('v3_action_mask', 'multi_noble', 50000, []),
    'hidden': ('v3_action_mask', 'hidden_reserve', 50000, []),
    'selfplay': ('v3_selfplay', 'initial', 10000, []),
    'mask48': ('action_mask', 'gold_payment', 200000, []),
    'mcts48': ('parallel_scheduler', 'five_moves', 20000, ['--threads', '1']),
}


def measure(stage):
    names = ('primary', 'encode', 'decode', 'selfplay') if stage != 'formal' else CASES
    for name in names:
        paired(stage + '_' + name, *CASES[name], pairs=4 if stage == 'smoke' else 22)


def configure_build(label, directory, source=ROOT, flags=(), targets=('benchmark_engine_hotpaths',)):
    checked('configure_' + label, ['cmake', '-S', str(source), '-B', str(directory),
        '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON',
        '-DCSPLENDOR_BUILD_ENGINE_BENCHMARK=ON', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF', *flags])
    checked('build_' + label, ['cmake', '--build', str(directory), '-j4', '--target', *targets], timeout=1200)


def build():
    prototype = (ROOT / 'src/v3_payment_dp.h').exists()
    unit = 'v3_payment_dp_unit' if prototype else 'v3_payment_codec_unit'
    targets = ('benchmark_engine_hotpaths', unit, 'encoding_schema_unit',
               'rule_query_unit', 'mcts_optimization_unit')
    for label, flags in [('release', []),
                         *([('reference', ['-DCSPLENDOR_V3_PAYMENT_DP=OFF'])] if prototype else []),
                         ('diagnostic', ['-DCSPLENDOR_PERF_INSTRUMENTATION=ON'])]:
        directory = ROOT / ('build/5d-' + label)
        configure_build(label, directory, flags=flags, targets=targets)
        checked('unit_' + label, ['ctest', '--test-dir', str(directory), '--output-on-failure',
                                 '-R', '^(' + '|'.join(targets[1:]) + ')$'])


def deploy():
    import phase4c_record_20260905 as previous
    # Only reuse its tested deployment procedure, not the 4C matrix or gates.
    prior.ROOT, prior.RAW, prior.BASE_SOURCE, prior.BASE, prior.RELEASE = ROOT, RAW, BASE_SOURCE, BASE, RELEASE
    previous.ROOT, previous.RELEASE = ROOT, RELEASE
    previous.checked, previous.save = checked, save
    previous.deploy()


def sanitizers():
    unit = 'v3_payment_dp_unit' if (ROOT / 'src/v3_payment_dp.h').exists() else 'v3_payment_codec_unit'
    targets = (unit, 'encoding_schema_unit', 'rule_query_unit', 'mcts_optimization_unit')
    directory = ROOT / 'build/5d-asan'
    configure_build('asan', directory, flags=['-DCSPLENDOR_SANITIZER=address-undefined'], targets=targets)
    run('unit_asan', ['ctest', '--test-dir', str(directory), '--output-on-failure',
                     '-R', '^(' + '|'.join(targets) + ')$'], timeout=600)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['build', 'smoke', 'formal', 'holdout', 'deploy', 'sanitizers'])
    args = parser.parse_args()
    measure(args.stage) if args.stage in {'smoke', 'formal', 'holdout'} else globals()[args.stage]()
