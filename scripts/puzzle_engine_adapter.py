"""Playout players used only by mate-puzzle generation."""

from __future__ import annotations

import random
from typing import Protocol

import csplendor as cs


class PuzzlePlayer(Protocol):
    def select_action(self, game: cs.Game) -> cs.Action: ...


class RandomPurchasePlayer:
    """Pick a uniformly random purchase when one is legal, else any legal action."""

    def __init__(self, rng: random.Random):
        self.rng = rng

    def select_action(self, game: cs.Game) -> cs.Action:
        actions = list(game.legal_actions)
        if not actions:
            raise ValueError("cannot select an action without legal actions")
        purchases = [
            action
            for action in actions
            if int(action.type) == int(cs.ActionType.PURCHASE)
        ]
        return self.rng.choice(purchases or actions)


__all__ = ["PuzzlePlayer", "RandomPurchasePlayer"]
