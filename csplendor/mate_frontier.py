"""JSON-safe, one-node expansion for reveal-verified mate proofs."""

from __future__ import annotations

import base64
import binascii
from typing import Any, Optional, Sequence

from ._csplendor import (
    Action,
    Game,
    solve_reveal_verified_frontier_cpp,
)
from .api.usi_kifu import action_to_usi, game_to_spn, spn_to_game

MATE_FRONTIER_FORMAT = "csplendor_mate_frontier_v1"


def encode_mate_frontier_state(game: Game) -> str:
    """Encode every rule-relevant field for the next lazy expansion."""
    return base64.b64encode(bytes(game.serialize_snapshot())).decode("ascii")


def decode_mate_frontier_state(token: str) -> Game:
    """Restore a state emitted by :func:`encode_mate_frontier_state`."""
    if not isinstance(token, str) or not token:
        raise ValueError("mate frontier state must be a non-empty base64 string")
    try:
        raw = base64.b64decode(token.encode("ascii"), validate=True)
    except (UnicodeEncodeError, binascii.Error) as error:
        raise ValueError("mate frontier state is not valid base64") from error
    return Game.deserialize_snapshot(raw)


def load_mate_frontier_game(
    *, position: Optional[str] = None, state: Optional[str] = None
) -> Game:
    """Load a root SPN or an exact child-state token.

    A state token takes precedence because SPN intentionally omits phase fields
    such as ``waiting_noble``, ``final_round`` and ``winner``.
    """
    if state:
        return decode_mate_frontier_state(state)
    if position and position.strip():
        return spn_to_game(position)
    raise ValueError("position or state is required")


def _exact_spn(game: Game) -> str:
    return game_to_spn(
        game,
        reveal_hidden_reserved_ids=True,
        require_purchased_card_ids=True,
    )


def _node_kind(game: Game) -> str:
    if game.is_game_over():
        return "terminal"
    if (
        bool(game.board.final_round)
        and not bool(game.board.waiting_noble)
        and int(game.current_player) == 1
    ):
        return "final_round_summary"
    return "state"


def _resolution(game: Game, attacker: int) -> Optional[str]:
    if not game.is_game_over():
        return None
    return "attacker_win" if int(game.winner) == attacker else "non_attacker_win"


def expand_mate_frontier(
    game: Game,
    *,
    attacker: int,
    depth: int,
    max_nodes: int = 5_000_000,
    time_limit_seconds: float = 30.0,
    edge_limit: int = 250_000,
    preferred_attacker_actions: Sequence[int] = (),
) -> dict[str, Any]:
    """Verify ``game`` and materialize only its immediate proof frontier."""
    if attacker not in (0, 1):
        raise ValueError("attacker must be 0 or 1")
    if depth < 0:
        raise ValueError("depth must be non-negative")
    if max_nodes < 0 or time_limit_seconds < 0 or edge_limit < 0:
        raise ValueError("search limits must be non-negative")

    native = solve_reveal_verified_frontier_cpp(
        game,
        attacker=attacker,
        depth=depth,
        max_nodes=max_nodes,
        time_limit_seconds=time_limit_seconds,
        edge_limit=edge_limit,
        preferred_attacker_actions=[
            int(action) for action in preferred_attacker_actions
        ],
    )
    edges: list[dict[str, Any]] = []
    for raw_edge in native["edges"]:
        child = raw_edge["child_game"]
        action_code = int(raw_edge["action_code"])
        action = Action.unpack(action_code)
        child_depth = int(raw_edge["child_depth"])
        edges.append(
            {
                "action_code": action_code,
                "action_usi": action_to_usi(action, game=game),
                "reveal_card": raw_edge["reveal_card"],
                "child_depth": child_depth,
                "child_player": int(child.current_player),
                "child_winner": int(child.winner),
                "child_waiting_noble": bool(child.board.waiting_noble),
                "child_kind": _node_kind(child),
                "child_resolution": _resolution(child, attacker),
                "child_position": _exact_spn(child),
                "child_state": encode_mate_frontier_state(child),
            }
        )

    return {
        "format": MATE_FRONTIER_FORMAT,
        "proven": bool(native["proven"]),
        "complete": bool(native["complete"]),
        "attacker": int(native["attacker"]),
        "depth": int(native["depth"]),
        "player": int(native["player"]),
        "winner": int(native["winner"]),
        "waiting_noble": bool(native["waiting_noble"]),
        "kind": str(native["kind"]),
        "resolution": native["resolution"],
        "reason": str(native["reason"]),
        "unknown_reason": native["unknown_reason"],
        "position": _exact_spn(game),
        "state": encode_mate_frontier_state(game),
        "memoized_states": int(native["memoized_states"]),
        "stats": dict(native["stats"]),
        "edges": edges,
    }


__all__ = [
    "MATE_FRONTIER_FORMAT",
    "decode_mate_frontier_state",
    "encode_mate_frontier_state",
    "expand_mate_frontier",
    "load_mate_frontier_game",
]
