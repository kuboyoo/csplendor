#!/usr/bin/env python3
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import random
import shutil
import sys
import tempfile
from typing import Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, game_to_spn, now_iso

from scripts.dfpn_mate_solver import principal_line_to_kifu_text, solve_reveal_verified_mate
from scripts.mate_solver import MATE, SearchResult, SolverOptions


@dataclass
class GenerationStats:
    attempts: int = 0
    candidates: int = 0
    mates: int = 0
    saved: int = 0
    duplicates: int = 0
    incomplete_dags: int = 0
    unknown: int = 0
    filtered: int = 0
    countermate_checks: int = 0
    countermates: int = 0
    errors: int = 0


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _position_id(position: str) -> str:
    return hashlib.sha256(position.encode("utf-8")).hexdigest()[:16]


def _weighted_random_action(game: cs.Game, rng: random.Random) -> cs.Action:
    actions = list(game.legal_actions)
    nobles = [
        action for action in actions
        if int(action.type) == int(cs.ActionType.VISIT_NOBLE)
    ]
    if nobles:
        return rng.choice(nobles)

    weights = []
    for action in actions:
        action_type = int(action.type)
        if action_type == int(cs.ActionType.PURCHASE):
            weights.append(14 + 10 * int(cs.get_card(int(action.card_id)).points))
        elif action_type == int(cs.ActionType.RESERVE_VISIBLE):
            weights.append(4)
        elif action_type == int(cs.ActionType.RESERVE_DECK):
            weights.append(2)
        else:
            weights.append(8)
    return rng.choices(actions, weights=weights, k=1)[0]


def generate_candidate_position(
    rng: random.Random,
    *,
    game_seed: int,
    min_playout_plies: int,
    max_playout_plies: int,
) -> Optional[cs.Game]:
    game = cs.Game(seed=game_seed)
    plies = rng.randint(min_playout_plies, max_playout_plies)
    for _ in range(plies):
        if game.is_game_over() or not game.legal_actions:
            return None
        if not game.apply(_weighted_random_action(game, rng), False):
            raise RuntimeError("engine rejected a generated legal action")
    return game


def is_suspicious_position(
    game: cs.Game,
    *,
    min_attacker_points: int,
    max_attacker_points: int,
    min_defender_points: int,
    max_score_gap: int,
    allow_final_round: bool,
) -> bool:
    if game.is_game_over() or not game.legal_actions:
        return False
    if not allow_final_round and bool(game.board.final_round):
        return False
    attacker = int(game.board.current_player)
    defender = 1 - attacker
    attacker_points = int(game.board.get_player(attacker).points)
    defender_points = int(game.board.get_player(defender).points)
    return (
        min_attacker_points <= attacker_points <= max_attacker_points
        and defender_points >= min_defender_points
        and abs(attacker_points - defender_points) <= max_score_gap
    )


def _completed_turn_children(game: cs.Game, action: cs.Action) -> list[cs.Game]:
    child = game.clone_light()
    if not child.apply(action, False):
        raise RuntimeError("engine rejected a generated legal action")
    if child.is_game_over() or int(child.board.current_player) != int(game.board.current_player):
        return [child]
    if not bool(child.board.waiting_noble):
        return []

    children = []
    for noble_action in child.legal_actions:
        noble_child = child.clone_light()
        if not noble_child.apply(noble_action, False):
            raise RuntimeError("engine rejected a generated noble choice")
        children.append(noble_child)
    return children


