"""Solution uniqueness and countermate validation for generated puzzles."""

from __future__ import annotations

from typing import Any, Callable, Optional

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi
from scripts.mate_solver import MATE, SearchResult, SolverOptions


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


def completed_turn_children(
    game: cs.Game,
    action: cs.Action,
) -> list[cs.Game]:
    child = game.clone_light()
    if not child.apply(action, False):
        raise RuntimeError("engine rejected a generated legal action")
    if child.is_game_over() or int(child.board.current_player) != int(
        game.board.current_player
    ):
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
    args: Any,
    *,
    max_winning_actions: int,
    arg_value: Callable[[Any, str, object], object],
    visible_only_prefilter: Callable[[cs.Game, Any], SearchResult],
    player0_win: str,
    player1_win: str,
) -> list[str]:
    expected = player0_win if int(game.board.current_player) == 0 else player1_win
    winning_actions: list[str] = []
    action_limit = int(arg_value(args, "visible_uniqueness_action_limit", 16))
    actions = list(game.legal_actions)
    if action_limit:
        actions = actions[:action_limit]
    for action in actions:
        for child in completed_turn_children(game, action):
            if child.is_game_over():
                winner = int(child.board.winner)
                status = (
                    player0_win if winner == 0 else player1_win if winner == 1 else None
                )
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
    solve_reveal_verified_mate: Callable[..., SearchResult],
    progress: Optional[Any] = None,
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
        action for action in game.legal_actions if int(action.pack()) != correct_pack
    ]
    if action_limit:
        alternatives = alternatives[:action_limit]

    blunders = []
    checks = 0
    for alternative_index, action in enumerate(alternatives, start=1):
        for child in completed_turn_children(game, action):
            if child.is_game_over() or int(child.board.current_player) != defender:
                continue
            checks += 1
            if progress is not None:
                progress.emit(
                    "countermate_search",
                    force=True,
                    attempt=attempt,
                    alternative=(f"{alternative_index}/{len(alternatives)}"),
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
            blunders.append(
                {
                    "action": action_to_usi(action, game=game),
                    "opponent": defender,
                    "forced_win_depth": int(countermate.depth),
                }
            )
            break
        if len(blunders) >= min_losing_alternatives:
            break
    return blunders, checks


__all__ = [
    "completed_turn_children",
    "find_countermate_blunders",
    "find_verified_winning_actions",
    "find_visible_winning_actions",
]
