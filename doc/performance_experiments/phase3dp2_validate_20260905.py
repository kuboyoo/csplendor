#!/usr/bin/env python3
"""Deployment, oracle and sanitizer gates for the isolated 3D-P2 branch."""
import argparse
import hashlib
import shutil
import sys

from phase3dp2_record_20260905 import ROOT, RELEASE, run, save

RUN_SUFFIX = ''

def checked(name, args, timeout=600):
    result = run(name + RUN_SUFFIX, args, timeout=timeout)
    if result.returncode:
        print(result.stdout.decode(errors='replace')[-6000:], flush=True)
        raise SystemExit(result.returncode)


def python_config(build, package):
    import pybind11
    return ['cmake', '-S', str(ROOT), '-B', str(build),
            '-DCSPLENDOR_BUILD_PYTHON_MODULE=ON',
            '-DPython_EXECUTABLE=' + sys.executable,
            '-Dpybind11_DIR=' + pybind11.get_cmake_dir(),
            '-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=' + str(package)]


def deploy():
    binary = RELEASE / 'benchmark_engine_hotpaths'
    before = hashlib.sha256(binary.read_bytes()).hexdigest()
    checked('configure_deploy', python_config(RELEASE, ROOT / 'csplendor'))
    checked('build_deploy_all', ['cmake', '--build', str(RELEASE), '-j4'])
    after = hashlib.sha256(binary.read_bytes()).hexdigest()
    save('deployment_benchmark_identity' + RUN_SUFFIX + '.json', {'before': before, 'after': after, 'identical': before == after})
    assert before == after, 'Deployment rebuild changed the timed benchmark'
    checked('native_full', ['ctest', '--test-dir', str(RELEASE), '--output-on-failure', '-j2'])
    checked('python_full', [sys.executable, '-c',
        "import csplendor._csplendor as n;print('tested extension:',n.__file__,flush=True);"
        "import pytest;raise SystemExit(pytest.main(['tests']))"])
    checked('python_performance', [sys.executable, '-m', 'pytest', '-m', 'performance', 'tests'])
    checked('python_compile', [sys.executable, '-m', 'py_compile',
        *map(str, sorted((ROOT / 'csplendor').glob('*.py')))])


def asan():
    build = ROOT / 'build/3dp2-asan'
    checked('configure_asan', ['cmake', '-S', str(ROOT), '-B', str(build),
        '-DCMAKE_BUILD_TYPE=Release', '-DCSPLENDOR_BUILD_PYTHON_MODULE=OFF',
        '-DCSPLENDOR_BUILD_NATIVE_TESTS=ON', '-DCSPLENDOR_SANITIZER=address-undefined',
        '-DCSPLENDOR_VERIFY_REVEAL_SCORE_ORDER=ON', '-DCSPLENDOR_VERIFY_REVEAL_SEARCH_STATE=ON',
        '-DCSPLENDOR_VERIFY_INCREMENTAL_HASH=ON'])
    checked('build_asan_all', ['cmake', '--build', str(build), '-j4'])
    checked('asan_native_full', ['ctest', '--test-dir', str(build), '--output-on-failure', '-j2'])


def verify():
    build = ROOT / 'build/3dp2-verify'
    package = build / 'python/csplendor'
    package.mkdir(parents=True, exist_ok=True)
    for path in (ROOT / 'csplendor').glob('*.py'):
        shutil.copy2(path, package / path.name)
    checked('configure_verify_python', python_config(build, package))
    checked('build_verify_python', ['cmake', '--build', str(build), '--target', '_csplendor', '-j4'])
    checked('verify_python_solver', [sys.executable, '-c',
        "import sys;sys.path.insert(0,sys.argv[1]);"
        "import csplendor._csplendor as n;print('tested extension:',n.__file__,flush=True);"
        "import pytest;raise SystemExit(pytest.main(['tests/test_reveal_verified_solver.py']))",
        str(package.parent)])
    for label in ('diagnostic', 'reference-diagnostic'):
        directory = ROOT / ('build/3dp2-' + label)
        checked('build_mechanism_' + label, ['cmake', '--build', str(directory), '--target', 'solver_components_unit', '-j4'])
        checked('mechanism_' + label, [str(directory / 'tests/solver_components_unit')])
    for fixture in ('multi_noble', 'final_round', 'reserve_limit', 'gold_payment', 'token_return'):
        for simple in ('false', 'true'):
            checked('verify_special_' + fixture + '_' + simple, [str(build / 'benchmark_engine_hotpaths'),
                '--workload', 'exact_reveal', '--fixture', fixture, '--depth', '3',
                '--iterations', '10000', '--warmup', '100', '--simple-payment', simple])


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stage', choices=['deploy', 'asan', 'verify'])
    parser.add_argument('--label-suffix', default='')
    args = parser.parse_args()
    RUN_SUFFIX = args.label_suffix
    {'deploy': deploy, 'asan': asan, 'verify': verify}[args.stage]()
