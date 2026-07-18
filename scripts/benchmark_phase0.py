#!/usr/bin/env python3
"""Phase 0 paired benchmark runner.

Run a baseline and a candidate in separate worktrees/environments, keep their
JSON outside the repository, then compare the two raw sample sets:

    python scripts/benchmark_phase0.py --label baseline --output /tmp/base.json
    python scripts/benchmark_phase0.py --label candidate --output /tmp/candidate.json
    python scripts/benchmark_phase0.py --compare /tmp/base.json /tmp/candidate.json

The runner intentionally uses the stdlib only.  It is a comparison tool, not
a release gate by itself: CPU affinity, compiler, extension path and command
metadata in the JSON must match before interpreting a confidence interval.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import random
import statistics
import subprocess
import sys
import time
from pathlib import Path

from csplendor import Game


SCHEMA_VERSION = 1


def paired_ratio_confidence_interval(baseline, candidate, *, iterations=10_000, seed=0):
    """Return (low, mean, high) for candidate/baseline via paired bootstrap."""
    if len(baseline) != len(candidate) or not baseline:
        raise ValueError("baseline and candidate require the same non-zero sample count")
    if any(value <= 0 for value in baseline) or any(value <= 0 for value in candidate):
        raise ValueError("rates must be positive")
    ratios = [candidate_value / baseline_value for baseline_value, candidate_value in zip(baseline, candidate)]
    rng = random.Random(seed)
    draws = []
    for _ in range(iterations):
        draws.append(sum(rng.choice(ratios) for _ in ratios) / len(ratios))
    draws.sort()
    lower = draws[int(0.025 * (iterations - 1))]
    upper = draws[int(0.975 * (iterations - 1))]
    return lower, statistics.fmean(ratios), upper


def _git_revision():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _make_midgame():
    game = Game(seed=42)
    for ply in range(12):
        codes = game.legal_action_codes
        if not codes or game.is_game_over():
            break
        game.apply_action_code_trusted(codes[(ply * 7 + 3) % len(codes)], False)
    return game


def _rate(work, iterations):
    started = time.perf_counter_ns()
    for _ in range(iterations):
        work()
    elapsed = time.perf_counter_ns() - started
    return iterations * 1_000_000_000 / elapsed


def _cpp_playout_rate(games):
    started = time.perf_counter_ns()
    moves = 0
    rng = random.Random(123)
    for seed in range(games):
        game = Game(seed=seed)
        while not game.is_game_over() and game.turn < 200:
            if not game.apply_random_action(rng.getrandbits(64), False):
                break
            moves += 1
    elapsed = time.perf_counter_ns() - started
    return moves * 1_000_000_000 / elapsed


def collect(samples, iterations):
    """Collect paired-order raw samples from a fixed corpus."""
    game = _make_midgame()
    # Warm caches and extension dispatch before taking samples.
    game.legal_actions
    game.legal_action_codes
    game.legal_action_count
    result = {
        "legal_actions_per_sec": [],
        "legal_action_codes_per_sec": [],
        "legal_action_count_per_sec": [],
        "cpp_playout_moves_per_sec": [],
    }
    for _ in range(samples):
        # Keep a fixed order: paired A/B runs compare sample i with sample i.
        result["legal_actions_per_sec"].append(_rate(lambda: game.legal_actions, iterations))
        result["legal_action_codes_per_sec"].append(_rate(lambda: game.legal_action_codes, iterations))
        result["legal_action_count_per_sec"].append(_rate(lambda: game.legal_action_count, iterations * 2))
        result["cpp_playout_moves_per_sec"].append(_cpp_playout_rate(10))
    return result


def report(label, samples, iterations):
    rates = collect(samples, iterations)
    return {
        "schema_version": SCHEMA_VERSION,
        "label": label,
        "revision": _git_revision(),
        "command": sys.argv,
        "environment": {
            "python": sys.version,
            "platform": platform.platform(),
            "processor": platform.processor(),
            "pid": os.getpid(),
        },
        "workload": {
            "midgame_seed": 42,
            "samples": samples,
            "iterations": iterations,
            "rates": rates,
        },
    }


def _load_json(path):
    data = json.loads(Path(path).read_text())
    if data.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(f"unsupported benchmark schema: {data.get('schema_version')}")
    return data


def compare(baseline_path, candidate_path, iterations):
    baseline = _load_json(baseline_path)
    candidate = _load_json(candidate_path)
    base_rates = baseline["workload"]["rates"]
    candidate_rates = candidate["workload"]["rates"]
    common = sorted(set(base_rates) & set(candidate_rates))
    metrics = {}
    for name in common:
        low, mean, high = paired_ratio_confidence_interval(
            base_rates[name], candidate_rates[name], iterations=iterations, seed=0
        )
        metrics[name] = {
            "candidate_over_baseline_ci95": [low, mean, high],
            "regression_confirmed": high < 1.0,
        }
    return {
        "schema_version": SCHEMA_VERSION,
        "baseline": {"path": str(baseline_path), "revision": baseline.get("revision")},
        "candidate": {"path": str(candidate_path), "revision": candidate.get("revision")},
        "metrics": metrics,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="local")
    parser.add_argument("--samples", type=int, default=15)
    parser.add_argument("--iterations", type=int, default=1_000)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare", nargs=2, metavar=("BASELINE", "CANDIDATE"))
    args = parser.parse_args()
    if args.samples < 1 or args.iterations < 1:
        parser.error("--samples and --iterations must be positive")

    payload = compare(*args.compare, args.iterations) if args.compare else report(args.label, args.samples, args.iterations)
    rendered = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(rendered + "\n")
    print(rendered)


if __name__ == "__main__":
    main()
