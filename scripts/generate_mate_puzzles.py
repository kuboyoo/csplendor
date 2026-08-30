#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import random
import sys
import time
from collections import deque
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Optional, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs
from csplendor.api.usi_kifu import game_to_spn
from scripts import puzzle_validation as _puzzle_validation
from scripts.dfpn_mate_solver import (
    PLAYER0_WIN,
    PLAYER1_WIN,
    solve_reveal_verified_mate,
    solve_visible_only_winner,
    strategy_dag_max_children,
    strategy_dag_node_count,
)
from scripts.mate_solver import MATE, SearchResult, SolverOptions
from scripts.puzzle_candidates import (
    generate_candidate_position,
    generate_candidate_positions,
)
from scripts.puzzle_engine_adapter import (
    DEFAULT_GENBU_WEIGHTS,
    DLSPLENDOR_ROOT,
    GenbuPuzzlePlayer,
    PuzzlePlayer,
    create_genbu_players,
)
from scripts.puzzle_persistence import save_puzzle as _persist_puzzle

_COMPAT_COMPONENT_EXPORTS = (
    DLSPLENDOR_ROOT,
    GenbuPuzzlePlayer,
    PuzzlePlayer,
    generate_candidate_position,
    generate_candidate_positions,
)


@dataclass
class GenerationStats:
    attempts: int = 0
    sampled_positions: int = 0
    candidates: int = 0
    balance_filtered: int = 0
    threat_filtered: int = 0
    visible_prefiltered: int = 0
    uniqueness_filtered: int = 0
    uniqueness_checks: int = 0
    uniqueness_depth_checks: int = 0
    mates: int = 0
    saved: int = 0
    duplicates: int = 0
    incomplete_dags: int = 0
    unknown: int = 0
    filtered: int = 0
    countermate_checks: int = 0
    countermates: int = 0
    errors: int = 0
    boundary_hits: int = 0
    boundary_candidates: int = 0
    branch_rollouts: int = 0
    ranked_candidates: int = 0
    visible_uniqueness_filtered: int = 0
    dag_builds: int = 0
    saved_without_complete_dag: int = 0


@dataclass(frozen=True)
class ThreatSummary:
    player: int
    points: int
    optimistic_score: int
    affordable_purchases: int
    reachable_scoring_cards: int
    immediate_winning_purchases: int
    score: int


@dataclass(frozen=True)
class RankedCandidate:
    game: cs.Game
    rank_score: int
    source: str
    ply: int
    visible_depth: Optional[int] = None


class ProgressReporter:
    def __init__(self, interval: float, rejection_log: Optional[Path] = None):
        self.interval = max(0.0, float(interval))
        self._last_emit = 0.0
        self.rejection_log = rejection_log

    def emit(self, stage: str, *, force: bool = False, **fields: object) -> None:
        now = time.monotonic()
        if not force and (self.interval <= 0 or now - self._last_emit < self.interval):
            return
        self._last_emit = now
        details = " ".join(f"{key}={value}" for key, value in fields.items())
        print(f"[progress] stage={stage}" + (f" {details}" if details else ""), file=sys.stderr, flush=True)

    def record_rejection(self, payload: dict[str, object]) -> None:
        if self.rejection_log is None:
            return
        with self.rejection_log.open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")


def _generator_spn(game: cs.Game, *, reveal_hidden_reserved_ids: bool = False) -> str:
    return game_to_spn(
        game,
        reveal_hidden_reserved_ids=reveal_hidden_reserved_ids,
        require_purchased_card_ids=True,
    )


def report_rejected_position(
    progress: ProgressReporter,
    game: cs.Game,
    *,
    attempt: int,
    reason: str,
    **fields: object,
) -> None:
    position = _generator_spn(game)
    progress.record_rejection({
        "attempt": int(attempt),
        "reason": reason,
        "position": position,
        **fields,
    })
    progress.emit(
        "rejected",
        force=True,
        attempt=attempt,
        reason=reason,
        position=json.dumps(position, ensure_ascii=False),
        **fields,
    )


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


def _arg(args: argparse.Namespace, name: str, default: object) -> object:
    return getattr(args, name, default)


def _position_card_ids(game: cs.Game, player: int) -> list[int]:
    card_ids = {
        int(card_id)
        for row in game.board.visible
        for card_id in row
        if int(card_id) >= 0
    }
    card_ids.update(
        int(card_id)
        for card_id in game.board.get_player(player).reserved
        if int(card_id) >= 0
    )
    return sorted(card_ids)


def _payment_gap(player: cs.PlayerState, card: cs.Card) -> int:
    shortage = sum(
        max(0, int(card.cost[color]) - int(player.bonuses[color]) - int(player.gems[color]))
        for color in range(5)
    )
    return max(0, shortage - int(player.gems[5]))


def _noble_points_after_purchase(
    game: cs.Game,
    player: cs.PlayerState,
    card: cs.Card,
) -> int:
    bonuses = [int(value) for value in player.bonuses]
    bonuses[int(card.bonus)] += 1
    return max(
        (
            int(noble.points)
            for noble_id in game.board.nobles
            for noble in [cs.get_noble(int(noble_id))]
            if all(bonuses[color] >= int(noble.requirement[color]) for color in range(5))
        ),
        default=0,
    )


