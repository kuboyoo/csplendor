import numpy as np
from . import _csplendor as core
import itertools

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
    
    BASE_ACTION_COUNT = 48  # Extended from 45 to 48 for VISIT_NOBLE
    
    def __init__(self):
        # 1. TAKE_DIFFERENT (10)
        self.take_diff_combinations = list(itertools.combinations(range(5), 3))
        
        # 2. TAKE_SAME (5)
        self.take_same_indices = list(range(5))
        
    def encode(self, action: core.Action, game: core.Game) -> int:
        """
        Maps a core.Action to an integer index [0, 47].
        """
        board = game.board
        
        if action.type == core.ActionType.TAKE_DIFFERENT:
            # Find which colors were taken
            colors = []
            for i in range(5):
                if action.take[i] > 0:
                    colors.append(i)
            if len(colors) == 3:
                colors = tuple(sorted(colors))
                return self.take_diff_combinations.index(colors)
            else:
                # If less than 3 were taken (only possible if bank < 3 colors)
                for idx, comb in enumerate(self.take_diff_combinations):
                    if all(c in comb for c in colors):
                        return idx
                return 0
                
        elif action.type == core.ActionType.TAKE_SAME:
            for i in range(5):
                if action.take[i] == 2:
                    return 10 + i
            return 10
            
        elif action.type == core.ActionType.RESERVE_VISIBLE:
            for l in range(3):
                for s in range(4):
                    if board.visible[l][s] == action.card_id:
                        return 15 + l * 4 + s
            return 15
            
        elif action.type == core.ActionType.RESERVE_DECK:
            return 27 + action.deck_level
            
        elif action.type == core.ActionType.PURCHASE:
            if action.from_reserved:
                # Find index in player's reserved cards
                p = board.players[board.current_player]
                for i in range(3):
                    if p.reserved[i] == action.card_id:
                        return 42 + i
                return 42
            else:
                # Visible on board
                for l in range(3):
                    for s in range(4):
                        if board.visible[l][s] == action.card_id:
                            return 30 + l * 4 + s
                return 30
        
        elif action.type == core.ActionType.VISIT_NOBLE:
            # noble_choice is the noble ID (0-9 typically), but we only have 3 nobles on board
            # Map to the position (0-2) in the current nobles list
            noble_id = action.noble_choice
            for i, nid in enumerate(board.nobles):
                if nid == noble_id:
                    return 45 + i
            # Fallback to first noble slot if not found
            return 45
                
        return -1

    def decode(self, index: int, game: core.Game) -> core.Action:
        """
        Maps an integer index [0, 47] back to a core.Action.
        Returns None if the action is illegal in the current state.

        For PURCHASE actions with multiple payment options, uses a heuristic
        to select the best payment method (minimize gold usage).
        """
        legal_actions = game.legal_actions
        matching_actions = []

        for action in legal_actions:
            if self.encode(action, game) == index:
                matching_actions.append(action)

        if not matching_actions:
            return None

        if len(matching_actions) == 1:
            return matching_actions[0]

        # Multiple actions match the same index (different payment methods)
        # Use heuristic: minimize gold usage, then minimize total gems returned
        return self._select_best_payment(matching_actions)

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
        mask = np.zeros(self.BASE_ACTION_COUNT, dtype=bool)
        legal_actions = game.legal_actions
        for action in legal_actions:
            idx = self.encode(action, game)
            if idx != -1:
                mask[idx] = True
        return mask
