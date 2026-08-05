"""Resolve parsed USI DTOs against the engine's ordered legal actions."""

from typing import List, Optional, Sequence, Tuple

from .. import ActionType as CoreActionType
from .usi_parser import parse_usi_move
from .usi_serializer import action_to_usi, compute_purchase_payment
from .usi_tokens import safe_int


def _returns_match(
    expected: Optional[List[int]], actual: Sequence[int]
) -> bool:
    if expected is None:
        return sum(safe_int(value) for value in actual) == 0
    return [int(value) for value in actual[:6]] == expected[:6]


def find_legal_action_index_by_usi(game, usi_move: str) -> int:
    value = usi_move.strip()
    legal_actions = game.legal_actions
    for index, action in enumerate(legal_actions):
        if action_to_usi(action, game=game) == value:
            return index

    parsed = parse_usi_move(value)
    if parsed.kind == "pass":
        for index, action in enumerate(legal_actions):
            if action.type == CoreActionType.PASS:
                return index
        raise ValueError(f"pass is not legal in current state: {value}")
    if parsed.kind == "take":
        for index, action in enumerate(legal_actions):
            if action.type not in (
                CoreActionType.TAKE_DIFFERENT,
                CoreActionType.TAKE_SAME,
            ):
                continue
            if [int(item) for item in action.take[:6]] != parsed.take:
                continue
            if _returns_match(parsed.return_gems, action.return_gems):
                return index
        raise ValueError(f"no legal take action matches USI move: {value}")
    if parsed.kind == "reserve_visible":
        for index, action in enumerate(legal_actions):
            if action.type != CoreActionType.RESERVE_VISIBLE:
                continue
            if int(action.card_id) == int(parsed.card_id) and _returns_match(
                parsed.return_gems, action.return_gems
            ):
                return index
        raise ValueError(
            f"no legal reserve-visible action matches USI move: {value}"
        )
    if parsed.kind == "reserve_deck":
        for index, action in enumerate(legal_actions):
            if action.type != CoreActionType.RESERVE_DECK:
                continue
            if int(action.deck_level) == int(
                parsed.deck_level
            ) and _returns_match(parsed.return_gems, action.return_gems):
                return index
        raise ValueError(
            f"no legal reserve-deck action matches USI move: {value}"
        )
    if parsed.kind == "buy":
        candidates: List[Tuple[int, int, int]] = []
        for index, action in enumerate(legal_actions):
            if action.type != CoreActionType.PURCHASE:
                continue
            if int(action.card_id) != int(parsed.card_id):
                continue
            gold_as = [int(item) for item in action.gold_as[:5]]
            if parsed.gold_as is not None and gold_as != parsed.gold_as:
                continue
            payment = compute_purchase_payment(action, game)
            if parsed.pay_gems is not None and payment != parsed.pay_gems:
                continue
            candidates.append(
                (
                    index,
                    sum(gold_as),
                    sum(int(item) for item in action.return_gems[:6]),
                )
            )
        if not candidates:
            raise ValueError(f"no legal buy action matches USI move: {value}")
        candidates.sort(key=lambda item: (item[1], item[2], item[0]))
        return candidates[0][0]
    if parsed.kind == "noble":
        for index, action in enumerate(legal_actions):
            if action.type == CoreActionType.VISIT_NOBLE and int(
                action.noble_choice
            ) == int(parsed.noble_id):
                return index
        raise ValueError(f"no legal noble action matches USI move: {value}")
    raise ValueError(f"unsupported USI move kind: {parsed.kind}")