def threat_summary(game: cs.Game, player_index: int, *, turns: int) -> ThreatSummary:
    player = game.board.get_player(player_index)
    points = int(player.points)
    reachable_points: list[int] = []
    affordable_purchases = 0
    immediate_winning_purchases = 0
    reachable_scoring_cards = 0
    for card_id in _position_card_ids(game, player_index):
        card = cs.get_card(card_id)
        gap = _payment_gap(player, card)
        if gap == 0:
            affordable_purchases += 1
        if gap > max(0, turns) * 3:
            continue
        gain = int(card.points) + _noble_points_after_purchase(game, player, card)
        reachable_points.append(gain)
        if gain > 0:
            reachable_scoring_cards += 1
        if gap == 0 and points + gain >= 15:
            immediate_winning_purchases += 1

    reachable_points.sort(reverse=True)
    optimistic_score = points + sum(reachable_points[:max(0, turns)])
    score = (
        optimistic_score * 10
        + affordable_purchases * 3
        + reachable_scoring_cards * 2
        + immediate_winning_purchases * 25
    )
    return ThreatSummary(
        player=player_index,
        points=points,
        optimistic_score=optimistic_score,
        affordable_purchases=affordable_purchases,
        reachable_scoring_cards=reachable_scoring_cards,
        immediate_winning_purchases=immediate_winning_purchases,
        score=score,
    )


def _threat_summaries(game: cs.Game, args: argparse.Namespace) -> tuple[ThreatSummary, ThreatSummary]:
    turns = int(_arg(args, "threat_turns", 3))
    return (
        threat_summary(game, 0, turns=turns),
        threat_summary(game, 1, turns=turns),
    )


def is_tactical_candidate(
    game: cs.Game,
    args: argparse.Namespace,
    *,
    summaries: Optional[tuple[ThreatSummary, ThreatSummary]] = None,
) -> bool:
    if not is_suspicious_position(
        game,
        min_attacker_points=int(_arg(args, "min_attacker_points", 8)),
        max_attacker_points=int(_arg(args, "max_attacker_points", 14)),
        min_defender_points=int(_arg(args, "min_defender_points", 8)),
        max_score_gap=int(_arg(args, "max_score_gap", 3)),
        allow_final_round=bool(_arg(args, "allow_final_round", False)),
    ):
        return False
    if len(game.legal_actions) < int(_arg(args, "min_legal_actions", 12)):
        return False

    summaries = summaries or _threat_summaries(game, args)
    attacker = int(game.board.current_player)
    defender = 1 - attacker
    min_optimistic_score = int(_arg(args, "min_optimistic_score", 15))
    if summaries[attacker].optimistic_score < min_optimistic_score:
        return False
    if bool(_arg(args, "require_both_threats", True)):
        if summaries[defender].optimistic_score < min_optimistic_score:
            return False
    min_threat_score = int(_arg(args, "min_threat_score", 0))
    return summaries[attacker].score >= min_threat_score


def candidate_rank_score(
    game: cs.Game,
    args: argparse.Namespace,
    *,
    summaries: Optional[tuple[ThreatSummary, ThreatSummary]] = None,
) -> int:
    summaries = summaries or _threat_summaries(game, args)
    attacker = int(game.board.current_player)
    defender = 1 - attacker
    optimistic_gap = abs(
        summaries[attacker].optimistic_score - summaries[defender].optimistic_score
    )
    immediate_wins = sum(summary.immediate_winning_purchases for summary in summaries)
    return (
        len(game.legal_actions) * 4
        + min(summaries[attacker].score, summaries[defender].score)
        - optimistic_gap * 8
        - immediate_wins * 40
    )


def visible_only_prefilter(
    game: cs.Game,
    args: argparse.Namespace,
) -> SearchResult:
    return solve_visible_only_winner(
        game,
        options=SolverOptions(
            max_nodes=int(_arg(args, "visible_prefilter_node_limit", 50000)),
            time_limit=float(_arg(args, "visible_prefilter_time_limit", 0.5)),
            include_proof=True,
            allow_deck_reserve=True,
        ),
    )


def _visible_depth(game: cs.Game, args: argparse.Namespace) -> Optional[int]:
    visible = visible_only_prefilter(game, args)
    expected = PLAYER0_WIN if int(game.board.current_player) == 0 else PLAYER1_WIN
    if visible.status != expected or visible.proof_tree is None:
        return None
    depth = visible.proof_tree.get("forced_win_depth")
    return int(depth) if depth is not None else None


def _candidate_snapshot(game: cs.Game, args: argparse.Namespace, *, source: str, ply: int) -> Optional[RankedCandidate]:
    candidate = game.clone_light()
    candidate.simple_payment_mode = False
    if not is_tactical_candidate(candidate, args):
        return None
    return RankedCandidate(
        game=candidate,
        rank_score=candidate_rank_score(candidate, args),
        source=source,
        ply=ply,
    )


