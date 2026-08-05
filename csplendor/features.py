import numpy as np

from . import _csplendor as core


class StateFeaturizer:
    """
    Converts Splendor game state into NumPy features for ML.
    """

    CARD_FEATURE_SIZE = core.StateEncoder.card_feature_size()
    NOBLE_FEATURE_SIZE = core.StateEncoder.noble_feature_size()

    def __init__(self):
        pass

    def featurize(self, game: core.Game, observer: int = -1) -> np.ndarray:
        return np.asarray(
            core.StateEncoder.encode(game, observer), dtype=np.float32
        )

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
