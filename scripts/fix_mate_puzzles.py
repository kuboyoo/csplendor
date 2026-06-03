#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import shutil
import sys
import tempfile
import time
from typing import Any, Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from csplendor.api.usi_kifu import now_iso, spn_to_game

from scripts.dfpn_mate_solver import principal_line_to_kifu_text, solve_reveal_verified_mate
from scripts.mate_solver import MATE, SolverOptions


@dataclass
class ConversionStats:
    scanned: int = 0
    converted: int = 0
    skipped: int = 0
    skipped_inexact_hidden: int = 0
    failed: int = 0
    incomplete_dag: int = 0
    no_mate: int = 0
    elapsed_ms: float = 0.0


class InexactHiddenPositionError(ValueError):
    pass


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _problem_paths(input_dir: Path) -> list[Path]:
    return sorted(input_dir.glob("depth_*/*/problem.json"))


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _output_problem_dir(output_dir: Path, depth: int, puzzle_id: str) -> Path:
    return output_dir / f"depth_{depth:02d}" / puzzle_id


def _max_defender_responses(dag: dict[str, Any], defender: int) -> int:
    return max(
        (
            len(node.get("children", []))
            for node in dag.get("nodes", [])
            if int(node.get("player", -1)) == int(defender)
        ),
        default=0,
    )


def _write_manifest(output_dir: Path, problem: dict[str, Any]) -> None:
    manifest: dict[str, Any] = {
        "id": problem["id"],
        "path": str(_output_problem_dir(
            output_dir,
            int(problem["forced_win_depth"]),
            str(problem["id"]),
        ).relative_to(output_dir)),
        "attacker": int(problem["attacker"]),
        "forced_win_depth": int(problem["forced_win_depth"]),
        "initial_scores": problem["initial_scores"],
        "generated_at": problem.get("generated_at"),
    }
    if "quality" in problem:
        manifest["quality"] = problem["quality"]
    with (output_dir / "manifest.jsonl").open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(manifest, ensure_ascii=False, sort_keys=True) + "\n")