def _rank_visible_candidate(candidate: RankedCandidate, args: argparse.Namespace) -> RankedCandidate:
    visible_depth = _visible_depth(candidate.game, args)
    score = candidate.rank_score
    if visible_depth is not None:
        preferred_min = int(_arg(args, "preferred_min_depth", 4))
        preferred_max = int(_arg(args, "preferred_max_depth", 7))
        if preferred_min <= visible_depth <= preferred_max:
            score += 250
        elif visible_depth < preferred_min:
            score -= (preferred_min - visible_depth) * 120
        else:
            score += 80
    return RankedCandidate(
        game=candidate.game,
        rank_score=score,
        source=candidate.source,
        ply=candidate.ply,
        visible_depth=visible_depth,
    )


def _branch_rollout_candidates(
    rng: random.Random,
    *,
    players: Sequence[PuzzlePlayer],
    root: cs.Game,
    args: argparse.Namespace,
    start_ply: int,
) -> list[RankedCandidate]:
    candidates: list[RankedCandidate] = []
    for branch in range(int(_arg(args, "branch_rollouts", 0))):
        game = root.clone_light()
        game.simple_payment_mode = True
        plies = int(_arg(args, "branch_rollout_plies", 4))
        for offset in range(plies):
            if game.is_game_over() or not game.legal_actions:
                break
            before_player = int(game.board.current_player)
            actions = list(game.legal_actions)
            if offset == 0 and len(actions) > 1:
                action = rng.choice(actions[1:])
            else:
                action = players[before_player].select_action(game)
            if not game.apply(action, False):
                raise RuntimeError("engine rejected a generated branch action")
            if int(game.board.current_player) == before_player:
                continue
            candidate = _candidate_snapshot(
                game,
                args,
                source=f"branch:{branch}",
                ply=start_ply + offset + 1,
            )
            if candidate is not None:
                candidates.append(_rank_visible_candidate(candidate, args))
    return candidates


def generate_ranked_candidate_positions(
    rng: random.Random,
    *,
    players: Sequence[PuzzlePlayer],
    game_seed: int,
    args: argparse.Namespace,
    stats: GenerationStats,
    progress: Optional[ProgressReporter] = None,
    attempt: Optional[int] = None,
) -> list[RankedCandidate]:
    game = cs.Game(seed=game_seed)
    game.simple_payment_mode = True
    start_ply = rng.randint(args.min_playout_plies, args.max_playout_plies)
    history: deque[tuple[int, cs.Game]] = deque(maxlen=int(_arg(args, "boundary_history", 10)))
    candidates: list[RankedCandidate] = []
    previous_visible_depth: Optional[int] = None
    ply = 0
    while not game.is_game_over() and game.legal_actions:
        if progress is not None:
            progress.emit("genbu_playout", attempt=attempt, ply=ply, start_ply=start_ply)
        before_player = int(game.board.current_player)
        if not game.apply(players[before_player].select_action(game), False):
            raise RuntimeError("engine rejected a generated legal action")
        ply += 1
        if int(game.board.current_player) == before_player:
            continue
        history.append((ply, game.clone_light()))
        if ply < start_ply:
            continue
        candidate = _candidate_snapshot(game, args, source="playout", ply=ply)
        if candidate is None:
            continue
        candidate = _rank_visible_candidate(candidate, args)
        candidates.append(candidate)
        visible_depth = candidate.visible_depth
        trigger_depth = int(_arg(args, "boundary_trigger_depth", 3))
        if visible_depth is not None and visible_depth <= trigger_depth and (
            previous_visible_depth is None or previous_visible_depth > trigger_depth
        ):
            stats.boundary_hits += 1
            if progress is not None:
                progress.emit("boundary_hit", force=True, attempt=attempt, ply=ply, depth=visible_depth)
            for historic_ply, historic_game in history:
                historic = _candidate_snapshot(
                    historic_game,
                    args,
                    source="boundary_history",
                    ply=historic_ply,
                )
                if historic is not None:
                    candidates.append(_rank_visible_candidate(historic, args))
                    stats.boundary_candidates += 1
            branch_roots = list(history)[:-1][-int(_arg(args, "branch_root_count", 3)):]
            for historic_ply, historic_game in branch_roots:
                branched = _branch_rollout_candidates(
                    rng,
                    players=players,
                    root=historic_game,
                    args=args,
                    start_ply=historic_ply,
                )
                candidates.extend(branched)
                stats.branch_rollouts += int(_arg(args, "branch_rollouts", 0))
            break
        previous_visible_depth = visible_depth

    unique: dict[str, RankedCandidate] = {}
    for candidate in candidates:
        position = _generator_spn(candidate.game)
        known = unique.get(position)
        if known is None or candidate.rank_score > known.rank_score:
            unique[position] = candidate
    ranked = sorted(unique.values(), key=lambda candidate: candidate.rank_score, reverse=True)
    limit = int(_arg(args, "ranked_candidates_per_attempt", 8))
    selected = ranked[:limit] if limit else ranked
    stats.ranked_candidates += len(selected)
    return selected


def find_verified_winning_actions(
    game: cs.Game,
    *,
    attacker: int,
    depth: int,
    max_depth: Optional[int] = None,
    max_winning_actions: int,
    node_limit: int,
    time_limit: float,
    jobs: int = 1,
    positive_time_limit: float = 0.0,
) -> dict[str, object]:
    return _puzzle_validation.find_verified_winning_actions(
        game,
        attacker=attacker,
        depth=depth,
        max_depth=max_depth,
        max_winning_actions=max_winning_actions,
        node_limit=node_limit,
        time_limit=time_limit,
        jobs=jobs,
        positive_time_limit=positive_time_limit,
    )


