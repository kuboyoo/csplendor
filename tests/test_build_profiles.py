"""Build-profile comparisons are explicit, never a general mismatch waiver."""

import importlib.util
import sys
from copy import deepcopy
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'scripts'))
manifest = importlib.import_module("benchmark_manifest")
runner = importlib.import_module("run_paired_benchmarks")


@pytest.mark.parametrize('axis,key,value', [
    ('cpu', 'CSPLENDOR_CPU_TARGET', 'native'),
    ('lto', 'CSPLENDOR_ENABLE_LTO', 'ON'),
    ('pgo', 'CSPLENDOR_PGO_MODE', 'use'),
])
def test_profile_axis_fingerprint_only_ignores_selected_option(tmp_path, axis, key, value):
    cache = tmp_path/'CMakeCache.txt'
    base = {'CSPLENDOR_CPU_TARGET': 'portable', 'CSPLENDOR_ENABLE_LTO': 'OFF',
            'CSPLENDOR_PGO_MODE': 'none', 'CSPLENDOR_PGO_DIR': '',
            'CMAKE_CXX_FLAGS_RELEASE': '-O3 -DNDEBUG', 'CSPLENDOR_SANITIZER': 'none'}
    def metadata(values):
        cache.write_text(''.join(f'{k}:STRING={v}\n' for k,v in values.items()))
        return manifest._cmake_build_metadata(cache)[0]
    a = metadata(base)
    b = metadata({**base, key: value})
    assert a['benchmark_build_fingerprint_sha256'] != b['benchmark_build_fingerprint_sha256']
    assert a['profile_axis_fingerprints_sha256'][axis] == b['profile_axis_fingerprints_sha256'][axis]
    for unsafe in ({'CSPLENDOR_SANITIZER': 'thread'}, {'CMAKE_CXX_FLAGS_RELEASE': '-Ofast'},
                   {'CSPLENDOR_VERIFY_INCREMENTAL_HASH': 'ON'}):
        bad = metadata({**base, key: value, **unsafe})
        assert a['profile_axis_fingerprints_sha256'][axis] != bad['profile_axis_fingerprints_sha256'][axis]


def test_runner_requires_explicit_axis_and_complete_provenance():
    spec = importlib.util.spec_from_file_location('profile_test_helpers', ROOT/'tests/test_engine_benchmark_tools.py')
    helpers = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helpers)
    a, b = helpers._manifest(), helpers._manifest('/tmp/b/bench')
    for item in (a, b):
        item['build']['profile_axis_fingerprints_sha256'] = {'cpu': 'same'}
    b['build']['benchmark_build_fingerprint_sha256'] = 'different'
    with pytest.raises(runner.BenchmarkContractError):
        runner.validate_manifest_compatibility(a, b)
    runner.validate_manifest_compatibility(a, b, build_profile_axis='cpu')
    with pytest.raises(runner.BenchmarkContractError):
        runner.validate_manifest_compatibility(a, b, build_profile_axis='lto')
    broken = deepcopy(b)
    broken['tools']['compiler']['sha256'] = 'different'
    with pytest.raises(runner.BenchmarkContractError):
        runner.validate_manifest_compatibility(a, broken, build_profile_axis='cpu')


def test_slot_busy_retry_is_bounded_and_does_not_hide_other_errors(monkeypatch):
    import errno
    attempts, sleeps = [], []
    def transient(*args):
        attempts.append(args)
        if len(attempts) < 3:
            raise OSError(errno.ETXTBSY, 'busy')
        return 123
    monkeypatch.setattr(runner.os, 'open', transient)
    monkeypatch.setattr(runner.time, 'sleep', sleeps.append)
    assert runner._FixedBinarySlotRotator._open_slot_for_write(Path('slot'), 1) == (123, 2)
    assert sleeps == [0.005, 0.005]
    for code, expected in [(errno.EACCES, 1), (errno.ETXTBSY, 51)]:
        attempts.clear()
        def failure(*args):
            attempts.append(args)
            raise OSError(code, 'failure')
        monkeypatch.setattr(runner.os, 'open', failure)
        with pytest.raises(OSError) as exc:
            runner._FixedBinarySlotRotator._open_slot_for_write(Path('slot'), 1)
        assert exc.value.errno == code and len(attempts) == expected