def convert_problem(
    problem_path: Path,
    output_dir: Path,
    *,
    time_limit: float,
    node_limit: int,
    proof_dag_node_limit: int,
    proof_dag_edge_limit: int,
    overwrite: bool,
) -> Optional[Path]:
    source_problem = _load_json(problem_path)
    position = str(source_problem["position"])
    if "?L" in position:
        raise InexactHiddenPositionError(
            "SPN hidden reserved cards (?Lx) cannot be solved exactly; use explicit card ids"
        )
    puzzle_id = str(source_problem["id"])
    game = spn_to_game(position)
    attacker = int(source_problem.get("attacker", game.board.current_player))
    if attacker != int(game.board.current_player):
        raise ValueError(
            f"attacker does not match current player: {problem_path} "
            f"attacker={attacker} current={int(game.board.current_player)}"
        )

    result = solve_reveal_verified_mate(
        game,
        attacker=attacker,
        options=SolverOptions(
            max_nodes=node_limit,
            time_limit=time_limit,
            include_proof=True,
            allow_deck_reserve=True,
        ),
        include_proof_dag=True,
        proof_dag_node_limit=proof_dag_node_limit,
        proof_dag_edge_limit=proof_dag_edge_limit,
    )
    if result.status != MATE or result.proof_tree is None:
        raise RuntimeError(f"mate was not reproven: status={result.status}")

    proof = result.proof_tree
    verification = proof.get("verification")
    if not isinstance(verification, dict):
        raise RuntimeError("verification payload is missing")
    dag = verification.get("proof_dag")
    if not isinstance(dag, dict) or not bool(dag.get("complete")):
        reason = dag.get("omitted_reason") if isinstance(dag, dict) else None
        raise RuntimeError(f"complete proof DAG was not produced: {reason}")
    line = proof.get("line")
    if not isinstance(line, list):
        raise RuntimeError("principal line is missing")

    depth = int(proof.get("forced_win_depth", result.depth))
    target_dir = _output_problem_dir(output_dir, depth, puzzle_id)
    if target_dir.exists() and not overwrite:
        return None

    converted_at = now_iso()
    problem = dict(source_problem)
    problem["format"] = "csplendor_mate_problem_v1"
    problem["id"] = puzzle_id
    problem["position"] = position
    problem["attacker"] = attacker
    problem["forced_win_depth"] = depth
    problem["initial_scores"] = [int(score) for score in game.scores]
    problem["answer_files"] = {
        "principal_line": "answer.kifu",
        "strategy_dag": "strategy.json",
    }
    problem["search_stats"] = asdict(result.stats)
    problem["conversion"] = {
        "converted_at": converted_at,
        "source_problem": str(problem_path),
        "source_format": source_problem.get("format"),
        "tool": "scripts/fix_mate_puzzles.py",
    }

    quality = dict(problem.get("quality") or {})
    quality["strategy_dag_nodes"] = len(dag.get("nodes", []))
    quality["max_defender_responses"] = _max_defender_responses(dag, 1 - attacker)
    problem["quality"] = quality

    strategy = {
        "format": "csplendor_mate_strategy_v1",
        "problem_id": puzzle_id,
        "position": position,
        "attacker": attacker,
        "forced_win_depth": depth,
        "assumptions": proof.get("assumptions"),
        "principal_line": line,
        "verification": {
            "all_reveals_verified": verification.get("all_reveals_verified"),
            "reason": verification.get("reason"),
            "stats": verification.get("stats"),
        },
        "strategy_dag": dag,
        "conversion": problem["conversion"],
    }
    kifu = principal_line_to_kifu_text(
        game,
        line,
        attacker=attacker,
        reveal_hidden_reserved_ids=True,
    )

    depth_dir = target_dir.parent
    depth_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = Path(tempfile.mkdtemp(prefix=f".{puzzle_id}.", dir=depth_dir))
    try:
        (tmp_dir / "problem.json").write_text(_json_text(problem), encoding="utf-8")
        (tmp_dir / "strategy.json").write_text(_json_text(strategy), encoding="utf-8")
        (tmp_dir / "answer.kifu").write_text(kifu, encoding="utf-8")
        if target_dir.exists():
            shutil.rmtree(target_dir)
        tmp_dir.replace(target_dir)
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise

    _write_manifest(output_dir, problem)
    return target_dir


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Rebuild generated mate puzzle DAGs with the current strategy format."
    )
    parser.add_argument("--input-dir", default="generated/mate_puzzles")
    parser.add_argument("--output-dir", default="generated/mate_puzzles2")
    parser.add_argument("--time-limit", type=float, default=30.0)
    parser.add_argument("--node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--proof-dag-node-limit", type=int, default=300000)
    parser.add_argument("--proof-dag-edge-limit", type=int, default=1000000)
    parser.add_argument("--limit", type=int, default=0, help="0 converts all problems")
    parser.add_argument(
        "--only-id",
        action="append",
        default=[],
        help="convert only the specified puzzle id; can be repeated",
    )
    parser.add_argument("--overwrite", action="store_true")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.time_limit < 0 or args.node_limit < 0:
        raise ValueError("search limits must be non-negative")
    if args.proof_dag_node_limit < 0 or args.proof_dag_edge_limit < 0:
        raise ValueError("proof DAG limits must be non-negative")
    if args.limit < 0:
        raise ValueError("limit must be non-negative")

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    if args.overwrite:
        for name in ("manifest.jsonl", "skipped_inexact_hidden.jsonl"):
            path = output_dir / name
            if path.exists():
                path.unlink()

    paths = _problem_paths(input_dir)
    if args.only_id:
        only_ids = {str(value) for value in args.only_id}
        paths = [path for path in paths if path.parent.name in only_ids]
    if args.limit:
        paths = paths[: args.limit]

    stats = ConversionStats(scanned=len(paths))
    started = time.monotonic()
    for index, path in enumerate(paths, start=1):
        print(f"[{index}/{len(paths)}] {path}", file=sys.stderr, flush=True)
        try:
            target = convert_problem(
                path,
                output_dir,
                time_limit=float(args.time_limit),
                node_limit=int(args.node_limit),
                proof_dag_node_limit=int(args.proof_dag_node_limit),
                proof_dag_edge_limit=int(args.proof_dag_edge_limit),
                overwrite=bool(args.overwrite),
            )
        except InexactHiddenPositionError as exc:
            stats.skipped += 1
            stats.skipped_inexact_hidden += 1
            with (output_dir / "skipped_inexact_hidden.jsonl").open("a", encoding="utf-8") as fh:
                fh.write(json.dumps({
                    "problem": str(path),
                    "reason": str(exc),
                }, ensure_ascii=False, sort_keys=True) + "\n")
            print(f"[skipped] {path}: {exc}", file=sys.stderr, flush=True)
            continue
        except RuntimeError as exc:
            stats.failed += 1
            message = str(exc)
            if "complete proof DAG" in message:
                stats.incomplete_dag += 1
            elif "mate was not reproven" in message:
                stats.no_mate += 1
            print(f"[failed] {path}: {exc}", file=sys.stderr, flush=True)
            continue
        except Exception as exc:
            stats.failed += 1
            print(f"[failed] {path}: {exc}", file=sys.stderr, flush=True)
            continue

        if target is None:
            stats.skipped += 1
            print("[skipped] already exists", file=sys.stderr, flush=True)
        else:
            stats.converted += 1
            print(f"[converted] {target}", file=sys.stderr, flush=True)

    stats.elapsed_ms = (time.monotonic() - started) * 1000.0
    print(_json_text(asdict(stats)))
    return 0 if stats.failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
