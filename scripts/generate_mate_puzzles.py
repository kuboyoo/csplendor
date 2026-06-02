#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import deque
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import random
import shutil
import sys
import tempfile
import time
from typing import Callable, Iterator, Optional, Protocol, Sequence

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, game_to_spn, now_iso

from scripts.dfpn_mate_solver import (
    PLAYER0_WIN,
    PLAYER1_WIN,
    principal_line_to_kifu_text,
    solve_reveal_verified_mate,
    solve_visible_only_winner,
)
from scripts.mate_solver import MATE, SearchResult, SolverOptions

DEFAULT_GENBU_WEIGHTS = REPO_ROOT / "scripts" / "weights" / "genbu.pt"
DLSPLENDOR_ROOT = REPO_ROOT.parent / "dlsplendor"


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


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def _position_id(position: str) -> str:
    return hashlib.sha256(position.encode("utf-8")).hexdigest()[:16]


def report_rejected_position(
    progress: ProgressReporter,
    game: cs.Game,
    *,
    attempt: int,
    reason: str,
    **fields: object,
) -> None:
    position = game_to_spn(game)
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


class PuzzlePlayer(Protocol):
    def select_action(self, game: cs.Game) -> cs.Action:
        ...


class GenbuPuzzlePlayer:
    def __init__(
        self,
        weights_path: Path,
        *,
        time_limit: float,
        num_simulations: int,
        rng: random.Random,
        best_action_rate: float,
        top_action_rate: float,
        top_action_count: int,
    ):
        if str(DLSPLENDOR_ROOT) not in sys.path:
            sys.path.insert(0, str(DLSPLENDOR_ROOT))
        from dlsplendor.search.genbu_adapter import GenbuAdapter

        self.adapter = GenbuAdapter(weights_path=str(weights_path))
        self.time_limit = time_limit
        self.num_simulations = num_simulations
        self.rng = rng
        self.best_action_rate = best_action_rate
        self.top_action_rate = top_action_rate
        self.top_action_count = top_action_count

    def select_action(self, game: cs.Game) -> cs.Action:
        actions = list(game.legal_actions)
        if not actions:
            raise ValueError("cannot select an action without legal actions")
        nobles = [
            action for action in actions
            if int(action.type) == int(cs.ActionType.VISIT_NOBLE)
        ]
        if nobles:
            return nobles[0]
        if self.rng.random() >= self.best_action_rate + self.top_action_rate:
            return self.rng.choice(actions)
        if self.adapter._mcts is None:
            self.adapter.select_action_with_search(
                game,
                time_limit=self.time_limit,
                num_simulations=self.num_simulations,
            )
        action_index, info = self.adapter._mcts.search(
            game,
            time_limit=self.time_limit,
            num_simulations=self.num_simulations,
        )
        if self.rng.random() >= self.best_action_rate / (self.best_action_rate + self.top_action_rate):
            visits = info.get("visit_counts", {})
            ranked = sorted(visits, key=lambda index: visits[index], reverse=True)
            if ranked:
                action_index = self.rng.choice(ranked[:self.top_action_count])
        if not 0 <= action_index < len(actions):
            raise RuntimeError(f"Genbu returned invalid legal action index: {action_index}")
        return actions[action_index]


def create_genbu_players(
    weights_path: Path,
    *,
    time_limit: float,
    num_simulations: int,
    rng: Optional[random.Random] = None,
    best_action_rate: float = 0.7,
    top_action_rate: float = 0.2,
    top_action_count: int = 4,
) -> tuple[PuzzlePlayer, PuzzlePlayer]:
    if not weights_path.is_file():
        raise FileNotFoundError(f"Genbu weights not found: {weights_path}")
    rng = rng or random.Random()
    return (
        GenbuPuzzlePlayer(
            weights_path, time_limit=time_limit, num_simulations=num_simulations,
            rng=rng, best_action_rate=best_action_rate,
            top_action_rate=top_action_rate, top_action_count=top_action_count,
        ),
        GenbuPuzzlePlayer(
            weights_path, time_limit=time_limit, num_simulations=num_simulations,
            rng=rng, best_action_rate=best_action_rate,
            top_action_rate=top_action_rate, top_action_count=top_action_count,
        ),
    )