def find_countermate_blunders(
    game: cs.Game,
    result: SearchResult,
    *,
    min_losing_alternatives: int,
    action_limit: int,
    node_limit: int,
    time_limit: float,
) -> tuple[list[dict[str, object]], int]:
    proof = result.proof_tree or {}
    verification = proof.get("verification")
    line = verification.get("line") if isinstance(verification, dict) else None
    if not isinstance(line, list) or not line:
        line = proof.get("line")
    if not isinstance(line, list) or not line:
        return [], 0
    action_info = line[0].get("action")
    if not isinstance(action_info, dict) or "pack" not in action_info:
        return [], 0

    correct_pack = int(action_info["pack"])
    attacker = int(game.board.current_player)
    defender = 1 - attacker
    alternatives = [
        action for action in game.legal_actions
        if int(action.pack()) != correct_pack
    ]
    if action_limit:
        alternatives = alternatives[:action_limit]

    blunders = []
    checks = 0
    for action in alternatives:
        for child in _completed_turn_children(game, action):
            if child.is_game_over() or int(child.board.current_player) != defender:
                continue
            checks += 1
            countermate = solve_reveal_verified_mate(
                child,
                attacker=defender,
                options=SolverOptions(
                    max_nodes=node_limit,
                    time_limit=time_limit,
                    include_proof=True,
                    allow_deck_reserve=True,
                ),
            )
            if countermate.status != MATE:
                continue
            blunders.append({
                "action": action_to_usi(action, game=game),
                "opponent": defender,
                "forced_win_depth": int(countermate.depth),
            })
            break
        if len(blunders) >= min_losing_alternatives:
            break
    return blunders, checks


def save_puzzle(
    output_dir: Path,
    game: cs.Game,
    result: SearchResult,
    *,
    game_seed: int,
    attempt: int,
    quality: Optional[dict[str, object]] = None,
) -> Optional[Path]:
    proof = result.proof_tree or {}
    verification = proof.get("verification")
    dag = verification.get("proof_dag") if isinstance(verification, dict) else None
    line = proof.get("line")
    if not isinstance(dag, dict) or not bool(dag.get("complete")):
        raise ValueError("complete strategy DAG is required")
    if not isinstance(line, list):
        raise ValueError("principal line is required")

    depth = int(proof.get("forced_win_depth", result.depth))
    position = game_to_spn(game)
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
    }
    kifu = principal_line_to_kifu_text(game, line, attacker=attacker)

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
        "path": str(puzzle_dir.relative_to(output_dir)),
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


