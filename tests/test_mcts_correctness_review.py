"""Final-review regressions for the two native MCTS orchestration paths."""

from __future__ import annotations

import csplendor._csplendor as core
import numpy as np
import pytest

from csplendor import (
    MCTS,
    ActionEncoderCpp,
    Game,
    InferenceResult,
    LeafRequest,
    MCTSConfig,
    StateEncoder,
)


def _mcts() -> MCTS:
    config = MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    return MCTS(config)


def test_batch_world_zero_encodes_the_selected_leaf_without_replaying_path():
    root = Game(seed=42)
    root_hash = root.board_hash()
    valid = np.asarray(ActionEncoderCpp.get_action_mask(root), dtype=np.uint8)
    action_index = int(np.flatnonzero(valid)[0])
    policy = np.zeros(48, dtype=np.float32)
    policy[action_index] = 1.0

    mcts = _mcts()
    mcts.expand_node(root_hash, policy, [0.0, 0.0], valid)
    request = mcts.prepare_batch_simulations(root, 0, 1, 1, None)

    expected = root.clone_light()
    action = ActionEncoderCpp.decode(action_index, expected)
    assert expected.apply_trusted(action, False)

    assert request["leaf_paths"] == [[(root_hash, action_index, 0)]]
    assert request["leaf_hashes"] == [expected.board_hash()]
    np.testing.assert_array_equal(
        request["flat_valids"][0],
        ActionEncoderCpp.get_action_mask(expected),
    )
    np.testing.assert_allclose(
        request["flat_boards"][0],
        StateEncoder.encode_canonical(expected, 0, 0),
    )
    assert root.board_hash() == root_hash


def test_full_information_batch_does_not_shuffle_additional_worlds():
    root = Game(seed=42)
    request = _mcts().prepare_batch_simulations(root, 0, 1, 3, None)

    assert request["leaf_world_counts"] == [3]
    for features in request["flat_boards"][1:]:
        np.testing.assert_array_equal(features, request["flat_boards"][0])
    for valid in request["flat_valids"][1:]:
        np.testing.assert_array_equal(valid, request["flat_valids"][0])


def test_python_batch_results_use_requested_valid_masks_not_policy_signs():
    root = Game(seed=42)
    root_hash = root.board_hash()
    mcts = _mcts()
    request = mcts.prepare_batch_simulations(root, 0, 1, 1, None)
    requested_valid = np.asarray(request["flat_valids"][0], dtype=np.uint8)
    assert requested_valid.sum() < requested_valid.size

    # NN logits/probabilities may be positive for illegal actions. Legality is
    # supplied separately by the request and must remain the source of truth.
    policies = [np.ones(48, dtype=np.float32)]
    values = [np.array([0.25, -0.25], dtype=np.float32)]
    mcts.apply_batch_results(request, policies, values)

    node = mcts.get_node(root_hash)
    np.testing.assert_array_equal(node.valid_actions, requested_valid)
    assert np.all(np.asarray(node.prior)[requested_valid == 0] == 0.0)

    follow_up = mcts.prepare_batch_simulations(root, 0, 1, 1, None)
    paths = follow_up["leaf_paths"] + [path for path, _ in follow_up["terminals"]]
    assert paths
    assert all(requested_valid[path[0][1]] for path in paths if path)


def test_mcts_request_and_inference_defaults_are_deterministically_zeroed():
    leaf = LeafRequest()
    assert leaf.hash == 0
    assert leaf.path_index == 0
    np.testing.assert_array_equal(leaf.features, np.zeros(196))
    np.testing.assert_array_equal(leaf.valid_actions, np.zeros(48))

    result = InferenceResult()
    np.testing.assert_array_equal(result.policy, np.zeros(48))
    np.testing.assert_array_equal(result.value, np.zeros(2))

    short = InferenceResult([0.75], [-0.5])
    np.testing.assert_array_equal(
        short.policy, np.array([0.75] + [0.0] * 47)
    )
    np.testing.assert_array_equal(short.value, np.array([-0.5, 0.0]))

    short.policy = [0.25]
    short.value = [0.5]
    np.testing.assert_array_equal(
        short.policy, np.array([0.25] + [0.0] * 47)
    )
    np.testing.assert_array_equal(short.value, np.array([0.5, 0.0]))


