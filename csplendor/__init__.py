from importlib.metadata import PackageNotFoundError
from importlib.metadata import version as _distribution_version

from ._csplendor import (
    MCTS,
    Action,
    ActionEncoderCpp,
    ActionEncoderV2,
    ActionEncoderV3,
    ActionType,
    Board,
    Card,
    Game,
    GemType,
    InferenceResult,
    LeafRequest,
    MCTSConfig,
    MCTSNode,
    MateSearchCancellationToken,
    Noble,
    ParallelCancellationToken,
    ParallelSearchLedger,
    ParallelSearchMode,
    ParallelSearchOptions,
    ParallelSearchResult,
    ParallelSearchStopReason,
    ParallelTreeBackend,
    PlayerState,
    RootParallelSearchResult,
    StateEncoder,
    get_all_cards,
    get_all_nobles,
    get_card,
    get_noble,
    mcts_search_parallel_native,
    mcts_search_root_parallel_native,
    solve_reveal_verified_frontier_cpp,
    solve_reveal_verified_mate_cpp,
    solve_visible_only_winner_cpp,
)
from .action_space import ActionEncoder
from .features import StateFeaturizer
from .gem_colors import (
    GEM_NAMES as GEM_NAMES,
)
from .gem_colors import (
    GEM_SYMBOLS as GEM_SYMBOLS,
)
from .gem_colors import (
    GEM_USI_SYMBOLS as GEM_USI_SYMBOLS,
)
from .mate_frontier import (
    decode_mate_frontier_state,
    encode_mate_frontier_state,
    expand_mate_frontier,
    load_mate_frontier_game,
)
from .mate_depth import (
    MATE_ANYTIME_SEARCH_FORMAT,
    MATE_DEPTH_SEARCH_FORMAT,
    search_reveal_verified_mate_anytime,
    search_reveal_verified_mate_depths,
)
from .mate_session import MATE_SEARCH_SESSION_FORMAT, MateSearchSession

try:
    __version__ = _distribution_version("csplendor")
except PackageNotFoundError:
    # A direct import from an unpacked source tree has no distribution
    # metadata. Release and installed builds always take the canonical value
    # from pyproject.toml through their installed metadata.
    __version__ = "0+unknown"
__all__ = [
    "GemType", "ActionType", "Card", "Noble", "Action",
    "PlayerState", "Board", "Game",
    "get_card", "get_noble", "get_all_cards", "get_all_nobles",
    "StateFeaturizer", "ActionEncoder", "ActionEncoderCpp", "ActionEncoderV2", "ActionEncoderV3", "StateEncoder",
    "MCTSConfig", "MCTSNode", "MCTS",
    "ParallelTreeBackend", "ParallelSearchMode", "ParallelSearchStopReason",
    "ParallelCancellationToken", "ParallelSearchOptions",
    "ParallelSearchLedger", "ParallelSearchResult",
    "RootParallelSearchResult", "mcts_search_parallel_native",
    "mcts_search_root_parallel_native",
    "LeafRequest", "InferenceResult",
    "solve_reveal_verified_frontier_cpp",
    "solve_reveal_verified_mate_cpp",
    "solve_visible_only_winner_cpp",
    "decode_mate_frontier_state", "encode_mate_frontier_state",
    "expand_mate_frontier", "load_mate_frontier_game",
    "MATE_DEPTH_SEARCH_FORMAT", "search_reveal_verified_mate_depths",
    "MateSearchCancellationToken",
    "MATE_SEARCH_SESSION_FORMAT", "MateSearchSession",
    "MATE_ANYTIME_SEARCH_FORMAT", "search_reveal_verified_mate_anytime",
]
