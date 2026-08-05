"""Conversion from native engine values to stable web response schemas."""

from typing import List

from .. import Action, Game
from .. import ActionType as CoreActionType
from .schemas import (
    ActionSchema,
    ActionType,
    BoardSchema,
    GameStateSchema,
    PlayerSchema,
)
from .usi_serializer import action_to_usi


def core_to_schema_action(game: Game, action: Action) -> ActionSchema:
    return ActionSchema(
        type=ActionType(int(action.type)),
        take=(
            list(action.take)
            if action.type
            in [CoreActionType.TAKE_DIFFERENT, CoreActionType.TAKE_SAME]
            else None
        ),
        card_id=action.card_id if action.card_id != -1 else None,
        deck_level=action.deck_level if action.deck_level != -1 else None,
        from_reserved=(
            action.from_reserved if action.type == CoreActionType.PURCHASE else None
        ),
        gold_as=(
            list(action.gold_as) if action.type == CoreActionType.PURCHASE else None
        ),
        return_gems=list(action.return_gems) if any(action.return_gems) else None,
        noble_choice=action.noble_choice if action.noble_choice != -1 else None,
        usi=action_to_usi(action, game=game),
    )


def get_game_state(game: Game) -> GameStateSchema:
    board = game.board
    board_schema = BoardSchema(
        bank=list(board.bank),
        visible_cards=[list(row) for row in board.visible],
        deck_counts=[len(deck) for deck in board.decks],
        nobles=list(board.nobles),
        current_player=board.current_player,
        turn=board.turn,
        waiting_noble=board.waiting_noble,
        game_over=game.is_game_over(),
        winner=board.winner,
    )

    players_schema: List[PlayerSchema] = []
    for index in range(2):
        player = board.players[index]
        players_schema.append(
            PlayerSchema(
                index=index,
                gems=list(player.gems),
                bonuses=list(player.bonuses),
                points=player.points,
                reserved_cards=[
                    card_id for card_id in player.reserved if card_id != -1
                ],
                purchased_cards=list(player.purchased_cards),
                acquired_nobles=list(player.acquired_nobles),
            )
        )

    return GameStateSchema(
        board=board_schema,
        players=players_schema,
        legal_actions=[
            core_to_schema_action(game, action) for action in game.legal_actions
        ],
    )
