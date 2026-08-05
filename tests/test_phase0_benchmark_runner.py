import importlib.util
from pathlib import Path


def _load_runner():
    path = Path(__file__).parents[1] / "scripts" / "benchmark_phase0.py"
    spec = importlib.util.spec_from_file_location("phase0_benchmark", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_bootstrap_ratio_ci_is_deterministic_and_detects_regression():
    runner = _load_runner()
    baseline = [100.0, 101.0, 99.0, 100.0]
    candidate = [90.0, 91.0, 89.0, 90.0]
    first = runner.paired_ratio_confidence_interval(baseline, candidate, iterations=500, seed=7)
    second = runner.paired_ratio_confidence_interval(baseline, candidate, iterations=500, seed=7)
    assert first == second
    assert first[1] < 1.0


def test_benchmark_has_explicit_nontrivial_warmup():
    runner = _load_runner()
    assert runner.WARMUP_WRAPPER_ITERATIONS >= 1_000
    assert runner.WARMUP_PLAYOUT_GAMES >= 100