def generate_candidate_positions(
    rng: random.Random,
    *,
    players: Sequence[PuzzlePlayer],
    game_seed: int,
    min_playout_plies: int,
    max_playout_plies: int,
    progress: Optional[ProgressReporter] = None,
    attempt: Optional[int] = None,
    candidate_filter: Optional[Callable[[cs.Game], bool]] = None,
) -> Iterator[cs.Game]:
    if len(players) != 2:
        raise ValueError("exactly two puzzle players are required")
    game = cs.Game(seed=game_seed)
    game.simple_payment_mode = True
    start_ply = rng.randint(min_playout_plies, max_playout_plies)
    ply = 0
    while not game.is_game_over() and game.legal_actions:
        if progress is not None:
            progress.emit("genbu_playout", attempt=attempt, ply=ply, start_ply=start_ply)
        before_player = int(game.board.current_player)
        player = players[int(game.board.current_player)]
        if not game.apply(player.select_action(game), False):
            raise RuntimeError("engine rejected a generated legal action")
        ply += 1
        if ply < start_ply or int(game.board.current_player) == before_player:
            continue
        game.simple_payment_mode = False
        candidate = game.clone_light()
        if candidate_filter is None or candidate_filter(candidate):
            yield candidate
        game.simple_payment_mode = True


def generate_candidate_position(
    rng: random.Random,
    *,
    players: Sequence[PuzzlePlayer],
    game_seed: int,
    min_playout_plies: int,
    max_playout_plies: int,
    progress: Optional[ProgressReporter] = None,
    attempt: Optional[int] = None,
    candidate_filter: Optional[Callable[[cs.Game], bool]] = None,
) -> Optional[cs.Game]:
    return next(
        generate_candidate_positions(
            rng,
            players=players,
            game_seed=game_seed,
            min_playout_plies=min_playout_plies,
            max_playout_plies=max_playout_plies,
            progress=progress,
            attempt=attempt,
            candidate_filter=candidate_filter,
        ),
        None,
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
        position = game_to_spn(candidate.game)
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
    max_winning_actions: int,
    node_limit: int,
    time_limit: float,
) -> dict[str, object]:
    winning_actions: list[str] = []
    unknown_actions: list[str] = []
    checks = 0
    truncated = False
    for action in game.legal_actions:
        checks += 1
        usi = action_to_usi(action, game=game)
        raw = cs.solve_reveal_verified_mate_cpp(
            game,
            attacker=attacker,
            depth=depth,
            max_nodes=max(0, node_limit),
            time_limit_seconds=max(0.0, time_limit),
            preferred_attacker_actions=[],
            include_proof_dag=False,
            required_root_action=int(action.pack()),
        )
        if bool(raw["proven"]):
            winning_actions.append(usi)
            if len(winning_actions) > max_winning_actions:
                truncated = True
                break
        elif raw["unknown_reason"] is not None:
            unknown_actions.append(usi)
    return {
        "checks": checks,
        "winning_actions": winning_actions,
        "unknown_actions": unknown_actions,
        "complete": not unknown_actions and not truncated,
        "truncated": truncated,
    }


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


def find_visible_winning_actions(
    game: cs.Game,
    args: argparse.Namespace,
    *,
    max_winning_actions: int,
) -> list[str]:
    expected = PLAYER0_WIN if int(game.board.current_player) == 0 else PLAYER1_WIN
    winning_actions: list[str] = []
    action_limit = int(_arg(args, "visible_uniqueness_action_limit", 16))
    actions = list(game.legal_actions)
    if action_limit:
        actions = actions[:action_limit]
    for action in actions:
        for child in _completed_turn_children(game, action):
            if child.is_game_over():
                winner = int(child.board.winner)
                status = PLAYER0_WIN if winner == 0 else PLAYER1_WIN if winner == 1 else None
            else:
                status = visible_only_prefilter(child, args).status
            if status != expected:
                continue
            winning_actions.append(action_to_usi(action, game=game))
            break
        if len(winning_actions) > max_winning_actions:
            break
    return winning_actions


