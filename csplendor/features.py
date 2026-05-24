import numpy as np
from . import _csplendor as core

class StateFeaturizer:
    """
    Converts Splendor game state into NumPy features for ML.
    """
    
    CARD_FEATURE_SIZE = 8 # points, emerald, sapphire, ruby, diamond, onyx, bonus_type (1-hot), level (normalized)
    NOBLE_FEATURE_SIZE = 6 # points, emerald, sapphire, ruby, diamond, onyx requirements

    def __init__(self):
        pass

    def featurize(self, game: core.Game, observer: int = -1) -> np.ndarray:
        board = game.board
        players = board.players
        
        # 1. Bank gems (6)
        bank_gems = np.array(board.bank, dtype=np.float32) / 7.0 # Normalize by max possible if needed, or leave as is
        
        # 2. Players (2 players)
        player_features = []
        for i in range(2):
            p = players[i]
            # Gems (6)
            gems = np.array(p.gems, dtype=np.float32) / 10.0
            # Bonuses (5)
            bonuses = np.array(p.bonuses, dtype=np.float32) / 10.0
            # Points (1)
            points = np.array([p.points], dtype=np.float32) / 15.0
            
            # Reserved (3 cards * 8 features)
            reserved = []
            reserved_ids = p.reserved
            # If observer is specified, hide cards that are hidden from the observer
            for r_idx, r_id in enumerate(reserved_ids):
                # Card is hidden if it's the opponent's card AND it's marked as hidden
                is_hidden = (observer != -1 and i != observer and p.reserved_is_hidden[r_idx])
                
                if is_hidden:
                    # For hidden cards, we can only see the card level
                    # We create a generic card feature with only the level
                    card = core.get_card(r_id)
                    feat = np.zeros(self.CARD_FEATURE_SIZE, dtype=np.float32)
                    feat[7] = card.level / 3.0
                    reserved.append(feat)
                else:
                    reserved.append(self._featurize_card(r_id))
            
            # Pad with zeros if less than 3 cards
            while len(reserved) < 3:
                reserved.append(np.zeros(self.CARD_FEATURE_SIZE, dtype=np.float32))
                
            reserved = np.concatenate(reserved)
            
            player_features.append(np.concatenate([gems, bonuses, points, reserved]))
            
        # 3. Board visible cards (12 cards * 8 features)
        visible_cards = []
        for l in range(3):
            for s in range(4):
                c_id = board.visible[l][s]
                visible_cards.append(self._featurize_card(c_id))
        visible_cards = np.concatenate(visible_cards)
        
        # 4. Deck counts (3)
        deck_counts = np.array([len(board.decks[i]) for i in range(3)], dtype=np.float32) / 40.0
        
        # 5. Nobles (3 * 6 features)
        nobles = []
        noble_ids = board.nobles
        for i in range(3):
            if i < len(noble_ids):
                nobles.append(self._featurize_noble(noble_ids[i]))
            else:
                nobles.append(np.zeros(self.NOBLE_FEATURE_SIZE, dtype=np.float32))
        nobles = np.concatenate(nobles)
        
        # 6. Current player (1)
        current_p = np.array([board.current_player], dtype=np.float32)
        
        # Concatenate everything
        return np.concatenate([
            bank_gems,
            player_features[0],
            player_features[1],
            visible_cards,
            deck_counts,
            nobles,
            current_p
        ])

    def _featurize_card(self, card_id: int) -> np.ndarray:
        if card_id == -1:
            return np.zeros(self.CARD_FEATURE_SIZE, dtype=np.float32)
        
        card = core.get_card(card_id)
        # points, cost[5], bonus (1-hot or index), level
        features = np.zeros(self.CARD_FEATURE_SIZE, dtype=np.float32)
        features[0] = card.points / 5.0
        for i in range(5):
            features[1+i] = card.cost[i] / 7.0
        
        # Bonus type as normalized value or we could do 1-hot. Let's do index/5
        features[6] = int(card.bonus) / 5.0 
        features[7] = card.level / 3.0
        return features

    def _featurize_noble(self, noble_id: int) -> np.ndarray:
        if noble_id == -1:
            return np.zeros(self.NOBLE_FEATURE_SIZE, dtype=np.float32)
        
        noble = core.get_noble(noble_id)
        features = np.zeros(self.NOBLE_FEATURE_SIZE, dtype=np.float32)
        features[0] = noble.points / 3.0
        for i in range(5):
            features[1+i] = noble.requirement[i] / 4.0
        return features