def test_python_inference_may_retain_owning_leaf_request_arrays():
    retained = []

    class Featurizer:
        def featurize(self, game):
            return np.arange(196, dtype=np.float32)

    class Encoder:
        def encode(self, action, game):
            return ActionEncoderCpp.encode(action, game)

        def decode(self, index, game):
            return ActionEncoderCpp.decode(index, game)

        def get_action_mask(self, game):
            return ActionEncoderCpp.get_action_mask(game)

    def inference(requests):
        retained.extend(
            (request["features"], request["valid_actions"])
            for request in requests
        )
        return [
            {
                "policy": np.ones(48, dtype=np.float32),
                "value": np.array([0.1, -0.1], dtype=np.float32),
            }
            for _ in requests
        ]

    root = Game(seed=42)
    root_hash = root.board_hash()
    core.mcts_search(
        _mcts(), Featurizer(), Encoder(), root, 1, inference, 1.0
    )

    assert len(retained) == 1
    features, valid = retained[0]
    assert features.flags["OWNDATA"] and features.base is None
    assert valid.flags["OWNDATA"] and valid.base is None
    np.testing.assert_array_equal(features, np.arange(196, dtype=np.float32))
    np.testing.assert_array_equal(valid, ActionEncoderCpp.get_action_mask(root))
    assert root.board_hash() == root_hash


def test_empty_world_batch_is_side_effect_free_and_short_results_fail_early():
    root = Game(seed=42)
    root_hash = root.board_hash()
    valid = np.asarray(ActionEncoderCpp.get_action_mask(root), dtype=np.uint8)
    mcts = _mcts()
    mcts.expand_node(root_hash, np.ones(48), [0.0, 0.0], valid)

    empty = mcts.prepare_batch_simulations(root, 0, 2, 0, None)
    assert empty["total_boards"] == 0
    assert empty["num_leaves"] == 0
    np.testing.assert_array_equal(
        mcts.get_node(root_hash).virtual_loss, np.zeros(48)
    )

    fresh = _mcts()
    request = fresh.prepare_batch_simulations(root, 0, 1, 1, None)
    with pytest.raises(ValueError, match="do not match requested worlds"):
        fresh.apply_batch_results(request, [], [])
    assert fresh.tree_size() == 0


def test_forced_playouts_receive_simulation_index_in_both_search_paths():
    root = Game(seed=42)
    root_hash = root.board_hash()
    policy = np.zeros(48, dtype=np.float32)
    policy[0] = 0.01
    policy[29] = 0.99
    valid = np.zeros(48, dtype=np.uint8)
    valid[0] = valid[29] = 1

    config = MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = True
    config.forced_playouts_k = 1000.0
    batch_mcts = MCTS(config)
    batch_mcts.expand_node(root_hash, policy, [0.0, 0.0], valid)

    batch = batch_mcts.prepare_batch_simulations(root, 0, 2, 1, None)
    assert [path[0][1] for path in batch["leaf_paths"]] == [29, 0]

    class Featurizer:
        def featurize(self, game):
            return np.zeros(196, dtype=np.float32)

    class RecordingEncoder:
        def __init__(self):
            self.decoded = []

        def encode(self, action, game):
            return ActionEncoderCpp.encode(action, game)

        def decode(self, index, game):
            self.decoded.append(index)
            return ActionEncoderCpp.decode(index, game)

        def get_action_mask(self, game):
            return ActionEncoderCpp.get_action_mask(game)

    encoder = RecordingEncoder()

    def inference(requests):
        return [
            {"policy": policy, "value": np.zeros(2, dtype=np.float32)}
            for _ in requests
        ]

    core.mcts_search(
        MCTS(config), Featurizer(), encoder, root, 2, inference, 1.0
    )
    assert encoder.decoded == [0]


