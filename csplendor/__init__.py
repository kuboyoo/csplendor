from ._csplendor import (
    GemType, ActionType, Card, Noble, Action,
    PlayerState, Board, Game,
    get_card, get_noble, get_all_cards, get_all_nobles,
    MCTSConfig, MCTSNode, MCTS,
    LeafRequest, InferenceResult,
    StateEncoder,
    ActionEncoderCpp,
    ActionEncoderV2,
    ActionEncoderV3,
)
from .features import StateFeaturizer
from .action_space import ActionEncoder

__version__ = "0.1.0"
__all__ = [
    "GemType", "ActionType", "Card", "Noble", "Action",
    "PlayerState", "Board", "Game",
    "get_card", "get_noble", "get_all_cards", "get_all_nobles",
    "StateFeaturizer", "ActionEncoder", "ActionEncoderCpp", "ActionEncoderV2", "ActionEncoderV3", "StateEncoder",
    "MCTSConfig", "MCTSNode", "MCTS",
    "LeafRequest", "InferenceResult",
]

GEM_NAMES = ["Emerald", "Sapphire", "Ruby", "Diamond", "Onyx", "Gold"]
GEM_SYMBOLS = ["E", "S", "R", "D", "O", "G"]
