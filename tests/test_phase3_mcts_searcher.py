"""Phase 3 contracts for the MCTSSearcher-owned simulation Game."""

import csplendor._csplendor as core
import numpy as np
import pytest

from csplendor import MCTS, ActionEncoder, ActionType, Game, MCTSConfig
from tests.support import game_state_signature as _game_signature


def _make_history(length):
    game = Game(seed=42)
    for _ in range(length):
        action = next(
            (
                candidate
                for candidate in game.legal_actions
                if candidate.type
                in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME)
            ),
            None,
        )
        if action is None:
            # Keep producing harmless token actions without clearing history.
            game.board.bank = [4, 4, 4, 4, 4, 5]
            for player_index in range(2):
                player = game.board.get_player(player_index)
                player.gems = [0, 0, 0, 0, 0, 0]
                game.board.set_player(player_index, player)
            action = next(
                candidate
                for candidate in game.legal_actions
                if candidate.type
                in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME)
            )
        assert game.apply(action, True)
    assert _history_depth(game) == length
    return game


def _undo_trace(game):
    probe = game.clone()
    trace = []
    while probe.undo():
        trace.append(_game_signature(probe))
    return tuple(trace)


def _history_depth(game):
    probe = game.clone()
    depth = 0
    while probe.undo():
        depth += 1
    return depth


def _config(*, determinization=False):
    config = MCTSConfig()
    config.use_determinization = determinization
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    return config


def _inference_result(requests, policy=None, value=None):
    if policy is None:
        policy = np.ones(48, dtype=np.float32)
    if value is None:
        value = np.array([0.1, -0.1], dtype=np.float32)
    return [{"policy": policy, "value": value} for _ in requests]


@pytest.mark.parametrize("history_length", [0, 50, 200])
@pytest.mark.parametrize("determinization", [False, True])
def test_callbacks_keep_history_ownership_and_root_isolation(
    history_length, determinization
):
    root = _make_history(history_length)
    root_signature = _game_signature(root)
    root_trace = _undo_trace(root)
    expected_trace = () if determinization else root_trace
    root_callback_state = (
        tuple(map(int, root.board.bank)),
        bool(root.simple_payment_mode),
        int(root.winner),
    )
    expected_hash = (
        root.board.observable_hash(root.current_player)
        if determinization
        else root.board_hash()
    )
    features = np.linspace(-1.0, 1.0, 196, dtype=np.float32)
    mask = (np.arange(48) % 2).astype(np.uint8)

    class RecordingFeaturizer:
        def __init__(self):
            self.received = []
            self.undo_traces = []
            self.observed_states = []
            self.undo_results = []

        def featurize(self, callback_game):
            self.received.append(callback_game)
            self.undo_traces.append(_undo_trace(callback_game))
            self.observed_states.append(
                (
                    tuple(map(int, callback_game.board.bank)),
                    bool(callback_game.simple_payment_mode),
                    int(callback_game.winner),
                )
            )
            self.undo_results.append(callback_game.undo())
            callback_game.board.bank = [0, 0, 0, 0, 0, 0]
            callback_game.simple_payment_mode = (
                not callback_game.simple_payment_mode
            )
            return features

    class RecordingEncoder:
        def __init__(self):
            self.encoder = ActionEncoder()
            self.received = []
            self.undo_traces = []
            self.observed_states = []

        def encode(self, action, callback_game):
            return self.encoder.encode(action, callback_game)

        def decode(self, index, callback_game):
            return self.encoder.decode(index, callback_game)

        def get_action_mask(self, callback_game):
            self.received.append(callback_game)
            self.undo_traces.append(_undo_trace(callback_game))
            self.observed_states.append(
                (
                    tuple(map(int, callback_game.board.bank)),
                    bool(callback_game.simple_payment_mode),
                    int(callback_game.winner),
                )
            )
            callback_game.board.winner = 0
            return mask

    featurizer = RecordingFeaturizer()
    encoder = RecordingEncoder()
    requests_seen = []

    def inference(requests):
        requests_seen.extend(
            {
                "hash": int(request["hash"]),
                "features": np.array(request["features"], copy=True),
                "valid_actions": np.array(
                    request["valid_actions"], copy=True
                ),
                "path_index": int(request["path_index"]),
            }
            for request in requests
        )
        return _inference_result(requests)

    probabilities = core.mcts_search(
        MCTS(_config(determinization=determinization)),
        featurizer,
        encoder,
        root,
        1,
        inference,
        1.0,
    )

    assert len(probabilities) == 48
    assert len(featurizer.received) == len(encoder.received) == 1
    assert featurizer.received[0] is not encoder.received[0]
    assert featurizer.undo_traces == encoder.undo_traces == [expected_trace]
    assert featurizer.observed_states == encoder.observed_states == [
        root_callback_state
    ]
    expected_undo = not determinization and history_length > 0
    assert featurizer.undo_results == [expected_undo]
    assert _history_depth(featurizer.received[0]) == max(
        len(expected_trace) - int(expected_undo), 0
    )
    assert _history_depth(encoder.received[0]) == len(expected_trace)
    assert list(featurizer.received[0].board.bank) == [0] * 6
    assert featurizer.received[0].winner == root.winner
    assert encoder.received[0].winner == 0
    assert tuple(map(int, encoder.received[0].board.bank)) == tuple(
        map(int, root.board.bank)
    )
    assert (
        encoder.received[0].simple_payment_mode
        == root.simple_payment_mode
    )

    assert len(requests_seen) == 1
    request = requests_seen[0]
    assert request["hash"] == expected_hash
    assert request["path_index"] == 0
    np.testing.assert_array_equal(request["features"], features)
    np.testing.assert_array_equal(request["valid_actions"], mask)

    assert _game_signature(root) == root_signature
    assert _undo_trace(root) == root_trace


