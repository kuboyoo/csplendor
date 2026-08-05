"""Phase 0: public, rule, packed-state, hash, and MCTS safety contracts.

These tests deliberately describe the current public behaviour, including the
full-information board hash contract used for game-position identity.
"""

import numpy as np
import pytest

import csplendor
from csplendor import (
    MCTS,
    Action,
    ActionEncoderCpp,
    ActionEncoderV2,
    ActionEncoderV3,
    Game,
    MCTSConfig,
    PlayerState,
    StateEncoder,
)
from tests.support import reachable_state_corpus

EXPECTED_PUBLIC_EXPORTS = {
    "GemType", "ActionType", "Card", "Noble", "Action", "PlayerState",
    "Board", "Game", "get_card", "get_noble", "get_all_cards",
    "get_all_nobles", "StateFeaturizer", "ActionEncoder", "ActionEncoderCpp",
    "ActionEncoderV2", "ActionEncoderV3", "StateEncoder", "MCTSConfig",
    "MCTSNode", "MCTS", "LeafRequest", "InferenceResult",
    "ParallelTreeBackend", "ParallelSearchMode", "ParallelSearchStopReason",
    "ParallelCancellationToken", "ParallelSearchOptions",
    "ParallelSearchLedger", "ParallelSearchResult", "RootParallelSearchResult",
    "mcts_search_parallel_native", "mcts_search_root_parallel_native",
    "solve_reveal_verified_mate_cpp", "solve_visible_only_winner_cpp",
}


# Fixed-seed action-code corpus.  The list order is observable through
# legal_action_code_at, apply_legal_action_index, random modulo selection, and
# the encoders' first-match behaviour.
GOLDEN_ACTION_CODES_SEED_42 = [
    168, 552, 2088, 648, 2184, 2568, 672, 2208, 2592, 2688,
    17, 65, 257, 1025, 4097, 130, 234, 194, 26, 370, 530, 506,
    418, 578, 666, 674, 626, 11, 19, 27,
]
GOLDEN_AFTER_FIRST_ACTION_SEED_42 = [
    168, 552, 2088, 648, 2184, 2568, 672, 2208, 2592, 2688,
    1025, 4097, 130, 234, 194, 26, 370, 530, 506, 418, 578, 666,
    674, 626, 11, 19, 27,
]


def _pack_12bit(values):
    return sum(int(value) << (12 * index) for index, value in enumerate(values))


def test_public_python_surface_and_encoder_result_ownership_contract():
    assert set(csplendor.__all__) == EXPECTED_PUBLIC_EXPORTS

    game = Game(seed=42)
    features = StateEncoder.encode(game)
    assert isinstance(features, list)
    assert len(features) == 196

    for encoder in (ActionEncoderCpp, ActionEncoderV2, ActionEncoderV3):
        first = encoder.get_action_mask(game)
        snapshot = np.array(first, copy=True)
        assert first.shape == (encoder.BASE_ACTION_COUNT if encoder is ActionEncoderCpp else encoder.ACTION_SIZE,)
        assert first.dtype == np.uint8
        # A later binding return must not overwrite a previously returned mask.
        encoder.get_action_mask(Game(seed=0))
        np.testing.assert_array_equal(first, snapshot)


def test_legal_action_codes_are_golden_and_indexed_in_the_same_order():
    game = Game(seed=42)
    assert list(game.legal_action_codes) == GOLDEN_ACTION_CODES_SEED_42
    assert game.legal_action_count == len(GOLDEN_ACTION_CODES_SEED_42)
    assert [game.legal_action_code_at(i) for i in range(game.legal_action_count)] == GOLDEN_ACTION_CODES_SEED_42
    assert game.legal_action_code_at(game.legal_action_count) == 0

    assert game.apply_legal_action_index(0, False)
    assert list(game.legal_action_codes) == GOLDEN_AFTER_FIRST_ACTION_SEED_42


