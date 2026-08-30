"""External-engine adapters used only by mate-puzzle generation."""

from __future__ import annotations

import os
import random
import sys
import tempfile
from pathlib import Path
from typing import Optional, Protocol, Sequence

import csplendor as cs
import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_GENBU_WEIGHTS = REPO_ROOT / "scripts" / "weights" / "genbu.pt"
DLSPLENDOR_ROOT = REPO_ROOT.parent / "dlsplendor"


def _resolve_alphazero_path() -> Path:
    """Find the legacy engine required to deserialize and run genbu.pt."""
    candidates = [
        os.environ.get("ALPHAZERO_ORI_PATH"),
        DLSPLENDOR_ROOT / "alphazero-general-ori",
        DLSPLENDOR_ROOT.parent / "alphazero-general-ori",
        REPO_ROOT.parents[1] / "src" / "alphazero-general-ori",
    ]
    checked: list[Path] = []
    for candidate in candidates:
        if not candidate:
            continue
        path = Path(candidate).expanduser().resolve()
        if path in checked:
            continue
        checked.append(path)
        if (
            (path / "splendor" / "SplendorGame.py").is_file()
            and (path / "splendor" / "SplendorNNet.py").is_file()
        ):
            return path
    paths = ", ".join(str(path) for path in checked)
    raise FileNotFoundError(
        "legacy alphazero-general-ori was not found; set "
        f"ALPHAZERO_ORI_PATH (checked: {paths})"
    )


def _prepare_genbu_import() -> None:
    if not DLSPLENDOR_ROOT.is_dir():
        raise FileNotFoundError(f"dlsplendor repository not found: {DLSPLENDOR_ROOT}")
    legacy_root = _resolve_alphazero_path()
    for path in (DLSPLENDOR_ROOT, legacy_root):
        value = str(path)
        if value not in sys.path:
            sys.path.insert(0, value)

    # The legacy engine enables Numba's on-disk cache at import time.  Its
    # source tree may be read-only (for example in a managed workspace), so use
    # a writable process cache unless the caller selected one explicitly.
    os.environ.setdefault(
        "NUMBA_CACHE_DIR",
        str(Path(tempfile.gettempdir()) / "csplendor-numba-cache"),
    )


def _create_genbu_adapter(weights_path: Path):
    _prepare_genbu_import()
    from dlsplendor.search.genbu_adapter import GenbuAdapter

    return GenbuAdapter(weights_path=str(weights_path))