def _completed_turn_children(game: cs.Game, action: cs.Action) -> list[cs.Game]:
    return _puzzle_validation.completed_turn_children(game, action)


def find_visible_winning_actions(
    game: cs.Game,
    args: argparse.Namespace,
    *,
    max_winning_actions: int,
) -> list[str]:
    return _puzzle_validation.find_visible_winning_actions(
        game,
        args,
        max_winning_actions=max_winning_actions,
        arg_value=_arg,
        visible_only_prefilter=visible_only_prefilter,
        player0_win=PLAYER0_WIN,
        player1_win=PLAYER1_WIN,
    )


def find_countermate_blunders(
    game: cs.Game,
    result: SearchResult,
    *,
    min_losing_alternatives: int,
    action_limit: int,
    node_limit: int,
    time_limit: float,
    jobs: int = 1,
    progress: Optional[ProgressReporter] = None,
    attempt: Optional[int] = None,
) -> tuple[list[dict[str, object]], int]:
    return _puzzle_validation.find_countermate_blunders(
        game,
        result,
        min_losing_alternatives=min_losing_alternatives,
        action_limit=action_limit,
        node_limit=node_limit,
        time_limit=time_limit,
        jobs=jobs,
        solve_reveal_verified_mate=solve_reveal_verified_mate,
        progress=progress,
        attempt=attempt,
    )


def save_puzzle(
    output_dir: Path,
    game: cs.Game,
    result: SearchResult,
    *,
    game_seed: int,
    attempt: int,
    quality: Optional[dict[str, object]] = None,
    strategy_dag_format: str = "compact",
    strategy_dag_requested: Optional[bool] = None,
    strategy_dag_omitted_reason: Optional[str] = None,
    require_complete_dag: bool = False,
) -> Optional[Path]:
    return _persist_puzzle(
        output_dir,
        game,
        result,
        game_seed=game_seed,
        attempt=attempt,
        quality=quality,
        strategy_dag_format=strategy_dag_format,
        strategy_dag_requested=strategy_dag_requested,
        strategy_dag_omitted_reason=strategy_dag_omitted_reason,
        require_complete_dag=require_complete_dag,
    )