@pytest.mark.parametrize("determinization", [False, True])
def test_inference_closure_cannot_retarget_the_in_progress_root_search(
    determinization,
):
    root = Game(seed=42)
    config = MCTSConfig()
    config.use_determinization = determinization
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    root_hash = (
        root.board.observable_hash(root.current_player)
        if determinization
        else root.board_hash()
    )
    mcts = MCTS(config)
    inference_calls = 0

    class Featurizer:
        def featurize(self, game):
            return np.zeros(196, dtype=np.float32)

    class Encoder:
        def encode(self, action, game):
            return ActionEncoderCpp.encode(action, game)

        def decode(self, index, game):
            return ActionEncoderCpp.decode(index, game)

        def get_action_mask(self, game):
            return ActionEncoderCpp.get_action_mask(game)

    def inference(requests):
        nonlocal inference_calls
        inference_calls += 1
        if inference_calls == 1:
            root.board.winner = 0
            root.board.bank = [0, 0, 0, 0, 0, 0]
        return [
            {
                "policy": np.ones(48, dtype=np.float32),
                "value": np.array([0.2, -0.2], dtype=np.float32),
            }
            for _ in requests
        ]

    probabilities = np.asarray(
        core.mcts_search(
            mcts, Featurizer(), Encoder(), root, 2, inference, 1.0
        )
    )

    assert root.winner == 0  # The callback's own mutation still takes effect.
    assert inference_calls == 2
    assert probabilities.sum() == pytest.approx(1.0)
    node = mcts.get_node(root_hash)
    assert node.total_visits == 1


def test_batch_max_depth_returns_path_and_releases_virtual_losses():
    root = Game(seed=42)
    for player_index in (0, 1):
        player = root.board.get_player(player_index)
        player.gems = [2, 2, 2, 2, 2, 0]
        root.board.set_player(player_index, player)

    def resources(game):
        return (
            tuple(game.board.bank),
            tuple(
                tuple(game.board.get_player(index).gems)
                for index in (0, 1)
            ),
        )

    mcts = _mcts()
    state = root.clone_light()
    chosen_indices = []
    for _ in range(300):
        before = resources(state)
        chosen = None
        for index in np.flatnonzero(ActionEncoderCpp.get_action_mask(state)):
            action = ActionEncoderCpp.decode(int(index), state)
            if action.type not in (
                core.ActionType.TAKE_DIFFERENT,
                core.ActionType.TAKE_SAME,
            ):
                continue
            child = state.clone_light()
            if child.apply_trusted(action, False) and resources(child) == before:
                chosen = (int(index), child)
                break
        assert chosen is not None
        index, child = chosen
        policy = np.zeros(48, dtype=np.float32)
        valid = np.zeros(48, dtype=np.uint8)
        policy[index] = 1.0
        valid[index] = 1
        mcts.expand_node(state.board_hash(), policy, [0.0, 0.0], valid)
        chosen_indices.append(index)
        state = child

    root_hash = root.board_hash()
    request = mcts.prepare_batch_simulations(root, 0, 1, 1, None)
    assert request["num_leaves"] == 0
    assert request["total_boards"] == 0
    assert len(request["terminals"]) == 1
    terminal_path, terminal_value = request["terminals"][0]
    assert len(terminal_path) == 300
    assert terminal_value == [0.0, 0.0]
    assert mcts.get_node(root_hash).virtual_loss[chosen_indices[0]] == 1

    mcts.apply_batch_results(request, [], [])
    root_node = mcts.get_node(root_hash)
    assert root_node.virtual_loss[chosen_indices[0]] == 0
    assert root_node.N[chosen_indices[0]] == 1


def test_extra_virtual_loss_removal_is_idempotent_at_zero():
    mcts = _mcts()
    valid = np.zeros(48, dtype=np.uint8)
    valid[0] = 1
    mcts.expand_node(123, np.ones(48), [0.0, 0.0], valid)

    mcts.remove_virtual_loss(123, 0)
    assert mcts.get_node(123).virtual_loss[0] == 0
    mcts.add_virtual_loss(123, 0)
    mcts.remove_virtual_loss(123, 0)
    mcts.remove_virtual_loss(123, 0)
    assert mcts.get_node(123).virtual_loss[0] == 0


def test_malformed_batch_path_player_is_ignored_without_out_of_bounds_value():
    root = Game(seed=42)
    root_hash = root.board_hash()
    valid = np.zeros(48, dtype=np.uint8)
    valid[0] = 1
    policy = np.zeros(48, dtype=np.float32)
    policy[0] = 1.0
    mcts = _mcts()
    mcts.expand_node(root_hash, policy, [0.0, 0.0], valid)
    request = mcts.prepare_batch_simulations(root, 0, 1, 1, None)
    assert request["leaf_paths"] == [[(root_hash, 0, 0)]]
    request["leaf_paths"][0][0] = (root_hash, 0, -1)

    mcts.apply_batch_results(
        request,
        [np.ones(48, dtype=np.float32)],
        [np.zeros(2, dtype=np.float32)],
    )
    node = mcts.get_node(root_hash)
    assert node.virtual_loss[0] == 0
    assert node.N[0] == 0
