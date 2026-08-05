"""Canonical SPN game builder/serializer surface.

The mature SPN implementation remains in the compatibility module during the
cross-repository USI migration. New application code imports this focused
surface, so the implementation can move without changing its consumers.
"""

from .usi_kifu import board_to_spn, game_to_spn, position_to_game, spn_to_game

__all__ = ["board_to_spn", "game_to_spn", "position_to_game", "spn_to_game"]
