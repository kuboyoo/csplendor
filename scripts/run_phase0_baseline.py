#!/usr/bin/env python3
"""Run the Phase-0 hot-path baseline as isolated 22-pair crossover cases."""

from __future__ import annotations

import argparse
import csv
import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence

try:
    from benchmark_manifest import atomic_write_json
    from run_paired_benchmarks import (
        DEFAULT_BOOTSTRAP_ITERATIONS,
        DEFAULT_WARMUPS,
        parse_cpu_set,
        run_paired,
    )
except ModuleNotFoundError:  # Importing as scripts.run_phase0_baseline.
    from scripts.benchmark_manifest import atomic_write_json
    from scripts.run_paired_benchmarks import (
        DEFAULT_BOOTSTRAP_ITERATIONS,
        DEFAULT_WARMUPS,
        parse_cpu_set,
        run_paired,
    )


SCHEMA = "csplendor.phase0_baseline.v1"
DEFAULT_PHASE0_PAIRS = 22


@dataclass(frozen=True)
class Case:
    workload: str
    fixture: str
    iterations: int
    warmup: int
    extra: tuple[str, ...] = ()


CASES = (
    Case("legal_count", "midgame_250", 100_000, 10_000),
    Case("legal_codes", "midgame_250", 20_000, 2_000),
    Case("legal_actions", "midgame_250", 20_000, 2_000),
    Case("random_selfplay_apply", "initial", 250_000, 25_000),
    Case("apply_only", "midgame_250", 2_000_000, 200_000),
    Case("purchase_apply", "midgame_250", 2_000_000, 200_000),
    Case("apply_exact_hash", "midgame_250", 2_000_000, 200_000),
    Case(
        "apply_observable_hash",
        "midgame_250",
        2_000_000,
        200_000,
        ("--observer", "0"),
    ),
    Case("cold_hash", "midgame_250", 5_000_000, 500_000),
    Case("cached_hash", "midgame_250", 20_000_000, 2_000_000),
    Case("clone_light", "midgame_250", 2_000_000, 200_000),
    Case("determinization_clone", "hidden_reserve", 200_000, 20_000),
    Case("state_encoder", "midgame_250", 500_000, 50_000),
    Case("action_mask", "midgame_250", 20_000_000, 2_000_000),
    Case("decode_apply", "midgame_250", 2_000_000, 200_000),
    Case(
        "legacy_mcts",
        "midgame_250",
        65_536,
        4_096,
        ("--batch-size", "16", "--retained-tree", "false"),
    ),
    Case(
        "shared_tree",
        "midgame_250",
        2_000_000,
        200_000,
        ("--tree-backend", "sharded"),
    ),
    # 65,536 can exhaust the engine's 65,536-node hard limit after root
    # bootstrap; 4,096 is the completed-simulation acceptance fixture.
    Case(
        "parallel_scheduler",
        "midgame_250",
        4_096,
        256,
        (
            "--threads",
            "1",
            "--batch-size",
            "16",
            "--latency-us",
            "0",
            "--tree-backend",
            "sharded",
        ),
    ),
    Case("board_copy_restore", "midgame_250", 2_000_000, 200_000),
    Case("solver_state_key", "reveal_heavy", 2_000_000, 200_000, ("--depth", "7")),
    Case("solver_tt", "midgame_250", 20_000_000, 2_000_000, ("--depth", "7")),
    Case("visible_solver", "five_moves", 100_000, 10_000, ("--time-limit", "0")),
    Case(
        "exact_reveal",
        "five_moves",
        1_000_000,
        100_000,
        (
            "--depth",
            "5",
            "--time-limit",
            "0",
            "--proof-dag",
            "false",
            "--persistent-reuse",
            "false",
        ),
    ),
)


def case_command(binary: Path, case: Case) -> list[str]:
    return [
        str(binary.resolve()),
        "--workload",
        case.workload,
        "--fixture",
        case.fixture,
        "--iterations",
        str(case.iterations),
        "--warmup",
        str(case.warmup),
        *case.extra,
    ]