def find_countermate_blunders(
    game: cs.Game,
    result: SearchResult,
    *,
    min_losing_alternatives: int,
    action_limit: int,
    node_limit: int,
    time_limit: float,
    progress: Optional[ProgressReporter] = None,
    attempt: Optional[int] = None,
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
    for alternative_index, action in enumerate(alternatives, start=1):
        for child in _completed_turn_children(game, action):
            if child.is_game_over() or int(child.board.current_player) != defender:
                continue
            checks += 1
            if progress is not None:
                progress.emit(
                    "countermate_search",
                    force=True,
                    attempt=attempt,
                    alternative=f"{alternative_index}/{len(alternatives)}",
                    check=checks,
                )
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
    position = game_to_spn(game, reveal_hidden_reserved_ids=True)
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
    uniqueness = find_verified_winning_actions(
        game,
        attacker=int(game.board.current_player),
        depth=depth,
        max_winning_actions=int(_arg(args, "max_winning_actions", 1)),
        node_limit=int(_arg(args, "uniqueness_node_limit", 0)),
        time_limit=float(_arg(args, "uniqueness_time_limit", 5.0)),
    )
    stats.uniqueness_checks += int(uniqueness["checks"])
    winning_actions = list(uniqueness["winning_actions"])
    if bool(_arg(args, "require_unique_solution", True)) and (
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

    blunders, checks = find_countermate_blunders(
        game,
        result,
        min_losing_alternatives=args.min_losing_alternatives,
        action_limit=args.countermate_action_limit,
        node_limit=args.countermate_node_limit,
        time_limit=args.countermate_time_limit,
        progress=progress,
        attempt=attempt,
    )
    stats.countermate_checks += checks
    stats.countermates += len(blunders)
    if len(blunders) < args.min_losing_alternatives:
        stats.filtered += 1
        report_rejected_position(
            progress,
            game,
            attempt=attempt,
            reason="insufficient_countermate_blunders",
            found=len(blunders),
            required=args.min_losing_alternatives,
        )
        return True

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
        ),
        include_proof_dag=True,
        proof_dag_node_limit=args.proof_dag_node_limit,
        proof_dag_edge_limit=args.proof_dag_edge_limit,
    )
    if dag_result.status != MATE or dag_result.proof_tree is None:
        stats.unknown += 1
        report_rejected_position(progress, game, attempt=attempt, reason="dag_search_no_mate")
        return True
    dag = dag_result.proof_tree["verification"].get("proof_dag", {})
    if not bool(dag.get("complete")):
        stats.incomplete_dags += 1
        report_rejected_position(progress, game, attempt=attempt, reason="incomplete_dag")
        return True

    scores = [int(score) for score in game.scores]
    dag_nodes = list(dag.get("nodes", []))
    defender = 1 - int(game.board.current_player)
    puzzle_dir = save_puzzle(
        output_dir,
        game,
        dag_result,
        game_seed=game_seed,
        attempt=attempt,
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
            "uniqueness_complete": bool(uniqueness["complete"]),
            "uniqueness_checks": int(uniqueness["checks"]),
            "countermate_blunders": blunders,
            "countermate_blunder_count": len(blunders),
            "strategy_dag_nodes": len(dag_nodes),
            "max_defender_responses": max(
                (
                    len(node.get("children", []))
                    for node in dag_nodes
                    if int(node.get("player", -1)) == defender
                ),
                default=0,
            ),
        },
    )
    if puzzle_dir is None:
        stats.duplicates += 1
        report_rejected_position(progress, game, attempt=attempt, reason="duplicate")
        return True

    stats.saved += 1
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
        description="Generate reveal-verified Splendor mate puzzles and compact complete response DAGs."
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
    parser.add_argument("--proof-dag-node-limit", type=int, default=100000)
    parser.add_argument("--proof-dag-edge-limit", type=int, default=500000)
    parser.add_argument("--min-losing-alternatives", type=int, default=1)
    parser.add_argument("--countermate-action-limit", type=int, default=12, help="0 checks all wrong moves")
    parser.add_argument("--countermate-node-limit", type=int, default=0, help="0 disables the search node limit")
    parser.add_argument("--countermate-time-limit", type=float, default=5.0, help="seconds per wrong move")
    parser.add_argument("--no-require-unique-solution", dest="require_unique_solution", action="store_false")
    parser.set_defaults(require_unique_solution=True)
    parser.add_argument("--max-winning-actions", type=int, default=1)
    parser.add_argument("--uniqueness-node-limit", type=int, default=0, help="0 disables the per-action node limit")
    parser.add_argument("--uniqueness-time-limit", type=float, default=5.0, help="seconds per initial action")
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
    if (
        args.max_winning_actions <= 0
        or args.uniqueness_node_limit < 0
        or args.uniqueness_time_limit < 0
    ):
        raise ValueError("uniqueness limits must be non-negative")
    stats = generate_puzzles(args)
    print(json.dumps(asdict(stats), indent=2, sort_keys=True))
    return 0 if stats.saved >= args.count else 1


if __name__ == "__main__":
    raise SystemExit(main())
