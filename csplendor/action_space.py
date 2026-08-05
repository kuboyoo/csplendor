import itertools

import numpy as np

from . import _csplendor as core

_V1_OFFSETS = {
    section["name"]: section["offset"]
    for section in core.ActionEncoderCpp.schema_sections()
}


class ActionEncoder:
    """
    Encodes/Decodes Splendor actions to/from integer indices.
    This encoder handles 48 actions:
    - TAKE_DIFFERENT: 10 (indices 0-9)
    - TAKE_SAME: 5 (indices 10-14)
    - RESERVE_VISIBLE: 12 (indices 15-26)
    - RESERVE_DECK: 3 (indices 27-29)
    - PURCHASE_VISIBLE: 12 (indices 30-41)
    - PURCHASE_RESERVED: 3 (indices 42-44)
    - VISIT_NOBLE: 3 (indices 45-47)

    Returns and Nobles are handled by providing the 'best' version
    of the action (e.g. heuristic return) if multiple exist for a base index.
    """

    BASE_ACTION_COUNT = core.ActionEncoderCpp.BASE_ACTION_COUNT

    def __init__(self):
        # 1. TAKE_DIFFERENT (10)
        self.take_diff_combinations = list(itertools.combinations(range(5), 3))

        # 2. TAKE_SAME (5)
        self.take_same_indices = list(range(5))

    def encode(self, action: core.Action, game: core.Game) -> int:
        """
        Maps a core.Action to an integer index [0, 47].
        """
        if action.type == core.ActionType.TAKE_DIFFERENT:
            colors = [index for index, count in enumerate(action.take) if count > 0]
            if len(colors) > 3:
                return _V1_OFFSETS["take_different"]

        encoded = core.ActionEncoderCpp.encode(action, game)
        if encoded >= 0:
            return encoded

        # Preserve the historical Python-only fallback contract for malformed
        # or state-mismatched actions. Legal actions always use the native path.
        if action.type == core.ActionType.TAKE_SAME:
            return _V1_OFFSETS["take_same"]
        if action.type == core.ActionType.RESERVE_VISIBLE:
            return _V1_OFFSETS["reserve_visible"]
        if action.type == core.ActionType.RESERVE_DECK:
            return _V1_OFFSETS["reserve_deck"] + action.deck_level
        if action.type == core.ActionType.PURCHASE:
            section = (
                "purchase_reserved" if action.from_reserved else "purchase_visible"
            )
            return _V1_OFFSETS[section]
        if action.type == core.ActionType.VISIT_NOBLE:
            return _V1_OFFSETS["visit_noble"]
        return encoded

    def decode(self, index: int, game: core.Game) -> core.Action:
        """
        Maps an integer index [0, 47] back to a core.Action.
        Returns None if the action is illegal in the current state.

        For PURCHASE actions with multiple payment options, uses a heuristic
        to select the best payment method (minimize gold usage).
        """
        if index < 0 or index >= self.BASE_ACTION_COUNT:
            return None
        mask = core.ActionEncoderCpp.get_action_mask(game)
        if not mask[index]:
            return None
        return core.ActionEncoderCpp.decode(index, game)

    def _select_best_payment(self, actions: list) -> core.Action:
        """
        Select the best payment option from multiple matching actions.
        Heuristic:
        1. Minimize gold usage (gold is the most valuable resource)
        2. Minimize gems returned
        3. Prefer actions that don't return recently acquired gems
        """
        def score_action(action):
            # Lower score = better
            gold_used = sum(action.gold_as) if action.gold_as else 0
            gems_returned = sum(action.return_gems) if action.return_gems else 0
            return (gold_used, gems_returned)

        return min(actions, key=score_action)

    def get_action_mask(self, game: core.Game) -> np.ndarray:
        """
        Returns a boolean mask of size 48 where True means legal.
        """
        return np.asarray(core.ActionEncoderCpp.get_action_mask(game), dtype=bool)