def generate_puzzles(args: argparse.Namespace) -> GenerationStats:
    rng = random.Random(args.seed)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stats = GenerationStats()

    while stats.saved < args.count and stats.attempts < args.max_attempts:
        stats.attempts += 1
        game_seed = rng.getrandbits(32)
        try:
            game = generate_candidate_position(
                rng,
                game_seed=game_seed,
                min_playout_plies=args.min_playout_plies,
                max_playout_plies=args.max_playout_plies,
            )
            if game is None or not is_suspicious_position(
                game,
                min_attacker_points=args.min_attacker_points,
                max_attacker_points=args.max_attacker_points,
                min_defender_points=args.min_defender_points,
                max_score_gap=args.max_score_gap,
                allow_final_round=args.allow_final_round,
            ):
                stats.filtered += 1
                continue

            stats.candidates += 1
            result = solve_reveal_verified_mate(
                game,
                attacker=int(game.board.current_player),
                options=SolverOptions(
                    max_nodes=args.node_limit,
                    time_limit=args.time_limit,
                    include_proof=True,
                    allow_deck_reserve=True,
                ),
                include_proof_dag=True,
                proof_dag_node_limit=args.proof_dag_node_limit,
                proof_dag_edge_limit=args.proof_dag_edge_limit,
            )
            if result.status != MATE or result.proof_tree is None:
                stats.unknown += 1
                continue

            stats.mates += 1
            depth = int(result.proof_tree["forced_win_depth"])
            if depth < args.min_depth or (args.max_depth and depth > args.max_depth):
                stats.filtered += 1
                continue
            dag = result.proof_tree["verification"].get("proof_dag", {})
            if not bool(dag.get("complete")):
                stats.incomplete_dags += 1
                continue

            blunders, checks = find_countermate_blunders(
                game,
                result,
                min_losing_alternatives=args.min_losing_alternatives,
                action_limit=args.countermate_action_limit,
                node_limit=args.countermate_node_limit,
                time_limit=args.countermate_time_limit,
            )
            stats.countermate_checks += checks
            stats.countermates += len(blunders)
            if len(blunders) < args.min_losing_alternatives:
                stats.filtered += 1
                continue

            scores = [int(score) for score in game.scores]
            quality = {
                "score_gap": abs(scores[0] - scores[1]),
                "countermate_blunders": blunders,
            }
            puzzle_dir = save_puzzle(
                output_dir,
                game,
                result,
                game_seed=game_seed,
                attempt=stats.attempts,
                quality=quality,
            )
            if puzzle_dir is None:
                stats.duplicates += 1
            else:
                stats.saved += 1
                print(f"[saved {stats.saved}/{args.count}] {puzzle_dir}")
        except Exception as exc:
            stats.errors += 1
            print(f"[attempt {stats.attempts}] {type(exc).__name__}: {exc}", file=sys.stderr)
        finally:
            if args.progress_interval and stats.attempts % args.progress_interval == 0:
                print(f"[progress] {json.dumps(asdict(stats), sort_keys=True)}", file=sys.stderr)
    return stats


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate reveal-verified Splendor mate puzzles and compact complete response DAGs."
    )
    parser.add_argument("--output-dir", default="generated/mate_puzzles")
    parser.add_argument("--count", type=int, default=100, help="number of puzzles to save")
    parser.add_argument("--max-attempts", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=0, help="generator RNG seed")
    parser.add_argument("--min-playout-plies", type=int, default=18)
    parser.add_argument("--max-playout-plies", type=int, default=48)
    parser.add_argument("--min-attacker-points", type=int, default=8)
    parser.add_argument("--max-attacker-points", type=int, default=14)
    parser.add_argument("--min-defender-points", type=int, default=8)
    parser.add_argument("--max-score-gap", type=int, default=3)
    parser.add_argument("--allow-final-round", action="store_true")
    parser.add_argument("--min-depth", type=int, default=1)
    parser.add_argument("--max-depth", type=int, default=0, help="0 disables the upper bound")
    parser.add_argument("--node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--time-limit", type=float, default=30.0, help="seconds per candidate")
    parser.add_argument("--proof-dag-node-limit", type=int, default=100000)
    parser.add_argument("--proof-dag-edge-limit", type=int, default=500000)
    parser.add_argument("--min-losing-alternatives", type=int, default=1)
    parser.add_argument("--countermate-action-limit", type=int, default=12, help="0 checks all wrong moves")
    parser.add_argument("--countermate-node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--countermate-time-limit", type=float, default=5.0, help="seconds per wrong move")
    parser.add_argument("--progress-interval", type=int, default=100)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.count < 0 or args.max_attempts < 0:
        raise ValueError("count and max-attempts must be non-negative")
    if args.min_playout_plies < 0 or args.min_playout_plies > args.max_playout_plies:
        raise ValueError("invalid playout ply range")
    if args.min_attacker_points < 0 or args.min_attacker_points > args.max_attacker_points:
        raise ValueError("invalid attacker point range")
    if args.min_defender_points < 0 or args.max_score_gap < 0:
        raise ValueError("invalid balance filter")
    if args.min_depth < 0 or args.max_depth < 0:
        raise ValueError("mate depths must be non-negative")
    if args.node_limit < 0 or args.time_limit < 0:
        raise ValueError("search limits must be non-negative")
    if args.proof_dag_node_limit < 0 or args.proof_dag_edge_limit < 0:
        raise ValueError("proof DAG limits must be non-negative")
    if (
        args.min_losing_alternatives < 0
        or args.countermate_action_limit < 0
        or args.countermate_node_limit < 0
        or args.countermate_time_limit < 0
    ):
        raise ValueError("countermate limits must be non-negative")
    stats = generate_puzzles(args)
    print(json.dumps(asdict(stats), indent=2, sort_keys=True))
    return 0 if stats.saved >= args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
