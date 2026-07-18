"""PS-0 characterization fixtures for parallel-search correctness blockers.

These tests deliberately capture the pre-PS-1 behavior.  PS-1 must turn the
first two aliases into explicit, correct behavior and retain the third fixture
as the world-local availability contract.
"""

from __future__ import annotations

import hashlib
import struct

import numpy as np
import pytest

import csplendor as cs


def _first_card_id(level: int) -> int:
    return next(card_id for card_id in range(90) if cs.get_card(card_id).level == level)


def _set_hidden_reserve(game: cs.Game, player_index: int, card_id: int) -> None:
    player = game.board.get_player(player_index)
    player.reserved = [card_id, -1, -1]
    player.reserved_is_hidden = [True, False, False]
    game.board.set_player(player_index, player)


def _tree_snapshot_digest(mcts: cs.MCTS, hashes: list[int]) -> str:
    digest = hashlib.sha256()
    for state_hash in sorted(hashes):
        node = mcts.get_node(state_hash)
        assert node is not None
        digest.update(struct.pack("<Q", state_hash))
        digest.update(np.asarray(node.valid_actions, dtype=np.uint8).tobytes())
        digest.update(np.asarray(node.prior, dtype="<f4").tobytes())
        digest.update(np.asarray(node.N, dtype="<u4").tobytes())
        digest.update(np.asarray(node.Q, dtype="<f4").tobytes())
        digest.update(np.asarray(node.virtual_loss, dtype="<i4").tobytes())
        digest.update(struct.pack("<I??", node.total_visits, node.is_terminal, node.is_expanded))
        digest.update(np.asarray(node.value, dtype="<f4").tobytes())
    return digest.hexdigest()


def _mcts(*, determinization: bool) -> cs.MCTS:
    config = cs.MCTSConfig()
    config.use_determinization = determinization
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.fpu = 0.0
    return cs.MCTS(config)


def test_single_thread_tree_snapshot_digest() -> None:
    mcts = _mcts(determinization=False)
    hashes = [0xF00D, 0xA11CE, 0x20]
    for index, state_hash in enumerate(hashes):
        policy = np.arange(1, 49, dtype=np.float32) + index
        valid = np.zeros(48, dtype=np.uint8)
        valid[[index, index + 3]] = 1
        mcts.expand_node(state_hash, policy, [0.25 * index, -0.25 * index], valid)
    mcts.update_stats(0xA11CE, 1, 0.5)
    mcts.update_stats(0xA11CE, 1, -0.25)
    mcts.add_virtual_loss(0xF00D, 2)

    assert _tree_snapshot_digest(mcts, hashes) == (
        "c01f04d7cc59f48ec6e07fb7ae932f5601196bca4fab47f4da262d44a47a27f6"
    )


def test_hidden_tier_is_part_of_observable_hash() -> None:
    game = cs.Game(seed=42)
    _set_hidden_reserve(game, 0, _first_card_id(1))
    level_one_hash = int(game.board.observable_hash(1))
    level_one_features = np.asarray(
        cs.StateEncoder.encode_canonical(game, 0, 1), dtype=np.float32
    )

    _set_hidden_reserve(game, 0, _first_card_id(2))
    level_two_hash = int(game.board.observable_hash(1))
    level_two_features = np.asarray(
        cs.StateEncoder.encode_canonical(game, 0, 1), dtype=np.float32
    )

    assert level_one_hash != level_two_hash
    assert not np.array_equal(level_one_features, level_two_features)


def test_hidden_card_identity_within_a_tier_is_not_observable() -> None:
    game = cs.Game(seed=42)
    _set_hidden_reserve(game, 0, 40)
    first_hash = int(game.board.observable_hash(1))
    first_features = np.asarray(cs.StateEncoder.encode_canonical(game, 0, 1))

    _set_hidden_reserve(game, 0, 41)
    second_hash = int(game.board.observable_hash(1))
    second_features = np.asarray(cs.StateEncoder.encode_canonical(game, 0, 1))

    assert first_hash == second_hash
    np.testing.assert_array_equal(first_features, second_features)


def test_ps0_characterizes_divergent_worlds_merged_under_world_zero_hash() -> None:
    root = cs.Game(seed=42)
    observer = root.current_player
    reserve = next(
        action
        for action in root.legal_actions
        if action.type == cs.ActionType.RESERVE_VISIBLE
    )
    action_index = cs.ActionEncoderCpp.encode(reserve, root)

    config = cs.MCTSConfig()
    config.use_determinization = True
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    mcts = cs.MCTS(config)

    policy = np.zeros(48, dtype=np.float32)
    valid = np.zeros(48, dtype=np.uint8)
    policy[action_index] = 1.0
    valid[action_index] = 1
    root_hash = int(root.board.observable_hash(observer))
    mcts.expand_node(root_hash, policy, [0.0, 0.0], valid)

    deterministic_leaf_hashes = set()
    for seed in range(20):
        world = root.shuffled_clone(observer, seed)
        decoded = cs.ActionEncoderCpp.decode(action_index, world)
        assert world.apply_trusted(decoded, False)
        deterministic_leaf_hashes.add(int(world.board.observable_hash(observer)))
    assert len(deterministic_leaf_hashes) > 1

    # The legacy API must not average those different public states into the
    # world-zero node.  Parallel redesign uses one independent world ticket.
    with pytest.raises(ValueError, match="one world per simulation"):
        mcts.prepare_batch_simulations(root, observer, 1, 20, None)


