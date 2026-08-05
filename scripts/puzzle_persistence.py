"""Atomic persistence of generated mate problems and strategy artifacts."""

from __future__ import annotations

import hashlib
import json
import shutil
import tempfile
from dataclasses import asdict
from pathlib import Path
from typing import Optional

import csplendor as cs
from csplendor.api.usi_kifu import game_to_spn, now_iso
from scripts.dfpn_mate_solver import (
    compact_proof_dag_to_v1,
    principal_line_to_kifu_text,
    proof_dag_to_compact,
)
from scripts.mate_solver import SearchResult


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _position_id(position: str) -> str:
    return hashlib.sha256(position.encode("utf-8")).hexdigest()[:16]


def save_puzzle(
    output_dir: Path,
    game: cs.Game,
    result: SearchResult,
    *,
    game_seed: int,
    attempt: int,
    quality: Optional[dict[str, object]] = None,
    strategy_dag_format: str = "compact",
) -> Optional[Path]:
    if strategy_dag_format not in {"compact", "v1", "both"}:
        raise ValueError("strategy_dag_format must be compact, v1, or both")
    proof = result.proof_tree or {}
    verification = proof.get("verification")
    dag = verification.get("proof_dag") if isinstance(verification, dict) else None
    line = proof.get("line")
    if not isinstance(dag, dict) or not bool(dag.get("complete")):
        raise ValueError("complete strategy DAG is required")
    if not isinstance(line, list):
        raise ValueError("principal line is required")

    depth = int(proof.get("forced_win_depth", result.depth))
    position = game_to_spn(
        game,
        reveal_hidden_reserved_ids=True,
        require_purchased_card_ids=True,
    )
    puzzle_id = _position_id(position)
    depth_dir = output_dir / f"depth_{depth:02d}"
    puzzle_dir = depth_dir / puzzle_id
    if puzzle_dir.exists():
        return None

    generated_at = now_iso()
    attacker = int(game.board.current_player)
    problem = {
        "format": "csplendor_mate_problem_v1",
        "id": puzzle_id,
        "generated_at": generated_at,
        "generation": {
            "attempt": int(attempt),
            "game_seed": int(game_seed),
        },
        "position": position,
        "attacker": attacker,
        "forced_win_depth": depth,
        "initial_scores": [int(score) for score in game.scores],
        "answer_files": {
            "principal_line": "answer.kifu",
            "strategy_dag": "strategy.json",
        },
        "search_stats": asdict(result.stats),
    }
    if quality is not None:
        problem["quality"] = quality
    if strategy_dag_format == "compact":
        output_dag = proof_dag_to_compact(dag)
        extra_strategy_fields: dict[str, object] = {}
    elif strategy_dag_format == "both":
        output_dag = compact_proof_dag_to_v1(dag)
        extra_strategy_fields = {
            "strategy_dag_compact": proof_dag_to_compact(dag),
        }
    else:
        output_dag = compact_proof_dag_to_v1(dag)
        extra_strategy_fields = {}
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
        "strategy_dag": output_dag,
    }
    strategy.update(extra_strategy_fields)
    kifu = principal_line_to_kifu_text(
        game,
        line,
        attacker=attacker,
        reveal_hidden_reserved_ids=True,
    )

    depth_dir.mkdir(parents=True, exist_ok=True)
    tmp_dir = Path(tempfile.mkdtemp(prefix=f".{puzzle_id}.", dir=depth_dir))
    try:
        (tmp_dir / "problem.json").write_text(_json_text(problem), encoding="utf-8")
        (tmp_dir / "strategy.json").write_text(_json_text(strategy), encoding="utf-8")
        (tmp_dir / "answer.kifu").write_text(kifu, encoding="utf-8")
        tmp_dir.replace(puzzle_dir)
    except Exception:
        shutil.rmtree(tmp_dir, ignore_errors=True)
        raise

    manifest = {
        "id": puzzle_id,
        "path": puzzle_dir.relative_to(output_dir).as_posix(),
        "attacker": attacker,
        "forced_win_depth": depth,
        "initial_scores": problem["initial_scores"],
        "generated_at": generated_at,
    }
    if quality is not None:
        manifest["quality"] = quality
    with (output_dir / "manifest.jsonl").open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(manifest, ensure_ascii=False, sort_keys=True) + "\n")
    return puzzle_dir


__all__ = ["save_puzzle"]