def try_save_candidate(
    args: argparse.Namespace,
    *,
    output_dir: Path,
    stats: GenerationStats,
    progress: ProgressReporter,
    game: cs.Game,
    game_seed: int,
    attempt: int,
    sample_source: str = "direct",
    sample_ply: Optional[int] = None,
    sample_rank_score: Optional[int] = None,
    sample_visible_depth: Optional[int] = None,
) -> bool:
    stats.sampled_positions += 1
    if not is_suspicious_position(
        game,
        min_attacker_points=int(_arg(args, "min_attacker_points", 8)),
        max_attacker_points=int(_arg(args, "max_attacker_points", 14)),
        min_defender_points=int(_arg(args, "min_defender_points", 8)),
        max_score_gap=int(_arg(args, "max_score_gap", 3)),
        allow_final_round=bool(_arg(args, "allow_final_round", False)),
    ):
        stats.filtered += 1
        stats.balance_filtered += 1
        report_rejected_position(progress, game, attempt=attempt, reason="balance_filter")
        return False

    summaries = _threat_summaries(game, args)
    if not is_tactical_candidate(game, args, summaries=summaries):
        stats.filtered += 1
        stats.threat_filtered += 1
        report_rejected_position(
            progress,
            game,
            attempt=attempt,
            reason="threat_filter",
            legal_actions=len(game.legal_actions),
            optimistic_scores=[summary.optimistic_score for summary in summaries],
            threat_scores=[summary.score for summary in summaries],
        )
        return False

    stats.candidates += 1
    expected_visible_status = PLAYER0_WIN if int(game.board.current_player) == 0 else PLAYER1_WIN
    if bool(_arg(args, "visible_prefilter", True)):
        progress.emit("visible_prefilter", force=True, attempt=attempt, candidates=stats.candidates)
        visible = visible_only_prefilter(game, args)
        visible_tree = visible.proof_tree or {}
        visible_depth = visible_tree.get("forced_win_depth")
        too_short = (
            visible.status == expected_visible_status
            and visible_depth is not None
            and int(visible_depth) < int(_arg(args, "min_depth", 1))
        )
        if (
            visible.status != expected_visible_status
            or visible_depth is None
            or too_short
            or (
                int(_arg(args, "max_depth", 0))
                and int(visible_depth) > int(_arg(args, "max_depth", 0))
            )
        ):
            stats.filtered += 1
            stats.visible_prefiltered += 1
            report_rejected_position(
                progress,
                game,
                attempt=attempt,
                reason="visible_prefilter",
                status=visible.status,
                depth=visible_depth,
            )
            return too_short

    if bool(_arg(args, "visible_uniqueness_prefilter", True)):
        visible_winning_actions = find_visible_winning_actions(
            game,
            args,
            max_winning_actions=int(_arg(args, "max_winning_actions", 1)),
        )
        if len(visible_winning_actions) > int(_arg(args, "max_winning_actions", 1)):
            stats.filtered += 1
            stats.visible_uniqueness_filtered += 1
            report_rejected_position(
                progress,
                game,
                attempt=attempt,
                reason="visible_uniqueness_filter",
                winning_actions=len(visible_winning_actions),
            )
            return False

    progress.emit("mate_search", force=True, attempt=attempt, candidates=stats.candidates)
    result = solve_reveal_verified_mate(
        game,
        attacker=int(game.board.current_player),
        options=SolverOptions(
            max_nodes=args.node_limit,
            time_limit=args.time_limit,
            include_proof=True,
            allow_deck_reserve=True,
            jobs=int(_arg(args, "mate_jobs", 1)),
        ),
        include_proof_dag=False,
    )
    if result.status != MATE or result.proof_tree is None:
        stats.unknown += 1
        report_rejected_position(
            progress,
            game,
            attempt=attempt,
            reason="no_mate",
            status=result.status,
        )
        return False

    stats.mates += 1
    depth = int(result.proof_tree["forced_win_depth"])
    if depth < args.min_depth or (args.max_depth and depth > args.max_depth):
        stats.filtered += 1
        report_rejected_position(progress, game, attempt=attempt, reason="depth_filter", depth=depth)
        return True
    require_unique_solution = bool(_arg(args, "require_unique_solution", True))
    if require_unique_solution:
        uniqueness = find_verified_winning_actions(
            game,
            attacker=int(game.board.current_player),
            depth=depth,
            max_depth=(
                int(_arg(args, "uniqueness_max_depth", 0)) or depth
            ),
            max_winning_actions=int(_arg(args, "max_winning_actions", 1)),
            node_limit=int(_arg(args, "uniqueness_node_limit", 0)),
            time_limit=float(_arg(args, "uniqueness_time_limit", 5.0)),
            jobs=int(_arg(args, "uniqueness_jobs", 1)),
            positive_time_limit=float(
                _arg(args, "uniqueness_positive_time_limit", 2.0)
            ),
        )
    else:
        uniqueness = {
            "checks": 0,
            "winning_actions": [],
            "unknown_actions": [],
            "complete": False,
            "truncated": False,
        }
    stats.uniqueness_checks += int(uniqueness["checks"])
    stats.uniqueness_depth_checks += int(uniqueness.get("depth_checks", 0))
    winning_actions = list(uniqueness["winning_actions"])
    if require_unique_solution and (
        not bool(uniqueness["complete"])
        or len(winning_actions) != int(_arg(args, "max_winning_actions", 1))
    ):
        stats.filtered += 1
        stats.uniqueness_filtered += 1
        report_rejected_position(
            progress,
            game,
            attempt=attempt,
            reason="uniqueness_filter",
            complete=uniqueness["complete"],
            winning_actions=len(winning_actions),
            unknown_actions=len(uniqueness["unknown_actions"]),
        )
        return True

    min_losing_alternatives = int(_arg(args, "min_losing_alternatives", 0))
    if min_losing_alternatives > 0:
        blunders, checks = find_countermate_blunders(
            game,
            result,
            min_losing_alternatives=min_losing_alternatives,
            action_limit=int(_arg(args, "countermate_action_limit", 12)),
            node_limit=int(_arg(args, "countermate_node_limit", 0)),
            time_limit=float(_arg(args, "countermate_time_limit", 5.0)),
            jobs=int(_arg(args, "mate_jobs", 1)),
            progress=progress,
            attempt=attempt,
        )
    else:
        blunders, checks = [], 0
    stats.countermate_checks += checks
    stats.countermates += len(blunders)
    if len(blunders) < min_losing_alternatives:
        stats.filtered += 1
        report_rejected_position(
            progress,
            game,
            attempt=attempt,
            reason="insufficient_countermate_blunders",
            found=len(blunders),
            required=min_losing_alternatives,
        )
        return True

    build_strategy_dag = bool(_arg(args, "build_strategy_dag", True))
    require_complete_dag = bool(_arg(args, "require_complete_dag", False))
    result_to_save = result
    dag: dict[str, object] = {}
    dag_complete = False
    dag_validated = False
    dag_omitted_reason: Optional[str] = "not_requested"
    if build_strategy_dag:
        progress.emit("proof_dag", force=True, attempt=attempt, depth=depth)
        stats.dag_builds += 1
        dag_result = solve_reveal_verified_mate(
            game,
            attacker=int(game.board.current_player),
            options=SolverOptions(
                max_nodes=args.node_limit,
                time_limit=args.time_limit,
                include_proof=True,
                allow_deck_reserve=True,
                jobs=int(_arg(args, "mate_jobs", 1)),
            ),
            include_proof_dag=True,
            proof_dag_node_limit=int(_arg(args, "proof_dag_node_limit", 100000)),
            proof_dag_edge_limit=int(_arg(args, "proof_dag_edge_limit", 500000)),
            proof_dag_format=str(_arg(args, "strategy_dag_format", "compact")),
        )
        dag_omitted_reason = "dag_search_no_mate"
        if dag_result.status == MATE and dag_result.proof_tree is not None:
            result_to_save = dag_result
            dag_verification = dag_result.proof_tree.get("verification")
            raw_dag = (
                dag_verification.get("proof_dag")
                if isinstance(dag_verification, dict)
                else None
            )
            if isinstance(raw_dag, dict):
                dag = raw_dag
                dag_complete = bool(dag.get("complete"))
                dag_validated = bool(dag.get("validated"))
                raw_reason = dag.get("omitted_reason")
                dag_omitted_reason = None if dag_complete else str(
                    raw_reason or "incomplete"
                )
            else:
                dag_omitted_reason = "proof_dag_not_returned"

        if not dag_complete:
            stats.incomplete_dags += 1
            if require_complete_dag:
                report_rejected_position(
                    progress,
                    game,
                    attempt=attempt,
                    reason="incomplete_dag",
                    omitted_reason=dag_omitted_reason,
                )
                return True
            progress.emit(
                "proof_dag_omitted",
                force=True,
                attempt=attempt,
                reason=dag_omitted_reason,
            )

    scores = [int(score) for score in game.scores]
    defender = 1 - int(game.board.current_player)
    puzzle_dir = save_puzzle(
        output_dir,
        game,
        result_to_save,
        game_seed=game_seed,
        attempt=attempt,
        strategy_dag_format=str(_arg(args, "strategy_dag_format", "compact")),
        strategy_dag_requested=build_strategy_dag,
        strategy_dag_omitted_reason=dag_omitted_reason,
        require_complete_dag=require_complete_dag,
        quality={
            "sample_source": sample_source,
            "sample_ply": sample_ply,
            "sample_rank_score": sample_rank_score,
            "sample_visible_depth": sample_visible_depth,
            "score_gap": abs(scores[0] - scores[1]),
            "legal_actions": len(game.legal_actions),
            "optimistic_scores": [summary.optimistic_score for summary in summaries],
            "threat_scores": [summary.score for summary in summaries],
            "verified_winning_actions": winning_actions,
            "verified_winning_action_count": len(winning_actions),
            "verified_winning_action_depths": list(
                uniqueness.get("winning_action_depths", [])
            ),
            "uniqueness_required": require_unique_solution,
            "uniqueness_complete": bool(uniqueness["complete"]),
            "uniqueness_checks": int(uniqueness["checks"]),
            "uniqueness_depth_checks": int(uniqueness.get("depth_checks", 0)),
            "uniqueness_jobs": int(uniqueness.get("jobs", 1)),
            "mate_jobs": int(_arg(args, "mate_jobs", 1)),
            "uniqueness_positive_time_limit": float(
                uniqueness.get("positive_time_limit", 0.0)
            ),
            "uniqueness_min_depth": int(uniqueness.get("min_depth", depth)),
            "uniqueness_max_depth": int(uniqueness.get("max_depth", depth)),
            "uniqueness_action_results": list(
                uniqueness.get("action_results", [])
            ),
            "countermate_blunders": blunders,
            "countermate_blunder_count": len(blunders),
            "countermate_required": min_losing_alternatives,
            "strategy_dag_requested": build_strategy_dag,
            "strategy_dag_complete": dag_complete,
            "strategy_dag_validated": dag_validated,
            "strategy_dag_omitted_reason": dag_omitted_reason,
            "strategy_dag_nodes": strategy_dag_node_count(dag) if dag_complete else 0,
            "max_defender_responses": (
                strategy_dag_max_children(dag, player=defender)
                if dag_complete
                else 0
            ),
        },
    )
    if puzzle_dir is None:
        stats.duplicates += 1
        report_rejected_position(progress, game, attempt=attempt, reason="duplicate")
        return True

    stats.saved += 1
    if not dag_complete:
        stats.saved_without_complete_dag += 1
    print(f"[saved {stats.saved}/{args.count}] {puzzle_dir}", flush=True)
    return True