@pytest.mark.parametrize("callback_name", ["featurize", "get_action_mask", "decode"])
@pytest.mark.parametrize("determinization", [False, True])
def test_callback_exception_preserves_root_and_propagates(
    determinization, callback_name
):
    root = _make_history(50)
    root_signature = _game_signature(root)
    root_trace = _undo_trace(root)
    received = {
        "featurize": [],
        "get_action_mask": [],
        "decode": [],
    }
    history_depths = {name: [] for name in received}

    def record_and_raise(name, callback_game):
        received[name].append(callback_game)
        history_depths[name].append(_history_depth(callback_game))
        callback_game.board.bank = [0, 0, 0, 0, 0, 0]
        callback_game.board.winner = 1
        raise RuntimeError(f"phase3 {name} failure")

    class RaisingFeaturizer:
        def featurize(self, callback_game):
            if callback_name == "featurize":
                record_and_raise("featurize", callback_game)
            return np.zeros(196, dtype=np.float32)

    class RaisingEncoder:
        def __init__(self):
            self.encoder = ActionEncoder()

        def encode(self, action, game):
            return self.encoder.encode(action, game)

        def decode(self, index, game):
            if callback_name == "decode":
                record_and_raise("decode", game)
            return self.encoder.decode(index, game)

        def get_action_mask(self, game):
            if callback_name == "get_action_mask":
                record_and_raise("get_action_mask", game)
            return self.encoder.get_action_mask(game)

    with pytest.raises(RuntimeError, match=f"phase3 {callback_name} failure"):
        core.mcts_search(
            MCTS(_config(determinization=determinization)),
            RaisingFeaturizer(),
            RaisingEncoder(),
            root,
            2 if callback_name == "decode" else 1,
            lambda requests: _inference_result(
                requests, np.arange(1, 49, dtype=np.float32)
            ),
            1.0,
        )

    assert history_depths[callback_name] == [0 if determinization else 50]
    assert len(received[callback_name]) == 1
    held_game = received[callback_name][0]
    assert list(held_game.board.bank) == [0] * 6
    assert held_game.winner == 1
    assert _history_depth(held_game) == (0 if determinization else 50)
    assert _game_signature(root) == root_signature
    assert _undo_trace(root) == root_trace


def test_non_determinized_search_keeps_request_action_and_value_digest():
    root = Game(seed=42)
    root_signature = _game_signature(root)
    features = np.zeros(196, dtype=np.float32)
    policy = np.arange(1, 49, dtype=np.float32)
    value = np.array([0.25, -0.25], dtype=np.float32)

    class Featurizer:
        def featurize(self, game):
            return features

    class RecordingEncoder:
        def __init__(self):
            self.encoder = ActionEncoder()
            self.decoded_indices = []

        def encode(self, action, game):
            return self.encoder.encode(action, game)

        def decode(self, index, game):
            self.decoded_indices.append(index)
            return self.encoder.decode(index, game)

        def get_action_mask(self, game):
            return self.encoder.get_action_mask(game)

    encoder = RecordingEncoder()
    requests_seen = []

    def inference(requests):
        requests_seen.extend(
            (
                int(request["hash"]),
                np.array(request["features"], copy=True),
                np.array(request["valid_actions"], copy=True),
                int(request["path_index"]),
            )
            for request in requests
        )
        return _inference_result(requests, policy, value)

    mcts = MCTS(_config())
    probabilities = np.asarray(
        core.mcts_search(
            mcts, Featurizer(), encoder, root, 4, inference, 1.0
        ),
        dtype=np.float32,
    )

    expected_games = []
    for indices in ((), (29,), (29, 29), (29, 28)):
        game = root.clone_light()
        for index in indices:
            assert game.apply_trusted(ActionEncoder().decode(index, game), False)
        expected_games.append(game)

    assert encoder.decoded_indices == [29, 29, 29, 29, 28]
    assert len(requests_seen) == len(expected_games) == 4
    for request, expected_game in zip(requests_seen, expected_games):
        request_hash, request_features, request_mask, path_index = request
        assert request_hash == expected_game.board_hash()
        assert path_index == 0
        np.testing.assert_array_equal(request_features, features)
        np.testing.assert_array_equal(
            request_mask, ActionEncoder().get_action_mask(expected_game)
        )

    expected_probabilities = np.zeros(48, dtype=np.float32)
    expected_probabilities[29] = 1.0
    np.testing.assert_array_equal(probabilities, expected_probabilities)
    assert mcts.tree_size() == 4
    root_node = mcts.get_node(root.board_hash())
    assert root_node.total_visits == 3
    assert root_node.N[29] == 3
    assert root_node.Q[29] == pytest.approx(0.25)
    assert _game_signature(root) == root_signature


def test_terminal_search_does_not_invoke_callbacks_or_mutate_root():
    root = _make_history(50)
    root.board.winner = 0
    root_signature = _game_signature(root)
    root_trace = _undo_trace(root)

    class UnusedCallback:
        def __getattr__(self, name):
            raise AssertionError(f"{name} must not be called")

    mcts = MCTS(_config())
    probabilities = core.mcts_search(
        mcts,
        UnusedCallback(),
        UnusedCallback(),
        root,
        2,
        lambda requests: pytest.fail("inference must not be called"),
        1.0,
    )

    np.testing.assert_array_equal(probabilities, np.zeros(48))
    node = mcts.get_node(root.board_hash())
    assert node.is_terminal
    np.testing.assert_array_equal(node.value, np.array([1.0, -1.0]))
    assert _game_signature(root) == root_signature
    assert _undo_trace(root) == root_trace
