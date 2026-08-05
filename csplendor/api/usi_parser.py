"""Tokenizer/parser for one USI move, independent of engine state."""

import re
from typing import List, Optional

from .usi_tokens import letters_to_counts, token_to_gold_as, token_to_pay_counts
from .usi_types import ParsedUSIMove


def parse_usi_move(text: str) -> ParsedUSIMove:
    value = text.strip()
    if value.lower() == "pass":
        return ParsedUSIMove(kind="pass")

    match = re.fullmatch(
        r"take:([WUGRKD]+)(?:/return:([WUGRKD]+))?",
        value,
        flags=re.IGNORECASE,
    )
    if match:
        take = letters_to_counts(match.group(1), allow_gold=False)
        returned = (
            letters_to_counts(match.group(2), allow_gold=True)
            if match.group(2)
            else None
        )
        return ParsedUSIMove(kind="take", take=take, return_gems=returned)

    match = re.fullmatch(
        r"reserve:C(\d+)(?:/return:([WUGRKD]+))?",
        value,
        flags=re.IGNORECASE,
    )
    if match:
        returned = (
            letters_to_counts(match.group(2), allow_gold=True)
            if match.group(2)
            else None
        )
        return ParsedUSIMove(
            kind="reserve_visible",
            card_id=int(match.group(1)),
            return_gems=returned,
        )

    match = re.fullmatch(
        r"reserve:L([123])(?:/return:([WUGRKD]+))?",
        value,
        flags=re.IGNORECASE,
    )
    if match:
        returned = (
            letters_to_counts(match.group(2), allow_gold=True)
            if match.group(2)
            else None
        )
        return ParsedUSIMove(
            kind="reserve_deck",
            deck_level=int(match.group(1)) - 1,
            return_gems=returned,
        )

    match = re.fullmatch(r"buy:C(\d+)(.*)", value, flags=re.IGNORECASE)
    if match:
        card_id = int(match.group(1))
        tail = match.group(2) or ""
        noble_id: Optional[int] = None
        gold: Optional[List[int]] = None
        pay_gems: Optional[List[int]] = None

        noble_match = re.search(
            r"\s+noble:N(\d+)$", tail, flags=re.IGNORECASE
        )
        if noble_match:
            noble_id = int(noble_match.group(1))
            tail = tail[: noble_match.start()]

        tail = tail.strip()
        if tail:
            for part in (part for part in tail.split("/") if part):
                token = part.strip()
                lowered = token.lower()
                if lowered.startswith("gold:"):
                    if gold is not None:
                        raise ValueError(
                            f"duplicate gold token in USI move: {text}"
                        )
                    gold = token_to_gold_as(token.split(":", 1)[1])
                elif lowered.startswith("pay:"):
                    if pay_gems is not None:
                        raise ValueError(
                            f"duplicate pay token in USI move: {text}"
                        )
                    pay_gems = token_to_pay_counts(token.split(":", 1)[1])
                else:
                    raise ValueError(f"invalid buy suffix token: {token}")
        return ParsedUSIMove(
            kind="buy",
            card_id=card_id,
            gold_as=gold,
            pay_gems=pay_gems,
            noble_id=noble_id,
        )

    match = re.fullmatch(r"noble:N(\d+)", value, flags=re.IGNORECASE)
    if match:
        return ParsedUSIMove(kind="noble", noble_id=int(match.group(1)))
    raise ValueError(f"invalid USI move: {text}")