def test_information_set_can_have_world_local_action_availability() -> None:
    observer = 0
    purchasable = cs.Game(seed=42)
    purchasable.board.current_player = 1
    card_zero = cs.get_card(0)
    player = purchasable.board.get_player(1)
    player.reserved = [card_zero.id, -1, -1]
    player.reserved_is_hidden = [True, False, False]
    player.gems = list(card_zero.cost) + [0]
    player.bonuses = [0, 0, 0, 0, 0]
    purchasable.board.set_player(1, player)

    unavailable = purchasable.clone_light()
    _set_hidden_reserve(unavailable, 1, 1)

    assert int(purchasable.board.observable_hash(observer)) == int(
        unavailable.board.observable_hash(observer)
    )
    available_mask = np.asarray(cs.ActionEncoderCpp.get_action_mask(purchasable))
    unavailable_mask = np.asarray(cs.ActionEncoderCpp.get_action_mask(unavailable))
    assert available_mask[42] == 1
    assert unavailable_mask[42] == 0
    np.testing.assert_array_equal(
        np.flatnonzero(available_mask != unavailable_mask), np.array([42])
    )


def _reserved_purchase_world(card_id: int) -> cs.Game:
    game = cs.Game(seed=42)
    decks = [list(level) for level in game.board.decks]
    decks[0] = []
    game.board.decks = decks
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.reserved = [card_id, -1, -1]
    player.reserved_is_hidden = [True, False, False]
    player.gems = [0, 0, 0, 0, 0, 0]
    player.bonuses = [2, 2, 2, 2, 2]
    game.board.set_player(1, player)
    return game


def test_world_local_mask_prevents_unavailable_action_draw_visit() -> None:
    observer = 0
    available = _reserved_purchase_world(1)
    unavailable = _reserved_purchase_world(8)
    available_mask = np.asarray(cs.ActionEncoderCpp.get_action_mask(available))
    unavailable_mask = np.asarray(cs.ActionEncoderCpp.get_action_mask(unavailable))
    assert available_mask[42] == 1
    assert unavailable_mask[42] == 0
    assert int(available.board.observable_hash(observer)) == int(
        unavailable.board.observable_hash(observer)
    )

    mcts = _mcts(determinization=True)
    state_hash = int(available.board.observable_hash(observer))
    policy = np.zeros(48, dtype=np.float32)
    policy[42] = 1.0
    mcts.expand_node(state_hash, policy, [0.0, 0.0], available_mask)

    request = mcts.prepare_batch_simulations(unavailable, observer, 1, 1, None)
    assert request["num_leaves"] == 1
    assert request["terminals"] == []
    assert request["leaf_paths"][0][0][1] != 42
    policies = [np.ones(48, dtype=np.float32) for _ in request["flat_boards"]]
    values = [np.zeros(2, dtype=np.float32) for _ in request["flat_boards"]]
    mcts.apply_batch_results(request, policies, values)
    node = mcts.get_node(state_hash)
    assert node.N[42] == 0
    assert node.Q[42] == 0.0
    assert node.virtual_loss[42] == 0


def test_later_world_can_use_unmasked_base_prior_for_newly_available_action() -> None:
    observer = 0
    unavailable = _reserved_purchase_world(8)
    available = _reserved_purchase_world(1)
    state_hash = int(unavailable.board.observable_hash(observer))
    assert state_hash == int(available.board.observable_hash(observer))

    unavailable_mask = np.asarray(cs.ActionEncoderCpp.get_action_mask(unavailable))
    assert unavailable_mask[42] == 0
    policy = np.zeros(48, dtype=np.float32)
    policy[42] = 1.0
    mcts = _mcts(determinization=True)
    mcts.expand_node(state_hash, policy, [0.0, 0.0], unavailable_mask)

    request = mcts.prepare_batch_simulations(available, observer, 1, 1, None)
    assert request["leaf_paths"][0][0] == (state_hash, 42, 1)


def test_dirichlet_noise_contract_without_reproducible_seed() -> None:
    mcts = _mcts(determinization=False)
    valid = np.zeros(48, dtype=np.uint8)
    valid[[1, 7, 9]] = 1
    mcts.expand_node(123, np.ones(48, dtype=np.float32), [0.0, 0.0], valid)

    noise = np.asarray(mcts.generate_dirichlet_noise(123), dtype=np.float32)
    assert np.isfinite(noise).all()
    assert (noise >= 0.0).all()
    assert float(noise.sum()) == pytest.approx(1.0, abs=1e-6)
    np.testing.assert_array_equal(noise[valid == 0], np.zeros(45, dtype=np.float32))
