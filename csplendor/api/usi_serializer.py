"""Canonical USI serialization for native engine actions."""

from typing import List, Optional

from .. import ActionType as CoreActionType
from .. import get_card
from .usi_tokens import (
    counts_to_letters,
    gold_as_to_token,
    pay_counts_to_token,
    safe_int,
)


def compute_purchase_payment(action, game) -> Optional[List[int]]:
    if int(action.type) != int(CoreActionType.PURCHASE) or game is None:
        return None
    try:
        card = get_card(int(action.card_id))
        player = game.board.players[game.board.current_player]
        bonuses = [int(value) for value in player.bonuses]
    except Exception:
        return None
    gold_as = [int(value) for value in action.gold_as[:5]]
    payment = [0, 0, 0, 0, 0, 0]
    for index in range(5):
        effective = max(0, int(card.cost[index]) - bonuses[index])
        payment[index] = max(0, effective - gold_as[index])
    payment[5] = sum(gold_as)
    return payment


def card_level_from_id(card_id: int) -> int:
    if 0 <= card_id <= 39:
        return 1
    if 40 <= card_id <= 69:
        return 2
    if 70 <= card_id <= 89:
        return 3
    return 0


def action_to_usi(action, game=None) -> str:
    if action.type in (
        CoreActionType.TAKE_DIFFERENT,
        CoreActionType.TAKE_SAME,
    ):
        take = counts_to_letters(list(action.take), include_gold=True)
        if not take:
            return "pass"
        returned = counts_to_letters(
            list(action.return_gems), include_gold=True
        )
        return f"take:{take}" + (
            f"/return:{returned}" if returned else ""
        )
    if action.type == CoreActionType.RESERVE_VISIBLE:
        base = f"reserve:C{safe_int(action.card_id)}"
        returned = counts_to_letters(
            list(action.return_gems), include_gold=True
        )
        return base + (f"/return:{returned}" if returned else "")
    if action.type == CoreActionType.RESERVE_DECK:
        base = f"reserve:L{safe_int(action.deck_level) + 1}"
        returned = counts_to_letters(
            list(action.return_gems), include_gold=True
        )
        return base + (f"/return:{returned}" if returned else "")
    if action.type == CoreActionType.PURCHASE:
        base = f"buy:C{safe_int(action.card_id)}"
        payment = compute_purchase_payment(action, game)
        if payment is not None:
            return base + f"/pay:{pay_counts_to_token(payment)}"
        gold = gold_as_to_token(list(action.gold_as))
        return base + (f"/gold:{gold}" if gold else "")
    if action.type == CoreActionType.VISIT_NOBLE:
        return f"noble:N{safe_int(action.noble_choice)}"
    return "pass"
