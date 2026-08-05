#!/usr/bin/env python3
"""Repeatable R6 benchmark for native solvers and Python DFPN."""

from __future__ import annotations

import argparse
import json
import os
import resource
import statistics
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Tuple

REPO_ROOT = Path(
    os.environ.get(
        "CSPLENDOR_BENCH_REPO_ROOT",
        Path(__file__).resolve().parents[1],
    )
).resolve()
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs  # noqa: E402
from scripts.dfpn_mate_solver import solve_game_dfpn  # noqa: E402
from scripts.mate_solver import (  # noqa: E402
    SolverOptions,
    load_game_from_usi_text,
)

BENCH_POSITION = (
    "position bank:W1U3G3R3K0D4 | "
    "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
    "decks:36,23,15 | nobles:[1,10,6] | "
    "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;points:5;"
    "nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
    "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;points:8;"
    "nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
)


def _visible_fixture() -> cs.Game:
    game = cs.Game(seed=0)
    game.board.visible = [
        [7, -1, -1, -1],
        [-1, -1, -1, -1],
        [-1, -1, -1, -1],
    ]
    game.board.decks = [[15], [], []]
    game.board.bank = [0, 0, 0, 0, 0, 0]
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(0, player)
    return game


def _reveal_fixture() -> cs.Game:
    game = cs.Game(seed=0)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(1, player)
    return game


def _sample(
    operation: Callable[[], int],
    *,
    calls: int,
    repetitions: int,
) -> Dict[str, object]:
    operation()
    samples: List[Dict[str, float]] = []
    for _ in range(repetitions):
        nodes = 0
        started = time.perf_counter()
        for _ in range(calls):
            nodes += operation()
        elapsed = time.perf_counter() - started
        samples.append(
            {
                "nodes": float(nodes),
                "seconds": elapsed,
                "nodes_per_second": nodes / elapsed,
            }
        )
    return {
        "calls_per_sample": calls,
        "repetitions": repetitions,
        "median_seconds": statistics.median(sample["seconds"] for sample in samples),
        "median_nodes_per_second": statistics.median(
            sample["nodes_per_second"] for sample in samples
        ),
        "samples": samples,
    }


def run_benchmarks(args: argparse.Namespace) -> Dict[str, object]:
    workloads: Dict[str, Tuple[Callable[[], int], int]] = {}
    if args.workload in {"all", "visible"}:
        game = _visible_fixture()

        def visible(game: cs.Game = game) -> int:
            result = cs.solve_visible_only_winner_cpp(
                game,
                max_nodes=100,
                time_limit_seconds=0.0,
            )
            return int(result["stats"]["nodes"])

        workloads["visible_only"] = (visible, args.visible_calls)
    if args.workload in {"all", "reveal"}:
        game = _reveal_fixture()

        def reveal(game: cs.Game = game) -> int:
            result = cs.solve_reveal_verified_mate_cpp(
                game,
                attacker=1,
                depth=1,
                max_nodes=0,
                time_limit_seconds=0.0,
                include_proof_dag=False,
            )
            return int(result["stats"]["nodes"])

        workloads["reveal_verified"] = (reveal, args.reveal_calls)
    if args.workload in {"all", "dfpn"}:
        game = load_game_from_usi_text(BENCH_POSITION)

        def dfpn(game: cs.Game = game) -> int:
            result = solve_game_dfpn(
                game,
                attacker=0,
                max_depth=5,
                options=SolverOptions(
                    max_nodes=args.dfpn_node_limit,
                    time_limit=0.0,
                    include_proof=False,
                ),
            )
            return int(result.stats.nodes)

        workloads["python_dfpn"] = (dfpn, 1)

    results = {
        name: _sample(
            operation,
            calls=calls,
            repetitions=args.repetitions,
        )
        for name, (operation, calls) in workloads.items()
    }
    return {
        "schema": "csplendor_solver_benchmark_v1",
        "workloads": results,
        "peak_rss_kib": int(resource.getrusage(resource.RUSAGE_SELF).ru_maxrss),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--workload",
        choices=("all", "visible", "reveal", "dfpn"),
        default="all",
    )
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--visible-calls", type=int, default=200)
    parser.add_argument("--reveal-calls", type=int, default=20)
    parser.add_argument("--dfpn-node-limit", type=int, default=1000)
    return parser


def main() -> int:
    args = build_parser().parse_args()
    if args.repetitions <= 0:
        raise SystemExit("--repetitions must be positive")
    print(json.dumps(run_benchmarks(args), indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