def test_generated_actions_are_unique_pack_roundtrippable_and_applicable():
    for game in reachable_state_corpus():
        actions = game.legal_actions
        codes = [int(action.pack()) for action in actions]
        assert codes == list(game.legal_action_codes)
        assert len(codes) == len(set(codes))
        assert game.legal_action_count == len(actions)

        for index, (action, code) in enumerate(zip(actions, codes)):
            assert game.is_legal(action)
            assert Action.unpack(code).pack() == code
            by_action = game.clone_light()
            by_index = game.clone_light()
            assert by_action.apply(action, False)
            assert by_index.apply_legal_action_index(index, False)
            assert by_action.board_hash() == by_index.board_hash()


def test_player_packed_fields_follow_normal_setters_and_board_set_player():
    player = PlayerState()
    player.gems = [1, 2, 3, 4, 5, 6]
    player.bonuses = [2, 3, 4, 5, 6]
    assert player.packed_gems == _pack_12bit([1, 2, 3, 4, 5])
    assert player.packed_bonuses == _pack_12bit([2, 3, 4, 5, 6])

    game = Game(seed=5)
    game.board.set_player(0, player)
    stored = game.board.get_player(0)
    assert stored.packed_gems == _pack_12bit(stored.gems[:5])
    assert stored.packed_bonuses == _pack_12bit(stored.bonuses)


def test_game_gem_transitions_only_update_the_gem_pack():
    game = Game(seed=42)
    before = game.board.get_player(0)
    action = next(action for action in game.legal_actions if action.type == csplendor.ActionType.TAKE_DIFFERENT)
    assert game.apply(action, False)

    after = game.board.get_player(0)
    assert after.packed_gems == _pack_12bit(after.gems[:5])
    assert after.packed_bonuses == before.packed_bonuses


def test_hash_cache_observes_currently_hashed_mutations_and_clone_isolation():
    game = Game(seed=11)
    first = game.board_hash()
    assert game.board_hash() == first  # hot-cache path

    game.board.bank = [3, 4, 4, 4, 4, 5]
    assert game.board_hash() != first

    clone = game.clone_light()
    clone.board.visible = [[-1, -1, -1, -1]] * 3
    assert clone.board_hash() != game.board_hash()


def test_hash_distinguishes_player_points():
    game = Game(seed=11)
    before = game.board_hash()
    player = game.board.get_player(0)
    player.points = 1
    game.board.set_player(0, player)
    assert game.board_hash() != before


def test_hash_distinguishes_deck_order_and_final_round():
    game = Game(seed=11)
    before = game.board_hash()
    decks = game.board.decks
    decks[0][0], decks[0][1] = decks[0][1], decks[0][0]
    game.board.decks = decks
    game.board.final_round = True
    assert game.board_hash() != before


def test_hash_distinguishes_tiebreak_and_hidden_information_state():
    game = Game(seed=11)
    before = game.board_hash()

    player = game.board.get_player(0)
    player.purchased_count = 1
    game.board.set_player(0, player)
    assert game.board_hash() != before

    before = game.board_hash()
    player = game.board.get_player(0)
    hidden = list(player.reserved_is_hidden)
    hidden[0] = True
    player.reserved_is_hidden = hidden
    game.board.set_player(0, player)
    assert game.board_hash() != before

    before = game.board_hash()
    game.board.winner = 0
    assert game.board_hash() != before


def test_observable_hash_uses_all_public_state_but_not_deck_order():
    game = Game(seed=11)
    observer = 0
    before = game.board.observable_hash(observer)

    player = game.board.get_player(0)
    player.points = 1
    player.purchased_count = 1
    game.board.set_player(0, player)
    assert game.board.observable_hash(observer) != before

    before = game.board.observable_hash(observer)
    game.board.final_round = True
    assert game.board.observable_hash(observer) != before

    before = game.board.observable_hash(observer)
    game.board.winner = 0
    assert game.board.observable_hash(observer) != before

    before = game.board.observable_hash(observer)
    game.board.turn = 1
    assert game.board.observable_hash(observer) != before

    before = game.board.observable_hash(observer)
    reordered = game.board.decks
    reordered[0][0], reordered[0][1] = reordered[0][1], reordered[0][0]
    game.board.decks = reordered
    assert game.board.observable_hash(observer) == before


