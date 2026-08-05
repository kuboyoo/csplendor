"""Data-transfer values for parsed USI moves."""

from dataclasses import dataclass
from typing import List, Optional


@dataclass
class ParsedUSIMove:
    kind: str
    take: Optional[List[int]] = None
    return_gems: Optional[List[int]] = None
    card_id: Optional[int] = None
    deck_level: Optional[int] = None
    gold_as: Optional[List[int]] = None
    pay_gems: Optional[List[int]] = None
    noble_id: Optional[int] = None
