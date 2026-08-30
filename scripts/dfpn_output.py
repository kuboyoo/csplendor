"""Pure strategy-DAG codecs and KIFU serialization for the DFPN solver."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

import csplendor as cs
from csplendor.api.usi_kifu import (
    build_kifu_text,
    find_legal_action_index_by_usi,
    game_to_spn,
    now_iso,
)


MATE_KIFU_REVEAL_COMMENT_PREFIX = "reveal:C"
_CARD_MASK_BYTES = 12


def _json_size_bytes(value: Any) -> int:
    return len(
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
        ).encode("utf-8")
    )


def _cards_to_mask_hex(cards: Sequence[int]) -> str:
    mask = bytearray(_CARD_MASK_BYTES)
    for raw_card in cards:
        card = int(raw_card)
        if not 0 <= card < _CARD_MASK_BYTES * 8:
            continue
        mask[card // 8] |= 1 << (card % 8)
    return mask.hex()


def _mask_hex_to_cards(mask_hex: str) -> List[int]:
    data = bytes.fromhex(mask_hex)
    cards: List[int] = []
    for card in range(min(len(data) * 8, 90)):
        if data[card // 8] & (1 << (card % 8)):
            cards.append(card)
    return cards


def _nullable_int(value: Any, default: int = -1) -> int:
    return default if value is None else int(value)


def _optional_int(value: int) -> Optional[int]:
    return None if int(value) < 0 else int(value)


def _compact_card_classes(reveal_cards: Sequence[int]) -> Dict[str, Any]:
    wanted = {int(card) for card in reveal_cards}
    classes: Dict[
        Tuple[int, int, int, Tuple[int, int, int, int, int]],
        List[int],
    ] = {}
    for raw_card in cs.get_all_cards():
        card_id = int(raw_card.id)
        if wanted and card_id not in wanted:
            continue
        key = (
            int(raw_card.level),
            int(raw_card.points),
            int(raw_card.bonus),
            tuple(int(raw_card.cost[color]) for color in range(5)),
        )
        classes.setdefault(key, []).append(card_id)
    return {
        "card_class_columns": [
            "level",
            "points",
            "bonus",
            "cost",
            "cards_mask",
        ],
        "card_classes": [
            [level, points, bonus, list(cost), _cards_to_mask_hex(cards)]
            for (level, points, bonus, cost), cards in sorted(classes.items())
        ],
    }


def _strategy_dag_semantics() -> Dict[str, str]:
    return {
        "attacker_nodes": (
            "one_proven_action_with_all_nondeterministic_outcomes"
        ),
        "defender_nodes": (
            "all_legal_actions_with_all_nondeterministic_outcomes"
        ),
        "reveal_card": "concrete_card_id_preserved_in_reveal_groups",
        "oracle_fields": (
            "deprecated_and_always_empty_for_legal_strategy_dag"
        ),
        "shared_states": "referenced_by_node_id",
        "resolved_leaves": (
            "terminal_results_after_expanded_final_round_responses"
        ),
        "compact_edges": (
            "edge rows reference action templates and exact reveal card bitsets"
        ),
    }


def proof_dag_to_compact(
    proof_dag: Dict[str, Any],
    *,
    include_card_classes: bool = False,
) -> Dict[str, Any]:
    """Encode a v1 strategy DAG without losing concrete reveal-card coverage."""
    if proof_dag.get("format") == "strategy_dag_compact_v1":
        compact = dict(proof_dag)
        compact.setdefault("semantics", _strategy_dag_semantics())
        if include_card_classes and "card_classes" not in compact:
            compact.update(
                _compact_card_classes(
                    [
                        card
                        for mask in compact.get("reveal_groups", [])
                        for card in _mask_hex_to_cards(str(mask))
                    ]
                )
            )
        return compact

    kind_ids: Dict[str, int] = {}
    resolution_ids: Dict[str, int] = {}
    action_ids: Dict[Tuple[Any, ...], int] = {}
    reveal_group_ids: Dict[Tuple[int, ...], int] = {}
    kind_strings: List[str] = []
    resolution_strings: List[str] = []
    action_templates: List[List[Any]] = []
    reveal_groups: List[str] = []
    nodes: List[List[Any]] = []
    edges: List[List[int]] = []
    all_reveal_cards: List[int] = []

    def intern_string(
        table: Dict[str, int], values: List[str], value: str
    ) -> int:
        known = table.get(value)
        if known is not None:
            return known
        table[value] = len(values)
        values.append(value)
        return table[value]

    def intern_action(edge: Dict[str, Any]) -> int:
        gold_as = tuple(
            int(value)
            for value in edge.get("oracle_gold_as", [0, 0, 0, 0, 0])
        )
        key = (
            int(edge.get("action_code", 0)),
            _nullable_int(edge.get("oracle_card")),
            bool(edge.get("oracle_reserve", False)),
            _nullable_int(edge.get("oracle_reserve_card")),
            _nullable_int(edge.get("oracle_return_color")),
            gold_as,
        )
        if key[1] >= 0 or key[2] or key[3] >= 0 or key[4] >= 0:
            raise ValueError("strategy DAG contains non-replayable oracle edge")
        known = action_ids.get(key)
        if known is not None:
            return known
        action_ids[key] = len(action_templates)
        action_templates.append(
            [
                key[0],
                key[1],
                1 if key[2] else 0,
                key[3],
                key[4],
                list(gold_as),
            ]
        )
        return action_ids[key]

    def intern_reveal_group(cards: Sequence[int]) -> int:
        unique_cards = tuple(sorted({int(card) for card in cards}))
        known = reveal_group_ids.get(unique_cards)
        if known is not None:
            return known
        reveal_group_ids[unique_cards] = len(reveal_groups)
        reveal_groups.append(_cards_to_mask_hex(unique_cards))
        all_reveal_cards.extend(unique_cards)
        return reveal_group_ids[unique_cards]

    for raw_node in proof_dag.get("nodes", []):
        node = dict(raw_node)
        edge_start = len(edges)
        grouped: Dict[Tuple[int, int, bool], Dict[str, Any]] = {}
        order: List[Tuple[int, int, bool]] = []
        for raw_edge in node.get("children", []):
            edge = dict(raw_edge)
            action_index = intern_action(edge)
            child = int(edge.get("child", 0))
            reveal = edge.get("reveal_card")
            key = (action_index, child, reveal is not None)
            if key not in grouped:
                grouped[key] = {"cards": []}
                order.append(key)
            if reveal is not None:
                grouped[key]["cards"].append(int(reveal))
        for action_index, child, has_reveal in order:
            reveal_group = (
                intern_reveal_group(
                    grouped[(action_index, child, has_reveal)]["cards"]
                )
                if has_reveal
                else -1
            )
            edges.append([action_index, reveal_group, child])

        kind_index = intern_string(
            kind_ids,
            kind_strings,
            str(node.get("kind", "state")),
        )
        resolution = node.get("resolution")
        resolution_index = (
            -1
            if resolution is None
            else intern_string(
                resolution_ids,
                resolution_strings,
                str(resolution),
            )
        )
        nodes.append(
            [
                int(node.get("id", len(nodes))),
                int(node.get("player", -1)),
                int(node.get("depth", 0)),
                kind_index,
                resolution_index,
                edge_start,
                len(edges) - edge_start,
            ]
        )

    compact = {
        "format": "strategy_dag_compact_v1",
        "source_format": proof_dag.get("format", "strategy_dag_v1"),
        "requested": bool(proof_dag.get("requested", True)),
        "complete": bool(proof_dag.get("complete", False)),
        "validated": bool(proof_dag.get("validated", False)),
        "omitted_reason": proof_dag.get("omitted_reason"),
        "root": proof_dag.get("root"),
        "semantics": _strategy_dag_semantics(),
        "reveal_group_encoding": "card_bitset_le_hex_v1",
        "node_columns": [
            "id",
            "player",
            "depth",
            "kind",
            "resolution",
            "edge_start",
            "edge_count",
        ],
        "edge_columns": ["action", "reveal_group", "child"],
        "action_template_columns": [
            "action_code",
            "oracle_card",
            "oracle_reserve",
            "oracle_reserve_card",
            "oracle_return_color",
            "oracle_gold_as",
        ],
        "kind_strings": kind_strings,
        "resolution_strings": resolution_strings,
        "action_templates": action_templates,
        "reveal_groups": reveal_groups,
        "nodes": nodes,
        "edges": edges,
    }
    if include_card_classes:
        compact.update(_compact_card_classes(all_reveal_cards))
    return compact


def compact_proof_dag_to_v1(compact: Dict[str, Any]) -> Dict[str, Any]:
    """Expand compact rows into the legacy v1 dictionary shape for validators."""
    if compact.get("format") != "strategy_dag_compact_v1":
        return dict(compact)

    kind_strings = [str(value) for value in compact.get("kind_strings", [])]
    resolution_strings = [
        str(value) for value in compact.get("resolution_strings", [])
    ]
    action_templates = list(compact.get("action_templates", []))
    reveal_groups = list(compact.get("reveal_groups", []))
    edge_rows = list(compact.get("edges", []))
    nodes: List[Dict[str, Any]] = []

    for raw_node in compact.get("nodes", []):
        row = list(raw_node)
        if len(row) < 7:
            raise ValueError("compact strategy DAG node row is too short")
        (
            node_id,
            player,
            depth,
            kind_index,
            resolution_index,
            edge_start,
            edge_count,
        ) = [int(value) for value in row[:7]]
        children: List[Dict[str, Any]] = []
        for raw_edge in edge_rows[edge_start : edge_start + edge_count]:
            edge_row = [int(value) for value in raw_edge[:3]]
            action_index, reveal_group_index, child = edge_row
            action = list(action_templates[action_index])
            action_code = int(action[0])
            oracle_card = int(action[1])
            oracle_reserve = bool(int(action[2]))
            oracle_reserve_card = int(action[3])
            oracle_return_color = int(action[4])
            oracle_gold_as = [int(value) for value in action[5]]
            reveal_cards: List[Optional[int]]
            if reveal_group_index < 0:
                reveal_cards = [None]
            else:
                reveal_cards = _mask_hex_to_cards(
                    str(reveal_groups[reveal_group_index])
                )
            for reveal_card in reveal_cards:
                children.append(
                    {
                        "action_code": action_code,
                        "reveal_card": reveal_card,
                        "oracle_card": _optional_int(oracle_card),
                        "oracle_reserve": oracle_reserve,
                        "oracle_reserve_card": _optional_int(
                            oracle_reserve_card
                        ),
                        "oracle_return_color": _optional_int(
                            oracle_return_color
                        ),
                        "oracle_gold_as": oracle_gold_as,
                        "child": child,
                    }
                )
        nodes.append(
            {
                "id": node_id,
                "player": player,
                "depth": depth,
                "kind": (
                    kind_strings[kind_index]
                    if 0 <= kind_index < len(kind_strings)
                    else "state"
                ),
                "resolution": (
                    resolution_strings[resolution_index]
                    if 0 <= resolution_index < len(resolution_strings)
                    else None
                ),
                "children": children,
            }
        )

    return {
        "format": "strategy_dag_v1",
        "requested": bool(compact.get("requested", True)),
        "complete": bool(compact.get("complete", False)),
        "validated": bool(compact.get("validated", False)),
        "omitted_reason": compact.get("omitted_reason"),
        "root": compact.get("root"),
        "nodes": nodes,
        "semantics": compact.get("semantics"),
    }


def strategy_dag_size_report(
    v1_dag: Dict[str, Any], compact_dag: Dict[str, Any]
) -> Dict[str, Any]:
    v1_bytes = _json_size_bytes(v1_dag)
    compact_bytes = _json_size_bytes(compact_dag)
    return {
        "v1_json_bytes": v1_bytes,
        "compact_json_bytes": compact_bytes,
        "compression_ratio": (compact_bytes / v1_bytes) if v1_bytes else None,
        "saved_bytes": max(0, v1_bytes - compact_bytes),
    }


def strategy_dag_node_count(dag: Dict[str, Any]) -> int:
    return len(dag.get("nodes", [])) if isinstance(dag, dict) else 0


def strategy_dag_max_children(
    dag: Dict[str, Any], *, player: Optional[int] = None
) -> int:
    v1 = (
        compact_proof_dag_to_v1(dag)
        if dag.get("format") == "strategy_dag_compact_v1"
        else dag
    )
    return max(
        (
            len(node.get("children", []))
            for node in v1.get("nodes", [])
            if player is None
            or int(node.get("player", -1)) == int(player)
        ),
        default=0,
    )


def _default_card_level(card_id: int) -> int:
    return int(cs.get_card(int(card_id)).level)


def proof_tree_to_kifu_text(
    game: cs.Game,
    proof_tree: Dict[str, Any],
    *,
    attacker: int,
    _now_iso: Optional[Callable[[], str]] = None,
    _card_level: Optional[Callable[[int], int]] = None,
) -> str:
    """Serialize one replayable principal line from a DFPN proof tree."""
    moves: List[Dict[str, object]] = []

    def walk(node: Any) -> None:
        if not isinstance(node, dict):
            return

        if node.get("kind") in {"action", "noble"}:
            action = node.get("action")
            usi = action.get("usi") if isinstance(action, dict) else None
            if usi:
                moves.append(
                    {
                        "player": int(node.get("current_player", 0)),
                        "usi": str(usi),
                    }
                )

        reveal_card = node.get("reveal_card")
        if reveal_card is not None and moves:
            moves[-1]["comment"] = (
                f"{MATE_KIFU_REVEAL_COMMENT_PREFIX}{int(reveal_card)}"
            )

        children = node.get("children")
        if isinstance(children, list) and children:
            walk(children[0])

    walk(proof_tree)
    return _build_mate_kifu_text(
        game,
        moves,
        attacker=attacker,
        _now_iso=_now_iso,
        _card_level=_card_level,
    )


def principal_line_to_kifu_text(
    game: cs.Game,
    line: Sequence[Dict[str, Any]],
    *,
    attacker: int,
    reveal_hidden_reserved_ids: bool = False,
    _now_iso: Optional[Callable[[], str]] = None,
    _card_level: Optional[Callable[[int], int]] = None,
) -> str:
    """Serialize a solver principal-line array as replayable KIFU."""
    moves = []
    for entry in line:
        action = entry.get("action")
        usi = action.get("usi") if isinstance(action, dict) else None
        if usi:
            move = {
                "player": int(entry.get("player", 0)),
                "usi": str(usi),
            }
            reveal_card = entry.get("reveal_card")
            if reveal_card is not None:
                move["comment"] = (
                    f"{MATE_KIFU_REVEAL_COMMENT_PREFIX}{int(reveal_card)}"
                )
            moves.append(move)
    return _build_mate_kifu_text(
        game,
        moves,
        attacker=attacker,
        reveal_hidden_reserved_ids=reveal_hidden_reserved_ids,
        _now_iso=_now_iso,
        _card_level=_card_level,
    )


def _build_mate_kifu_text(
    game: cs.Game,
    moves: Sequence[Dict[str, object]],
    *,
    attacker: int,
    reveal_hidden_reserved_ids: bool = False,
    _now_iso: Optional[Callable[[], str]] = None,
    _card_level: Optional[Callable[[int], int]] = None,
) -> str:
    replay_moves = _with_implicit_noble_visits(
        game,
        moves,
        _card_level=_card_level,
    )
    timestamp = (_now_iso or now_iso)()
    return build_kifu_text(
        headers={
            "Format": "Splendor KIFU v1.0",
            "Players": "2",
            "Player0": "DFPN Attacker" if attacker == 0 else "Defender",
            "Player1": "DFPN Attacker" if attacker == 1 else "Defender",
            "Date": timestamp,
            "Event": "DFPN Mate Principal Line",
            "MateAttacker": f"P{attacker}",
            "MateLine": "principal variation",
        },
        position=game_to_spn(
            game,
            reveal_hidden_reserved_ids=reveal_hidden_reserved_ids,
        ),
        moves=replay_moves,
        result=f"P{attacker}_WIN",
        total_turns=len(replay_moves),
    )


def _with_implicit_noble_visits(
    root_game: cs.Game,
    moves: Sequence[Dict[str, object]],
    *,
    _card_level: Optional[Callable[[int], int]] = None,
) -> List[Dict[str, object]]:
    """Add KIFU rows for noble visits that the engine applies automatically."""
    game = root_game.clone()
    replay_moves: List[Dict[str, object]] = []
    for move in moves:
        replay_move = dict(move)
        usi = str(replay_move.get("usi", "pass"))
        player = int(replay_move.get("player", game.board.current_player))
        if player != int(game.board.current_player):
            raise ValueError(
                f"KIFU move player does not match engine turn: P{player} {usi}"
            )

        action_index = find_legal_action_index_by_usi(game, usi)
        if action_index < 0:
            raise ValueError(f"no legal action matches KIFU move: {usi}")
        action = game.legal_actions[action_index]
        _set_reveal_card_for_kifu_move(
            game,
            action,
            replay_move.get("comment"),
            _card_level=_card_level,
        )

        before_nobles = {
            int(noble_id)
            for noble_id in game.board.get_player(player).acquired_nobles
        }
        if not game.apply(action, False):
            raise RuntimeError(f"engine rejected KIFU move: {usi}")
        replay_moves.append(replay_move)

        if usi.startswith("noble:"):
            continue
        after_nobles = [
            int(noble_id)
            for noble_id in game.board.get_player(player).acquired_nobles
        ]
        for noble_id in after_nobles:
            if noble_id not in before_nobles:
                replay_moves.append(
                    {
                        "player": player,
                        "usi": f"noble:N{noble_id}",
                        "time_ms": 0,
                        "comment": "auto",
                    }
                )
    return replay_moves


def _set_reveal_card_for_kifu_move(
    game: cs.Game,
    action: cs.Action,
    comment: object,
    *,
    _card_level: Optional[Callable[[int], int]] = None,
) -> None:
    if not isinstance(comment, str) or not comment.startswith(
        MATE_KIFU_REVEAL_COMMENT_PREFIX
    ):
        return
    reveal_card = int(comment[len(MATE_KIFU_REVEAL_COMMENT_PREFIX) :])
    if int(action.type) == int(cs.ActionType.RESERVE_DECK):
        level = int(action.deck_level)
    elif int(action.type) in (
        int(cs.ActionType.PURCHASE),
        int(cs.ActionType.RESERVE_VISIBLE),
    ):
        card_level = _card_level or _default_card_level
        level = int(card_level(int(action.card_id))) - 1
    else:
        return
    decks = [
        [int(card_id) for card_id in deck] for deck in game.board.decks
    ]
    if reveal_card in decks[level]:
        decks[level].remove(reveal_card)
    elif decks[level]:
        decks[level].pop()
    decks[level].append(reveal_card)
    game.board.decks = decks


def write_mate_kifu(
    path: str,
    game: cs.Game,
    proof_tree: Dict[str, Any],
    *,
    attacker: int,
    _now_iso: Optional[Callable[[], str]] = None,
    _card_level: Optional[Callable[[int], int]] = None,
) -> None:
    Path(path).write_text(
        proof_tree_to_kifu_text(
            game,
            proof_tree,
            attacker=attacker,
            _now_iso=_now_iso,
            _card_level=_card_level,
        ),
        encoding="utf-8",
    )


def write_principal_line_kifu(
    path: str,
    game: cs.Game,
    line: Sequence[Dict[str, Any]],
    *,
    attacker: int,
    _now_iso: Optional[Callable[[], str]] = None,
    _card_level: Optional[Callable[[int], int]] = None,
) -> None:
    Path(path).write_text(
        principal_line_to_kifu_text(
            game,
            line,
            attacker=attacker,
            _now_iso=_now_iso,
            _card_level=_card_level,
        ),
        encoding="utf-8",
    )
