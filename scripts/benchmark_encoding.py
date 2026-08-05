#!/usr/bin/env python3
"""Benchmark Python/native encoder and state-feature compatibility wrappers."""

from __future__ import annotations

import argparse
import json
import statistics
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

from csplendor import (
    ActionEncoder,
    ActionEncoderCpp,
    Game,
    StateEncoder,
    StateFeaturizer,
)

SCHEMA_VERSION = 1


def _revision():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _midgame() -> Game:
    game = Game(42)
    for ply in range(16):
        codes = game.legal_action_codes
        if not codes:
            break
        game.apply_action_code_trusted(codes[(ply * 11 + 5) % len(codes)], False)
    return game


def _rate(function, iterations: int) -> float:
    started = time.perf_counter_ns()
    for _ in range(iterations):
        function()
    elapsed = time.perf_counter_ns() - started
    return iterations * 1_000_000_000 / elapsed


def collect(samples: int, iterations: int):
    game = _midgame()
    python_action = ActionEncoder()
    python_state = StateFeaturizer()
    native_mask = np.asarray(ActionEncoderCpp.get_action_mask(game), dtype=bool)
    action_id = int(np.flatnonzero(native_mask)[0])
    workloads = {
        "python_state_features_per_sec": lambda: python_state.featurize(game, 0),
        "native_state_features_per_sec": lambda: StateEncoder.encode(game, 0),
        "python_action_mask_per_sec": lambda: python_action.get_action_mask(game),
        "native_action_mask_per_sec": lambda: ActionEncoderCpp.get_action_mask(game),
        "python_action_decode_per_sec": lambda: python_action.decode(action_id, game),
        "native_action_decode_per_sec": lambda: ActionEncoderCpp.decode(action_id, game),
    }
    for _ in range(500):
        for workload in workloads.values():
            workload()
    rates = {name: [] for name in workloads}
    for _ in range(samples):
        for name, workload in workloads.items():
            rates[name].append(_rate(workload, iterations))
    return rates


def report(label: str, samples: int, iterations: int):
    rates = collect(samples, iterations)
    return {
        "schema_version": SCHEMA_VERSION,
        "label": label,
        "revision": _revision(),
        "python": sys.version,
        "samples": samples,
        "iterations": iterations,
        "rates": rates,
        "medians": {name: statistics.median(values) for name, values in rates.items()},
    }


def compare(baseline_path: Path, candidate_path: Path):
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    candidate = json.loads(candidate_path.read_text(encoding="utf-8"))
    if baseline["schema_version"] != candidate["schema_version"]:
        raise ValueError("benchmark schema mismatch")
    return {
        "schema_version": SCHEMA_VERSION,
        "baseline": str(baseline_path),
        "candidate": str(candidate_path),
        "candidate_over_baseline": {
            name: candidate["medians"][name] / baseline["medians"][name]
            for name in sorted(set(baseline["medians"]) & set(candidate["medians"]))
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="local")
    parser.add_argument("--samples", type=int, default=15)
    parser.add_argument("--iterations", type=int, default=1_000)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare", nargs=2, type=Path, metavar=("BASE", "CANDIDATE"))
    args = parser.parse_args()
    payload = (
        compare(*args.compare)
        if args.compare
        else report(args.label, args.samples, args.iterations)
    )
    rendered = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)


if __name__ == "__main__":
    main()