def generate_puzzles(args: argparse.Namespace) -> GenerationStats:
    rng = random.Random(args.seed)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stats = GenerationStats()
    progress = ProgressReporter(args.progress_seconds, output_dir / "rejections.jsonl")
    progress.emit(
        "startup",
        force=True,
        count=args.count,
        max_attempts=args.max_attempts,
        genbu_simulations=args.genbu_simulations,
        genbu_time_limit=args.genbu_time_limit,
        build_strategy_dag=args.build_strategy_dag,
        require_complete_dag=args.require_complete_dag,
        min_losing_alternatives=args.min_losing_alternatives,
        mate_jobs=args.mate_jobs,
    )
    players = create_genbu_players(
        Path(args.genbu_weights),
        time_limit=args.genbu_time_limit,
        num_simulations=args.genbu_simulations,
        rng=rng,
        best_action_rate=args.genbu_best_action_rate,
        top_action_rate=args.genbu_top_action_rate,
        top_action_count=args.genbu_top_action_count,
    )

    while stats.saved < args.count and stats.attempts < args.max_attempts:
        stats.attempts += 1
        game_seed = rng.getrandbits(32)
        progress.emit("attempt_start", force=True, attempt=stats.attempts, saved=stats.saved, game_seed=game_seed)
        game: Optional[cs.Game] = None
        try:
            positions = generate_ranked_candidate_positions(
                rng,
                players=players,
                game_seed=game_seed,
                args=args,
                stats=stats,
                progress=progress,
                attempt=stats.attempts,
            )
            for ranked in positions:
                game = ranked.game
                progress.emit(
                    "ranked_candidate",
                    force=True,
                    attempt=stats.attempts,
                    source=ranked.source,
                    ply=ranked.ply,
                    rank=ranked.rank_score,
                    visible_depth=ranked.visible_depth,
                )
                try_save_candidate(
                    args,
                    output_dir=output_dir,
                    stats=stats,
                    progress=progress,
                    game=game,
                    game_seed=game_seed,
                    attempt=stats.attempts,
                    sample_source=ranked.source,
                    sample_ply=ranked.ply,
                    sample_rank_score=ranked.rank_score,
                    sample_visible_depth=ranked.visible_depth,
                )
                if stats.saved >= args.count:
                    break
        except Exception as exc:
            stats.errors += 1
            if game is not None:
                report_rejected_position(
                    progress,
                    game,
                    attempt=stats.attempts,
                    reason="error",
                    error=type(exc).__name__,
                )
            print(f"[attempt {stats.attempts}] {type(exc).__name__}: {exc}", file=sys.stderr)
        finally:
            if args.progress_interval and stats.attempts % args.progress_interval == 0:
                print(f"[progress] {json.dumps(asdict(stats), sort_keys=True)}", file=sys.stderr)
    return stats


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Generate reveal-verified Splendor mate puzzles and optional response DAGs."
    )
    parser.add_argument("--output-dir", default="generated/mate_puzzles")
    parser.add_argument("--count", type=int, default=100, help="number of puzzles to save")
    parser.add_argument("--max-attempts", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=0, help="generator RNG seed")
    parser.add_argument("--genbu-weights", default=str(DEFAULT_GENBU_WEIGHTS))
    parser.add_argument("--genbu-time-limit", type=float, default=0.25, help="seconds per Genbu move")
    parser.add_argument("--genbu-simulations", type=int, default=100, help="maximum MCTS simulations per Genbu move")
    parser.add_argument("--genbu-best-action-rate", type=float, default=0.7)
    parser.add_argument("--genbu-top-action-rate", type=float, default=0.2)
    parser.add_argument("--genbu-top-action-count", type=int, default=4)
    parser.add_argument("--min-playout-plies", type=int, default=18)
    parser.add_argument("--max-playout-plies", type=int, default=48)
    parser.add_argument("--boundary-history", type=int, default=10)
    parser.add_argument("--boundary-trigger-depth", type=int, default=3)
    parser.add_argument("--preferred-min-depth", type=int, default=4)
    parser.add_argument("--preferred-max-depth", type=int, default=7)
    parser.add_argument("--branch-root-count", type=int, default=3)
    parser.add_argument("--branch-rollouts", type=int, default=2)
    parser.add_argument("--branch-rollout-plies", type=int, default=4)
    parser.add_argument("--ranked-candidates-per-attempt", type=int, default=8)
    parser.add_argument("--min-attacker-points", type=int, default=4)
    parser.add_argument("--max-attacker-points", type=int, default=14)
    parser.add_argument("--min-defender-points", type=int, default=5)
    parser.add_argument("--max-score-gap", type=int, default=5)
    parser.add_argument("--allow-final-round", action="store_true")
    parser.add_argument("--min-legal-actions", type=int, default=12)
    parser.add_argument("--threat-turns", type=int, default=5)
    parser.add_argument("--min-optimistic-score", type=int, default=15)
    parser.add_argument("--min-threat-score", type=int, default=0)
    parser.add_argument("--require-both-threats", action="store_true")
    parser.add_argument("--no-require-both-threats", dest="require_both_threats", action="store_false")
    parser.set_defaults(require_both_threats=False)
    parser.add_argument("--min-depth", type=int, default=3)
    parser.add_argument("--max-depth", type=int, default=0, help="0 disables the upper bound")
    parser.add_argument("--no-visible-prefilter", dest="visible_prefilter", action="store_false")
    parser.set_defaults(visible_prefilter=True)
    parser.add_argument("--visible-prefilter-node-limit", type=int, default=50000)
    parser.add_argument("--visible-prefilter-time-limit", type=float, default=0.5)
    parser.add_argument("--no-visible-uniqueness-prefilter", dest="visible_uniqueness_prefilter", action="store_false")
    parser.set_defaults(visible_uniqueness_prefilter=True)
    parser.add_argument("--visible-uniqueness-action-limit", type=int, default=16)
    parser.add_argument("--node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--time-limit", type=float, default=30.0, help="seconds per candidate")
    parser.add_argument(
        "--mate-jobs",
        type=int,
        default=1,
        help="CPU workers for candidate and countermate proof search; 0 uses CPU count",
    )
    parser.add_argument("--proof-dag-node-limit", type=int, default=100000)
    parser.add_argument("--proof-dag-edge-limit", type=int, default=500000)
    parser.add_argument(
        "--no-strategy-dag",
        dest="build_strategy_dag",
        action="store_false",
        help="skip the expensive response-DAG build and save the verified principal line only",
    )
    parser.set_defaults(build_strategy_dag=True)
    parser.add_argument(
        "--require-complete-dag",
        action="store_true",
        help="reject a proven mate unless a complete validated response DAG is also built",
    )
    parser.add_argument(
        "--strategy-dag-format",
        choices=("compact", "v1", "both"),
        default="compact",
        help="strategy DAG encoding saved in strategy.json",
    )
    parser.add_argument(
        "--min-losing-alternatives",
        type=int,
        default=0,
        help="required wrong moves allowing an opponent mate; 0 disables this optional quality filter",
    )
    parser.add_argument("--countermate-action-limit", type=int, default=12, help="0 checks all wrong moves")
    parser.add_argument("--countermate-node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--countermate-time-limit", type=float, default=5.0, help="seconds per wrong move")
    parser.add_argument("--no-require-unique-solution", dest="require_unique_solution", action="store_false")
    parser.set_defaults(require_unique_solution=True)
    parser.add_argument("--max-winning-actions", type=int, default=1)
    parser.add_argument(
        "--uniqueness-max-depth",
        type=int,
        default=0,
        help=(
            "verify every initial action from the puzzle depth through this depth; "
            "0 checks only the puzzle depth"
        ),
    )
    parser.add_argument(
        "--uniqueness-node-limit",
        type=int,
        default=0,
        help="cumulative node limit per initial action; 0 disables the limit",
    )
    parser.add_argument(
        "--uniqueness-time-limit",
        type=float,
        default=5.0,
        help="cumulative seconds per initial action across all uniqueness depths",
    )
    parser.add_argument(
        "--uniqueness-jobs",
        type=int,
        default=1,
        help="initial-action verification workers; 0 uses CPU count",
    )
    parser.add_argument(
        "--uniqueness-positive-time-limit",
        type=float,
        default=2.0,
        help=(
            "seconds per initial action for a fast sound mate-proof pass "
            "before exhaustive uniqueness refutation; 0 disables the pass"
        ),
    )
    parser.add_argument("--progress-interval", type=int, default=100)
    parser.add_argument("--progress-seconds", type=float, default=10.0, help="seconds between in-attempt progress messages; 0 disables periodic messages")
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.count < 0 or args.max_attempts < 0:
        raise ValueError("count and max-attempts must be non-negative")
    if args.min_playout_plies < 0 or args.min_playout_plies > args.max_playout_plies:
        raise ValueError("invalid playout ply range")
    if (
        args.genbu_time_limit < 0
        or args.genbu_simulations <= 0
        or args.genbu_best_action_rate < 0
        or args.genbu_top_action_rate < 0
        or args.genbu_best_action_rate + args.genbu_top_action_rate > 1
        or args.genbu_best_action_rate + args.genbu_top_action_rate <= 0
        or args.genbu_top_action_count <= 0
    ):
        raise ValueError("invalid Genbu search limits")
    if (
        args.boundary_history <= 0
        or args.boundary_trigger_depth < 0
        or args.preferred_min_depth < 0
        or args.preferred_min_depth > args.preferred_max_depth
        or args.branch_root_count < 0
        or args.branch_rollouts < 0
        or args.branch_rollout_plies < 0
        or args.ranked_candidates_per_attempt < 0
    ):
        raise ValueError("invalid boundary sampling limits")
    if args.progress_seconds < 0:
        raise ValueError("progress-seconds must be non-negative")
    if args.min_attacker_points < 0 or args.min_attacker_points > args.max_attacker_points:
        raise ValueError("invalid attacker point range")
    if args.min_defender_points < 0 or args.max_score_gap < 0:
        raise ValueError("invalid balance filter")
    if (
        args.min_legal_actions < 0
        or args.threat_turns < 0
        or args.min_optimistic_score < 0
        or args.min_threat_score < 0
    ):
        raise ValueError("invalid threat filter")
    if args.min_depth < 0 or args.max_depth < 0:
        raise ValueError("mate depths must be non-negative")
    if (
        args.visible_prefilter_node_limit < 0
        or args.visible_prefilter_time_limit < 0
        or args.visible_uniqueness_action_limit < 0
    ):
        raise ValueError("visible prefilter limits must be non-negative")
    if args.node_limit < 0 or args.time_limit < 0 or args.mate_jobs < 0:
        raise ValueError("search limits must be non-negative")
    if args.proof_dag_node_limit < 0 or args.proof_dag_edge_limit < 0:
        raise ValueError("proof DAG limits must be non-negative")
    if args.require_complete_dag and not args.build_strategy_dag:
        raise ValueError("--require-complete-dag cannot be used with --no-strategy-dag")
    if (
        args.min_losing_alternatives < 0
        or args.countermate_action_limit < 0
        or args.countermate_node_limit < 0
        or args.countermate_time_limit < 0
    ):
        raise ValueError("countermate limits must be non-negative")
    if (
        args.max_winning_actions <= 0
        or args.uniqueness_max_depth < 0
        or args.uniqueness_node_limit < 0
        or args.uniqueness_time_limit < 0
        or args.uniqueness_jobs < 0
        or args.uniqueness_positive_time_limit < 0
    ):
        raise ValueError("uniqueness limits must be non-negative")
    stats = generate_puzzles(args)
    print(json.dumps(asdict(stats), indent=2, sort_keys=True))
    return 0 if stats.saved >= args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