def _create_compatible_genbu_mcts(
    adapter,
    *,
    time_limit: float,
    num_simulations: int,
    seed: int,
):
    """Bridge the legacy Genbu policy to the current dlsplendor MCTS API."""
    from dlsplendor.config import SearchConfig
    from dlsplendor.search.genbu_adapter import GenbuMCTS
    from dlsplendor.search.mcts import ChanceNode

    class PuzzleGenbuMCTS(GenbuMCTS):
        def __init__(self, legacy_adapter, config, rng_seed: int):
            super().__init__(legacy_adapter, config)

            # GenbuMCTS intentionally bypasses MCTS.__init__ because its legacy
            # network has no current StateEncoder.  Initialize the bookkeeping
            # added by newer dlsplendor MCTS versions while keeping that
            # intentional encoder bypass.
            self.inference_client = None
            self.stall_guard_enabled = False
            self._stall_observed_counts = {}
            self._stall_recorded_events = set()
            self._stall_token_noop_streaks = [0, 0]
            self._roots = {}
            self._pending_prior_refreshes = None
            self._active_root = None
            self._gumbel_state = None
            self._root_visit_floor = None
            self._root_visit_floor_baseline = None
            self._strategic_payment_filter_enabled = False
            self.rng = random.Random(rng_seed)

        def _policy_value(
            self,
            game: cs.Game,
            legal_ids: Sequence[int],
        ) -> tuple[list[float], float]:
            probabilities, value = self.adapter.get_action_probs(game)
            actions = list(game.legal_actions)
            if len(probabilities) != len(actions):
                raise RuntimeError(
                    "Genbu policy size does not match csplendor legal actions"
                )
            probabilities_by_id: dict[int, float] = {}
            for action, probability in zip(actions, probabilities):
                action_id = int(self.action_encoder.encode(action, game))
                probabilities_by_id[action_id] = (
                    probabilities_by_id.get(action_id, 0.0) + float(probability)
                )
            return (
                [probabilities_by_id.get(int(action_id), 0.0) for action_id in legal_ids],
                float(value),
            )

        def _expand_and_evaluate_batch(self, leaves):
            self._initialize_deferred_chance_nodes(
                [
                    node
                    for node, _ in leaves
                    if isinstance(node, ChanceNode) and not node.is_expanded
                ]
            )
            values = np.empty(len(leaves), dtype=np.float32)
            for index, (node, game) in enumerate(leaves):
                if isinstance(node, ChanceNode):
                    values[index] = node.q_value
                    continue
                custom_value = self._custom_terminal_value(node, game)
                if custom_value is not None:
                    values[index] = custom_value
                    continue
                if game.is_game_over():
                    winner = int(game.winner)
                    values[index] = (
                        0.0
                        if winner == -2
                        else (1.0 if winner == node.current_player else -1.0)
                    )
                    continue
                legal_ids, _, _ = self._legal_action_ids(game)
                if not legal_ids:
                    values[index] = 0.0
                    continue
                probabilities, value = self._policy_value(game, legal_ids)
                self._set_legal_priors(node, legal_ids, probabilities)
                node.is_expanded = True
                values[index] = value

            prior_refreshes = (
                list(self._pending_prior_refreshes.values())
                if self._pending_prior_refreshes is not None
                else []
            )
            for node, game, legal_ids in prior_refreshes:
                probabilities, _ = self._policy_value(game, legal_ids)
                self._set_legal_priors(node, legal_ids, probabilities)
                node.is_expanded = True
            return values

        def _ensure_priors(self, node, game, legal_ids) -> None:
            if all(action_id in node.child_priors for action_id in legal_ids):
                return
            probabilities, _ = self._policy_value(game, legal_ids)
            self._set_legal_priors(node, legal_ids, probabilities)

        def _batch_values_by_perspective(self, games, perspective_players):
            if len(games) != len(perspective_players):
                raise ValueError("each value game requires one perspective")
            values = np.empty(len(games), dtype=np.float32)
            for index, (game, perspective_player) in enumerate(
                zip(games, perspective_players)
            ):
                terminal = self._terminal_value_for_player(game, perspective_player)
                if terminal is not None:
                    values[index] = terminal
                    continue
                _, value = self.adapter.get_action_probs(game)
                values[index] = (
                    float(value)
                    if int(game.current_player) == int(perspective_player)
                    else -float(value)
                )
            return values

    config = SearchConfig(
        time_limit_ms=max(0, int(float(time_limit) * 1000)),
        num_simulations=max(1, int(num_simulations)),
    )
    return PuzzleGenbuMCTS(adapter, config, int(seed))


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
        self.adapter = _create_genbu_adapter(weights_path)
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
            self.adapter._mcts = _create_compatible_genbu_mcts(
                self.adapter,
                time_limit=self.time_limit,
                num_simulations=self.num_simulations,
                seed=self.rng.getrandbits(64),
            )
        action_id, info = self.adapter._mcts.search(
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
                action_id = self.rng.choice(ranked[: self.top_action_count])
        action_index = next(
            (
                index
                for index, action in enumerate(actions)
                if int(self.adapter._mcts.action_encoder.encode(action, game))
                == int(action_id)
            ),
            -1,
        )
        if action_index < 0:
            raise RuntimeError(
                f"Genbu returned unknown stable action id: {action_id}"
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
    "_resolve_alphazero_path",
    "create_genbu_players",
]