def test_observable_repetition_hash_ignores_only_the_turn_counter():
    game = Game(seed=11)
    observer = 0
    observable_before = game.board.observable_hash(observer)
    repetition_before = game.board.observable_repetition_hash(observer)

    game.board.turn = 7

    assert game.board.observable_hash(observer) != observable_before
    assert game.board.observable_repetition_hash(observer) == repetition_before

    player = game.board.get_player(0)
    player.points = 1
    player.purchased_count = 1
    game.board.set_player(0, player)

    assert game.board.observable_repetition_hash(observer) != repetition_before


def test_mcts_defaults_to_public_information_determinization():
    assert MCTSConfig().use_determinization is True


def test_mcts_expand_select_backprop_and_virtual_loss_contract():
    config = MCTSConfig()
    config.use_dirichlet_noise = False
    config.fpu = 0.0
    mcts = MCTS(config)
    state_hash = 0xA11CE
    policy = [0.0] * 48
    policy[3], policy[7] = 0.25, 0.75
    valid = [0] * 48
    valid[3] = valid[7] = 1
    mcts.expand_node(state_hash, policy, [0.2, -0.2], valid)

    node = mcts.get_node(state_hash)
    assert node.is_expanded and not node.is_terminal
    assert node.valid_actions[3] == node.valid_actions[7] == 1
    assert mcts.select_action_with_virtual_loss(state_hash, False, None, 0) == 7

    mcts.update_stats(state_hash, 7, 0.5)
    node = mcts.get_node(state_hash)
    assert node.total_visits == 1
    assert node.N[7] == 1
    assert node.Q[7] == pytest.approx(0.5)

    mcts.add_virtual_loss(state_hash, 7)
    assert mcts.get_node(state_hash).virtual_loss[7] == 1
    mcts.clear_virtual_losses()
    assert mcts.get_node(state_hash).virtual_loss[7] == 0


def test_mcts_python_callback_receives_an_isolated_history_copy():
    # mcts_search is intentionally an internal binding today.  This test fixes
    # the observable callback contract without promoting it to csplendor.__all__.
    import csplendor._csplendor as core

    class RecordingFeaturizer:
        def __init__(self):
            self.received = []

        def featurize(self, callback_game):
            self.received.append(callback_game)
            # The root has one retained action.  A successful undo proves the
            # callback copy includes history; it must not affect the root.
            assert callback_game.undo() is True
            return np.zeros(196, dtype=np.float32)

    class PythonEncoder:
        def __init__(self):
            self.encoder = csplendor.ActionEncoder()

        def encode(self, action, game):
            return self.encoder.encode(action, game)

        def decode(self, index, game):
            return self.encoder.decode(index, game)

        def get_action_mask(self, game):
            return self.encoder.get_action_mask(game)

    root = Game(seed=42)
    assert root.apply(root.legal_actions[0], True)
    root_hash = root.board_hash()
    config = MCTSConfig()
    config.use_dirichlet_noise = False
    config.use_determinization = False
    mcts = MCTS(config)
    featurizer = RecordingFeaturizer()

    def inference(requests):
        return [
            {"policy": np.ones(48, dtype=np.float32), "value": np.array([0.1, -0.1], dtype=np.float32)}
            for _ in requests
        ]

    probs = core.mcts_search(mcts, featurizer, PythonEncoder(), root, 1, inference, 1.0)
    assert len(probs) == 48
    assert len(featurizer.received) == 1
    assert root.board_hash() == root_hash
