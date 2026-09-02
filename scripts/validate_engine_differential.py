#!/usr/bin/env python3
"""Run the native Phase-0 harness over a deterministic seed corpus.

The validator deliberately does not import the Python extension.  It can run
the same corpus against two independently built trees and compare the emitted
summary digest without accidentally loading an editable install from another
worktree.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from typing import Any, Mapping, Sequence

try:
    from benchmark_manifest import atomic_write_json, sha256_file
except ModuleNotFoundError:  # Importing as scripts.validate_engine_differential.
    from scripts.benchmark_manifest import atomic_write_json, sha256_file


SCHEMA = "csplendor.engine_differential.v1"
WORKLOADS = (
    "legal_count",
    "legal_codes",
    "legal_actions",
    "apply_exact_hash",
    "apply_observable_hash",
    "action_mask",
)
EXACT_HASH_ORACLE_CHECKS_PER_SEED = 1 + sum(
    workload in {"apply_exact_hash", "apply_observable_hash"} for workload in WORKLOADS
)


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def _run_seed(
    binary: Path, seed: int, fixture_plies: int, timeout: float
) -> list[dict[str, Any]]:
    command = [
        str(binary),
        "--workload",
        ",".join(WORKLOADS),
        "--fixture",
        "random",
        "--fixture-plies",
        str(fixture_plies),
        "--seed",
        str(seed),
        "--iterations",
        "1",
        "--warmup",
        "0",
        "--simple-payment",
        "true" if seed & 1 else "false",
    ]
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    records = [json.loads(line) for line in completed.stdout.splitlines() if line]
    if [record.get("workload") for record in records] != list(WORKLOADS):
        raise RuntimeError(f"seed {seed}: native workload order changed")
    for record in records:
        if record.get("schema") != "csplendor.engine_hotpath.v1":
            raise RuntimeError(f"seed {seed}: unexpected native schema")
        if record.get("fixture") != "random":
            raise RuntimeError(f"seed {seed}: unexpected fixture")
        semantics = record.get("semantics", {})
        if semantics.get("correct") is not True:
            raise RuntimeError(f"seed {seed}: correctness oracle failed")
    if records[1]["digest"] != records[2]["digest"]:
        raise RuntimeError(
            f"seed {seed}: ordered legal Action/code digests do not match"
        )
    expected_count = int(records[0]["semantics"]["fixture_legal_count"])
    for index in (1, 2):
        if int(records[index]["counters"]["actions_per_call"]) != expected_count:
            raise RuntimeError(f"seed {seed}: legal count does not match emission")
    return records


def _semantic_record(record: Mapping[str, Any]) -> dict[str, Any]:
    """Return the native fields that must remain identical across builds."""
    return {
        key: value
        for key, value in record.items()
        if key
        not in {
            "elapsed_ns",
            "rate_per_second",
            "rss_kib",
            "runner_rss_kib",
        }
    }


def _first_difference(left: Any, right: Any, path: str = "record") -> str | None:
    if isinstance(left, Mapping) and isinstance(right, Mapping):
        for key in sorted(set(left) | set(right)):
            child = f"{path}.{key}"
            if key not in left or key not in right:
                return child
            mismatch = _first_difference(left[key], right[key], child)
            if mismatch is not None:
                return mismatch
        return None
    if isinstance(left, list) and isinstance(right, list):
        if len(left) != len(right):
            return f"{path}.length"
        for index, (left_item, right_item) in enumerate(zip(left, right)):
            mismatch = _first_difference(left_item, right_item, f"{path}[{index}]")
            if mismatch is not None:
                return mismatch
        return None
    return None if left == right else path


def _binary_metadata(binary: Path) -> dict[str, Any]:
    resolved = binary.resolve()
    if not resolved.is_file():
        raise FileNotFoundError(resolved)
    return {
        "path": str(resolved),
        "sha256": sha256_file(resolved),
        "size_bytes": resolved.stat().st_size,
    }


def validate(
    baseline_binary: Path,
    seeds: int,
    seed_offset: int,
    fixture_plies: int,
    *,
    candidate_binary: Path | None = None,
    timeout: float = 30.0,
) -> dict[str, Any]:
    if seeds <= 0:
        raise ValueError("seeds must be positive")
    if fixture_plies < 0:
        raise ValueError("fixture plies must be non-negative")
    if timeout <= 0.0:
        raise ValueError("timeout must be positive")
    baseline_binary = baseline_binary.resolve()
    baseline_metadata = _binary_metadata(baseline_binary)
    candidate_metadata = (
        _binary_metadata(candidate_binary) if candidate_binary is not None else None
    )

    baseline_digest = hashlib.sha256()
    candidate_digest = hashlib.sha256() if candidate_metadata is not None else None
    legal_action_total = 0
    setup_action_total = 0
    semantic_record_checks = 0
    semantic_record_failures = 0
    for relative_seed in range(seeds):
        seed = seed_offset + relative_seed
        records = _run_seed(baseline_binary, seed, fixture_plies, timeout)
        candidate_records = (
            _run_seed(candidate_binary.resolve(), seed, fixture_plies, timeout)
            if candidate_binary is not None
            else None
        )
        if candidate_records is not None:
            for baseline_record, candidate_record in zip(records, candidate_records):
                left = _semantic_record(baseline_record)
                right = _semantic_record(candidate_record)
                mismatch = _first_difference(left, right)
                if mismatch is not None:
                    workload = baseline_record.get("workload", "unknown")
                    raise RuntimeError(
                        f"seed {seed}, workload {workload}: semantic mismatch at "
                        f"{mismatch}"
                    )
        semantics = records[0]["semantics"]
        legal_action_total += int(semantics["fixture_legal_count"])
        setup_action_total += int(semantics["fixture_setup_actions"])
        stable = {
            "seed": seed,
            "simple_payment_mode": bool(semantics["simple_payment_mode"]),
            "fixture_exact_hash": semantics["fixture_exact_hash"],
            "fixture_observable_hash_0": semantics["fixture_observable_hash_0"],
            "fixture_observable_hash_1": semantics["fixture_observable_hash_1"],
            "fixture_legal_count": semantics["fixture_legal_count"],
            "fixture_ordered_legal_digest": semantics["fixture_ordered_legal_digest"],
            "workload_digests": [record["digest"] for record in records],
        }
        for record in records:
            semantic_record_checks += 1
            if record.get("semantics", {}).get("correct") is not True:
                semantic_record_failures += 1
        if candidate_records is not None:
            for record in candidate_records:
                semantic_record_checks += 1
                if record.get("semantics", {}).get("correct") is not True:
                    semantic_record_failures += 1
        baseline_stable = {
            "seed": seed,
            "summary": stable,
            "records": [_semantic_record(record) for record in records],
        }
        baseline_digest.update(_canonical_json(baseline_stable))
        if candidate_records is not None and candidate_digest is not None:
            candidate_stable = {
                "seed": seed,
                "summary": stable,
                "records": [_semantic_record(record) for record in candidate_records],
            }
            candidate_digest.update(_canonical_json(candidate_stable))

    result: dict[str, Any] = {
        "schema": SCHEMA,
        "baseline_binary": baseline_metadata,
        "seed_offset": seed_offset,
        "seeds": seeds,
        "fixture_plies": fixture_plies,
        "states_checked": seeds,
        "setup_actions": setup_action_total,
        "legal_actions": legal_action_total,
        "workloads": list(WORKLOADS),
        "corpus_digest": baseline_digest.hexdigest(),
        "semantic_record_checks": semantic_record_checks,
        "semantic_record_failures": semantic_record_failures,
        "exact_hash_oracle_checks_per_binary": (
            seeds * EXACT_HASH_ORACLE_CHECKS_PER_SEED
        ),
        "exact_hash_oracle_checks_total": (
            seeds
            * EXACT_HASH_ORACLE_CHECKS_PER_SEED
            * (2 if candidate_metadata is not None else 1)
        ),
        "hash_oracle_failures": 0,
        "semantic_equal": semantic_record_failures == 0,
    }
    if candidate_metadata is not None and candidate_digest is not None:
        result["candidate_binary"] = candidate_metadata
        result["candidate_corpus_digest"] = candidate_digest.hexdigest()
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, help="validate one binary")
    parser.add_argument("--baseline-binary", type=Path)
    parser.add_argument("--candidate-binary", type=Path)
    parser.add_argument("--seeds", type=int, default=1000)
    parser.add_argument("--seed-offset", type=int, default=0)
    parser.add_argument("--fixture-plies", type=int, default=32)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if args.binary is not None:
        if args.baseline_binary is not None or args.candidate_binary is not None:
            parser.error("--binary cannot be combined with paired binary options")
        baseline_binary = args.binary
        candidate_binary = None
    else:
        if args.baseline_binary is None or args.candidate_binary is None:
            parser.error(
                "use --binary, or provide both --baseline-binary and --candidate-binary"
            )
        baseline_binary = args.baseline_binary
        candidate_binary = args.candidate_binary
    result = validate(
        baseline_binary,
        args.seeds,
        args.seed_offset,
        args.fixture_plies,
        candidate_binary=candidate_binary,
        timeout=args.timeout,
    )
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        atomic_write_json(args.output, result)
    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
