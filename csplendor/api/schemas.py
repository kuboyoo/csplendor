from pydantic import BaseModel, Field
from typing import List, Optional
from enum import IntEnum


class GemType(IntEnum):
    EMERALD = 0
    SAPPHIRE = 1
    RUBY = 2
    DIAMOND = 3
    ONYX = 4
    GOLD = 5


class ActionType(IntEnum):
    TAKE_DIFFERENT = 0
    TAKE_SAME = 1
    RESERVE_VISIBLE = 2
    RESERVE_DECK = 3
    PURCHASE = 4
    VISIT_NOBLE = 5


class CardSchema(BaseModel):
    id: int
    level: int
    points: int
    bonus: GemType
    cost: List[int]  # Size 5


class NobleSchema(BaseModel):
    id: int
    points: int
    requirement: List[int]  # Size 5


class ActionSchema(BaseModel):
    type: ActionType
    take: Optional[List[int]] = None  # Size 5/6
    card_id: Optional[int] = None
    deck_level: Optional[int] = None
    from_reserved: Optional[bool] = None
    gold_as: Optional[List[int]] = None  # Size 5
    return_gems: Optional[List[int]] = None  # Size 6
    noble_choice: Optional[int] = None
    usi: Optional[str] = None  # USI move notation


class PlayerSchema(BaseModel):
    index: int
    gems: List[int]  # Size 6
    bonuses: List[int]  # Size 5
    points: int
    reserved_cards: List[int]
    purchased_cards: List[int]
    acquired_nobles: List[int] = Field(default_factory=list)


class BoardSchema(BaseModel):
    bank: List[int]  # Size 6
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
