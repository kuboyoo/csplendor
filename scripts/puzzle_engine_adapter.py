"""External-engine adapters used only by mate-puzzle generation."""

from __future__ import annotations

import random
import sys
from pathlib import Path
from typing import Optional, Protocol

import csplendor as cs

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GENBU_WEIGHTS = REPO_ROOT / "scripts" / "weights" / "genbu.pt"
DLSPLENDOR_ROOT = REPO_ROOT.parent / "dlsplendor"


class PuzzlePlayer(Protocol):
    def select_action(self, game: cs.Game) -> cs.Action: ...


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
            action
            for action in actions
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
        if self.rng.random() >= self.best_action_rate / (
            self.best_action_rate + self.top_action_rate
        ):
            visits = info.get("visit_counts", {})
            ranked = sorted(
                visits,
                key=lambda index: visits[index],
                reverse=True,
            )
            if ranked:
                action_index = self.rng.choice(ranked[: self.top_action_count])
        if not 0 <= action_index < len(actions):
            raise RuntimeError(
                f"Genbu returned invalid legal action index: {action_index}"
            )
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
            weights_path,
            time_limit=time_limit,
            num_simulations=num_simulations,
            rng=rng,
            best_action_rate=best_action_rate,
            top_action_rate=top_action_rate,
            top_action_count=top_action_count,
        ),
        GenbuPuzzlePlayer(
            weights_path,
            time_limit=time_limit,
            num_simulations=num_simulations,
            rng=rng,
            best_action_rate=best_action_rate,
            top_action_rate=top_action_rate,
            top_action_count=top_action_count,
        ),
    )


__all__ = [
    "DEFAULT_GENBU_WEIGHTS",
    "DLSPLENDOR_ROOT",
    "GenbuPuzzlePlayer",
    "PuzzlePlayer",
    "create_genbu_players",
]
