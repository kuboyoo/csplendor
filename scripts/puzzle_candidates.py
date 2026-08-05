"""Candidate playout generation independent of external engine details."""

from __future__ import annotations

import random
from typing import Any, Callable, Iterator, Optional, Sequence

import csplendor as cs
from scripts.puzzle_engine_adapter import PuzzlePlayer


def generate_candidate_positions(
    rng: random.Random,
    *,
    players: Sequence[PuzzlePlayer],
    game_seed: int,
    min_playout_plies: int,
    max_playout_plies: int,
    progress: Optional[Any] = None,
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
            progress.emit(
                "genbu_playout",
                attempt=attempt,
                ply=ply,
                start_ply=start_ply,
            )
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
    progress: Optional[Any] = None,
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


__all__ = ["generate_candidate_position", "generate_candidate_positions"]