def _atomic_write_csv(path: Path, rows: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "workload",
        "fixture",
        "operations",
        "A_median_rate_per_second",
        "B_median_rate_per_second",
        "B_over_A_median",
        "ci95_low",
        "ci95_high",
        "A_p95_rate_per_second",
        "B_p95_rate_per_second",
        "A_median_runner_rss_kib",
        "B_median_runner_rss_kib",
        "digest",
    ]
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary = Path(stream.name)
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _csv_row(result: dict[str, Any]) -> dict[str, Any]:
    comparison = result["comparison"][0]
    absolute = comparison["absolute"]
    ratio = comparison["B_over_A"]
    baseline_rss = absolute.get("A_runner_rss_kib", {})
    candidate_rss = absolute.get("B_runner_rss_kib", {})
    first_record = result["pairs"][0]["A"]["records"][0]
    confidence_interval = ratio.get(
        "crossover_block_bootstrap_ci95", ratio.get("paired_bootstrap_ci95")
    )
    if confidence_interval is None:
        raise ValueError("paired result has no supported confidence interval")
    return {
        "workload": comparison["workload"],
        "fixture": comparison["fixture"],
        "operations": first_record["operations"],
        "A_median_rate_per_second": absolute["A_rate_per_second"]["median"],
        "B_median_rate_per_second": absolute["B_rate_per_second"]["median"],
        "B_over_A_median": ratio["median"],
        "ci95_low": confidence_interval[0],
        "ci95_high": confidence_interval[1],
        "A_p95_rate_per_second": absolute["A_rate_per_second"]["p95"],
        "B_p95_rate_per_second": absolute["B_rate_per_second"]["p95"],
        "A_median_runner_rss_kib": baseline_rss.get("median"),
        "B_median_runner_rss_kib": candidate_rss.get("median"),
        "digest": comparison["digest"],
    }


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-binary", required=True, type=Path)
    parser.add_argument("--candidate-binary", required=True, type=Path)
    parser.add_argument("--baseline-repo-root", required=True, type=Path)
    parser.add_argument("--candidate-repo-root", required=True, type=Path)
    parser.add_argument("--output-json", required=True, type=Path)
    parser.add_argument("--output-csv", required=True, type=Path)
    parser.add_argument("--pairs", type=int, default=DEFAULT_PHASE0_PAIRS)
    parser.add_argument("--warmups", type=int, default=DEFAULT_WARMUPS)
    parser.add_argument(
        "--bootstrap-iterations",
        type=int,
        default=DEFAULT_BOOTSTRAP_ITERATIONS,
    )
    parser.add_argument("--cpu-set", type=parse_cpu_set, default=parse_cpu_set("4"))
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--case", action="append", dest="case_names")
    parser.add_argument("--list-cases", action="store_true")
    parser.add_argument(
        "--rotate-binary-slots",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="cross A/B over two fixed-inode executable slots (default: enabled)",
    )
    args = parser.parse_args(argv)

    if args.list_cases:
        for case in CASES:
            print(case.workload)
        return 0
    if args.pairs < 1 or args.warmups < 0 or args.bootstrap_iterations < 1:
        parser.error("pairs/bootstrap must be positive and warmups non-negative")
    if args.rotate_binary_slots and args.pairs % 2:
        parser.error("binary-slot rotation requires an even number of pairs")

    selected_names = set(args.case_names or ())
    unknown = selected_names - {case.workload for case in CASES}
    if unknown:
        parser.error("unknown cases: " + ", ".join(sorted(unknown)))
    selected = [
        case for case in CASES if not selected_names or case.workload in selected_names
    ]

    payload: dict[str, Any] = {
        "schema": SCHEMA,
        "complete": False,
        "settings": {
            "pairs": args.pairs,
            "warmups_per_side": args.warmups,
            "bootstrap_iterations": args.bootstrap_iterations,
            "cpu_set": args.cpu_set,
            "timeout_seconds": args.timeout,
            "order": "ABBA",
            "rotate_binary_slots": args.rotate_binary_slots,
            "statistical_unit": (
                "two_pair_crossover_block" if args.rotate_binary_slots else "pair"
            ),
            "binary_slot_policy": (
                "two_private_fixed_inodes_crossed_every_pair"
                if args.rotate_binary_slots
                else "disabled"
            ),
        },
        "cases": [],
    }
    csv_rows: list[dict[str, Any]] = []
    for index, case in enumerate(selected, 1):
        print(
            f"[{index}/{len(selected)}] {case.workload} ({case.fixture})",
            file=sys.stderr,
            flush=True,
        )
        result = run_paired(
            case_command(args.baseline_binary, case),
            case_command(args.candidate_binary, case),
            pairs=args.pairs,
            warmups=args.warmups,
            cpu_set=args.cpu_set,
            timeout=args.timeout,
            bootstrap_iterations=args.bootstrap_iterations,
            baseline_repo_root=args.baseline_repo_root,
            candidate_repo_root=args.candidate_repo_root,
            rotate_binary_slots=args.rotate_binary_slots,
        )
        payload["cases"].append(result)
        csv_rows.append(_csv_row(result))
        atomic_write_json(args.output_json, payload)

    payload["complete"] = True
    atomic_write_json(args.output_json, payload)
    _atomic_write_csv(args.output_csv, csv_rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
