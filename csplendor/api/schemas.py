from enum import IntEnum
from typing import List, Optional

from pydantic import BaseModel, Field

from .._csplendor import GemType as CoreGemType


class GemType(IntEnum):
    DIAMOND = int(CoreGemType.DIAMOND)
    SAPPHIRE = int(CoreGemType.SAPPHIRE)
    EMERALD = int(CoreGemType.EMERALD)
    RUBY = int(CoreGemType.RUBY)
    ONYX = int(CoreGemType.ONYX)
    GOLD = int(CoreGemType.GOLD)


class ActionType(IntEnum):
    TAKE_DIFFERENT = 0
    TAKE_SAME = 1
    RESERVE_VISIBLE = 2
    RESERVE_DECK = 3
    PURCHASE = 4
    VISIT_NOBLE = 5
    PASS = 6


class CardSchema(BaseModel):
    id: int
    level: int
    points: int
    bonus: GemType
    cost: List[int]  # Diamond, Sapphire, Emerald, Ruby, Onyx


class NobleSchema(BaseModel):
    id: int
    points: int
    requirement: List[int]  # Diamond, Sapphire, Emerald, Ruby, Onyx


class ActionSchema(BaseModel):
    type: ActionType
    take: Optional[List[int]] = None  # Diamond, Sapphire, Emerald, Ruby, Onyx
    card_id: Optional[int] = None
    deck_level: Optional[int] = None
    from_reserved: Optional[bool] = None
    gold_as: Optional[List[int]] = None  # Five colors in canonical order
    return_gems: Optional[List[int]] = None  # Five colors above, then Gold
    noble_choice: Optional[int] = None
    usi: Optional[str] = None  # USI move notation


class PlayerSchema(BaseModel):
    index: int
    gems: List[int]  # Diamond, Sapphire, Emerald, Ruby, Onyx, Gold
    bonuses: List[int]  # Five non-Gold colors in canonical order
    points: int
    reserved_cards: List[int]
    purchased_cards: List[int]
    acquired_nobles: List[int] = Field(default_factory=list)


class BoardSchema(BaseModel):
    bank: List[int]  # Diamond, Sapphire, Emerald, Ruby, Onyx, Gold
    visible_cards: List[List[int]]  # [level][slot]
    deck_counts: List[int]
    nobles: List[int]
    current_player: int
    turn: int
    waiting_noble: bool
    game_over: bool
    winner: int


class GameStateSchema(BaseModel):
    board: BoardSchema
    players: List[PlayerSchema]
    legal_actions: List[ActionSchema]
