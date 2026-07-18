"""Deterministic property tests for the public Python release surface.

These tests deliberately use fixed seed corpora instead of a property-test
framework so failures are replayable in minimal PyPI installations.
"""

from __future__ import annotations

import gc
import random
import threading

import numpy as np
import pytest

import csplendor as cs


def _player_payload(player):
    return (
        int(player.points),
        tuple(map(int, player.gems)),
        int(player.packed_gems),
        tuple(map(int, player.bonuses)),
        int(player.packed_bonuses),
        int(player.reserved_count),
        tuple(map(int, player.reserved)),
        tuple(map(bool, player.reserved_is_hidden)),
        int(player.purchased_count),
        tuple(map(int, player.purchased_cards)),
        tuple(map(int, player.acquired_nobles)),
    )


def _board_payload(game):
    board = game.board
    return (
        tuple(map(int, board.bank)),
        tuple(tuple(map(int, level)) for level in board.visible),
        tuple(tuple(map(int, level)) for level in board.decks),
        tuple(map(int, board.nobles)),
        tuple(_player_payload(board.get_player(index)) for index in range(2)),
        int(board.current_player),
        int(board.turn),
        bool(board.final_round),
        bool(board.waiting_noble),
        int(board.winner),
    )


def _game_payload(game):
    return (
        _board_payload(game),
        bool(game.simple_payment_mode),
        bool(game.blank_refill_mode),
        int(game.board_hash()),
    )


def _undo_trace(game):
    probe = game.clone()
    trace = []
    while probe.undo():
        trace.append(_game_payload(probe))
    return tuple(trace)


def _card_partition(board):
    cards = []
    for level in board.visible:
        cards.extend(int(card) for card in level if int(card) >= 0)
    for level in board.decks:
        cards.extend(map(int, level))
    for player in board.players:
        cards.extend(int(card) for card in player.reserved if int(card) >= 0)
        cards.extend(map(int, player.purchased_cards))
    return tuple(sorted(cards))


def _assert_reachable_rule_invariants(game):
    board = game.board
    assert board.current_player in (0, 1)
    assert board.winner in (-2, -1, 0, 1)
    assert all(sum(map(int, player.gems)) <= 10 for player in board.players)
    assert [
        int(board.bank[color])
        + sum(int(player.gems[color]) for player in board.players)
        for color in range(6)
    ] == [4, 4, 4, 4, 4, 5]

    card_ids = []
    for level, cards in enumerate(board.visible, start=1):
        for card_id in map(int, cards):
            if card_id >= 0:
                assert int(cs.get_card(card_id).level) == level
                card_ids.append(card_id)
    for level, cards in enumerate(board.decks, start=1):
        for card_id in map(int, cards):
            assert int(cs.get_card(card_id).level) == level
            card_ids.append(card_id)

    acquired_nobles = []
    for player in board.players:
        reserved = tuple(map(int, player.reserved))
        purchased = tuple(map(int, player.purchased_cards))
        acquired = tuple(map(int, player.acquired_nobles))
        assert int(player.reserved_count) == sum(card >= 0 for card in reserved)
        assert int(player.purchased_count) == len(purchased)
        assert tuple(map(int, player.bonuses)) == tuple(
            sum(int(cs.get_card(card).bonus) == color for card in purchased)
            for color in range(5)
        )
        assert int(player.points) == sum(
            int(cs.get_card(card).points) for card in purchased
        ) + sum(int(cs.get_noble(noble).points) for noble in acquired)
        card_ids.extend(card for card in reserved if card >= 0)
        card_ids.extend(purchased)
        acquired_nobles.extend(acquired)

    assert len(card_ids) == len(set(card_ids))
    if not game.blank_refill_mode:
        assert tuple(sorted(card_ids)) == tuple(range(90))
    all_nobles = tuple(map(int, board.nobles)) + tuple(acquired_nobles)
    assert len(all_nobles) == len(set(all_nobles)) == 3


def _advance(game, rng):
    if game.legal_action_count == 0:
        return False
    return game.apply_random_action(rng.getrandbits(64), False)


def test_fixed_seed_playouts_preserve_all_apply_paths_hashes_and_full_undo():
    """Exercise thousands of reachable transitions with exact rollback."""

    hashes = {}
    checked_plies = 0
    for seed in range(64):
        rng = random.Random(0xC5_0000 + seed)
        game = cs.Game(seed=seed)
        game.simple_payment_mode = bool(seed & 1)
        game.blank_refill_mode = seed % 5 == 0
        snapshots = [_game_payload(game)]
        _assert_reachable_rule_invariants(game)

        for _ in range(96):
            actions = game.legal_actions
            codes = tuple(map(int, game.legal_action_codes))
            assert len(actions) == game.legal_action_count == len(codes)
            assert tuple(int(action.pack()) for action in actions) == codes
            if not actions:
                break

            index = rng.randrange(len(actions))
            action = actions[index]
            code = codes[index]

            via_action = game.clone_light()
            via_code = game.clone_light()
            via_index = game.clone_light()
            assert via_action.apply(action, False)
            assert via_code.apply_action_code(code, False)
            assert via_index.apply_legal_action_index(index, False)
            assert _game_payload(via_action) == _game_payload(via_code)
            assert _game_payload(via_action) == _game_payload(via_index)

            assert game.apply(action, True)
            assert _game_payload(game) == _game_payload(via_action)
            _assert_reachable_rule_invariants(game)
            snapshots.append(_game_payload(game))
            checked_plies += 1

            board_payload = _board_payload(game)
            board_hash = int(game.board_hash())
            previous = hashes.setdefault(board_hash, board_payload)
            assert previous == board_payload
            assert int(game.board_hash()) == board_hash  # cached-hash path
            if game.is_game_over():
                break

        original = _game_payload(game)
        full_clone = game.clone()
        light_clone = game.clone_light()
        assert _game_payload(full_clone) == original
        assert _game_payload(light_clone) == original
        assert light_clone.undo() is False
        if len(snapshots) > 1:
            assert full_clone.undo() is True
            assert _game_payload(game) == original

        for expected in reversed(snapshots[:-1]):
            assert game.undo() is True
            assert _game_payload(game) == expected
        assert game.undo() is False

    assert checked_plies > 4_000


def test_python_and_cpp_state_encoders_match_across_observers_and_canonical_swap():
    featurizer = cs.StateFeaturizer()
    checked = 0

    for seed in range(16):
        rng = random.Random(0xFEA7_0000 + seed)
        game = cs.Game(seed=seed)
        for _ in range(40):
            for observer in (-1, 0, 1):
                python_features = featurizer.featurize(game, observer)
                native_features = np.asarray(
                    cs.StateEncoder.encode(game, observer), dtype=np.float32
                )
                assert native_features.shape == (196,)
                assert np.isfinite(native_features).all()
                np.testing.assert_array_equal(python_features, native_features)

                canonical_zero = np.asarray(
                    cs.StateEncoder.encode_canonical(game, 0, observer),
                    dtype=np.float32,
                )
                canonical_one = np.asarray(
                    cs.StateEncoder.encode_canonical(game, 1, observer),
                    dtype=np.float32,
                )
                expected_one = native_features.copy()
                expected_one[6:42] = native_features[42:78]
                expected_one[42:78] = native_features[6:42]
                expected_one[195] = 1.0 - native_features[195]
                np.testing.assert_array_equal(canonical_zero, native_features)
                np.testing.assert_array_equal(canonical_one, expected_one)
                checked += 1

            if game.is_game_over() or not _advance(game, rng):
                break

    assert checked > 1_000


def test_legacy_python_and_cpp_action_encoders_match_reachable_seed_corpus():
    python_encoder = cs.ActionEncoder()
    checked_states = 0

    for seed in range(16):
        rng = random.Random(0xAC71_0000 + seed)
        game = cs.Game(seed=seed)
        for _ in range(48):
            python_mask = python_encoder.get_action_mask(game)
            native_mask = np.asarray(
                cs.ActionEncoderCpp.get_action_mask(game), dtype=bool
            )
            np.testing.assert_array_equal(python_mask, native_mask)

            for action_id in np.flatnonzero(native_mask):
                action_id = int(action_id)
                python_action = python_encoder.decode(action_id, game)
                native_action = cs.ActionEncoderCpp.decode(action_id, game)
                assert python_action is not None
                assert int(python_action.pack()) == int(native_action.pack())
                assert game.is_legal(native_action)
                assert cs.ActionEncoderCpp.encode(native_action, game) == action_id
            checked_states += 1

            if game.is_game_over() or not _advance(game, rng):
                break

    assert checked_states > 600


def test_determinization_preserves_public_information_and_card_partition():
    game = cs.Game(seed=42)
    reserve = next(
        action
        for action in game.legal_actions
        if action.type == cs.ActionType.RESERVE_DECK
    )
    assert game.apply(reserve, False)
    observer = game.current_player
    before = _game_payload(game)
    public_hash = int(game.board.observable_hash(observer))
    public_features = np.asarray(cs.StateEncoder.encode(game, observer))
    partition = _card_partition(game.board)
    full_hashes = set()

    for seed in range(128):
        world = game.shuffled_clone(observer, seed)
        assert int(world.board.observable_hash(observer)) == public_hash
        np.testing.assert_array_equal(
            cs.StateEncoder.encode(world, observer), public_features
        )
        assert _card_partition(world.board) == partition
        assert world.undo() is False
        full_hashes.add(int(world.board_hash()))

    assert len(full_hashes) > 1
    assert _game_payload(game) == before


def test_invalid_public_game_operations_are_atomic_and_do_not_add_history():
    game = cs.Game(seed=7)
    assert game.apply(game.legal_actions[0], True)
    before = _game_payload(game)
    before_undo = _undo_trace(game)

    invalid_take = cs.Action()
    invalid_take.type = cs.ActionType.TAKE_DIFFERENT
    invalid_take.take = [3, 0, 0, 0, 0]

    invalid_purchase = cs.Action()
    invalid_purchase.type = cs.ActionType.PURCHASE
    invalid_purchase.card_id = int(game.board.visible[0][0])
    invalid_purchase.gold_as = [7, 7, 7, 7, 7]

    operations = [
        lambda: game.apply(cs.Action(), True),
        lambda: game.apply(invalid_take, True),
        lambda: game.apply(invalid_purchase, True),
        lambda: game.apply_action_code((1 << 64) - 1, True),
        lambda: game.apply_legal_action_index(65535, True),
    ]
    for operation in operations:
        assert operation() is False
        assert _game_payload(game) == before
        assert _undo_trace(game) == before_undo

    with pytest.raises(TypeError):
        game.apply(object(), True)
    assert _game_payload(game) == before
    assert _undo_trace(game) == before_undo


@pytest.mark.parametrize(
    ("field", "invalid", "error"),
    [
        ("turn", -1, ValueError),
        ("turn", 65536, ValueError),
        ("current_player", -1, IndexError),
        ("current_player", 2, IndexError),
        ("winner", -3, ValueError),
        ("winner", 2, ValueError),
        ("bank", [4, 4, 4, 4, 4], TypeError),
        ("bank", [4, 4, 4, 4, 4, 256], TypeError),
    ],
)
def test_invalid_scalar_and_fixed_collection_editors_are_atomic(
    field, invalid, error
):
    game = cs.Game(seed=42)
    before = _game_payload(game)
    with pytest.raises(error):
        setattr(game.board, field, invalid)
    assert _game_payload(game) == before


@pytest.mark.parametrize(
    ("field", "invalid"),
    [
        ("gems", [0, 0, 0, 0, 0]),
        ("gems", [0, 0, 0, 0, 0, 256]),
        ("bonuses", [0, 0, 0, 0]),
        ("bonuses", [0, 0, 0, 0, 256]),
        ("reserved", [1, 128]),
        ("reserved_is_hidden", [True, False]),
    ],
)
def test_invalid_player_collection_conversion_preserves_all_fields(field, invalid):
    player = cs.PlayerState()
    player.gems = [1, 2, 3, 0, 1, 2]
    player.bonuses = [2, 1, 0, 3, 1]
    player.reserved = [1, 2, -1]
    player.reserved_is_hidden = [True, False, False]
    before = _player_payload(player)

    with pytest.raises(TypeError):
        setattr(player, field, invalid)
    assert _player_payload(player) == before


def test_invalid_board_player_index_is_atomic():
    game = cs.Game(seed=42)
    before = _game_payload(game)
    replacement = game.board.get_player(0)
    replacement.points = 9
    for index in (-1, 2):
        with pytest.raises(IndexError):
            game.board.set_player(index, replacement)
        assert _game_payload(game) == before


def _parallel_config(simulations=12):
    config = cs.MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.num_simulations = simulations
    return config


def _parallel_options(simulations=12):
    options = cs.ParallelSearchOptions()
    options.num_threads = 4
    options.batch_size = 3
    options.batch_wait_us = 25
    options.max_inflight = 8
    options.num_simulations = simulations
    options.master_seed = 0xC5EED
    options.search_nonce = 17
    options.evaluator_version = 303
    return options


def _valid_parallel_results(requests):
    results = []
    for request in requests:
        mask = np.asarray(request["valid_actions"], dtype=np.float32)
        if mask.sum():
            mask /= mask.sum()
        results.append(
            {
                "policy": mask,
                "value": np.array([0.25, -0.25], dtype=np.float32),
            }
        )
    return results


@pytest.mark.parametrize(
    "malformation",
    ["count", "missing", "short_policy", "negative_policy", "nan_value"],
)
def test_parallel_callback_validation_failure_drains_and_mcts_is_reusable(
    malformation,
):
    mcts = cs.MCTS(_parallel_config())
    options = _parallel_options()
    callback_count = 0

    def malformed_after_bootstrap(requests):
        nonlocal callback_count
        callback_count += 1
        if callback_count == 1:
            return _valid_parallel_results(requests)
        if malformation == "count":
            return []
        if malformation == "missing":
            return [{"policy": np.ones(48, dtype=np.float32)} for _ in requests]
        if malformation == "short_policy":
            return [
                {
                    "policy": np.ones(47, dtype=np.float32),
                    "value": np.zeros(2, dtype=np.float32),
                }
                for _ in requests
            ]
        if malformation == "negative_policy":
            return [
                {
                    "policy": -np.ones(48, dtype=np.float32),
                    "value": np.zeros(2, dtype=np.float32),
                }
                for _ in requests
            ]
        return [
            {
                "policy": np.ones(48, dtype=np.float32),
                "value": np.array([np.nan, 0.0], dtype=np.float32),
            }
            for _ in requests
        ]

    with pytest.raises(ValueError):
        cs.mcts_search_parallel_native(
            mcts,
            cs.Game(seed=42),
            options,
            malformed_after_bootstrap,
            1.0,
        )
    assert callback_count >= 2
    assert not mcts.is_parallel_search_active()

    recovered = cs.mcts_search_parallel_native(
        mcts,
        cs.Game(seed=42),
        _parallel_options(),
        _valid_parallel_results,
        1.0,
    )
    assert recovered.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert recovered.ledger.completed == 12
    assert recovered.ledger.virtual_loss_balanced


def test_precancelled_token_is_owned_by_options_skips_callbacks_and_recovers():
    mcts = cs.MCTS(_parallel_config(simulations=8))
    options = _parallel_options(simulations=8)
    token = cs.ParallelCancellationToken()
    token.request_cancel()
    options.cancellation_token = token
    del token
    gc.collect()
    assert options.cancellation_token.is_cancelled

    callback_calls = 0

    def should_not_run(requests):
        nonlocal callback_calls
        callback_calls += 1
        return _valid_parallel_results(requests)

    result = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), options, should_not_run, 1.0
    )
    assert callback_calls == 0
    assert result.stop_reason == cs.ParallelSearchStopReason.CANCELLED
    assert result.partial
    assert result.ledger.issued == result.ledger.cancelled == 0
    assert not mcts.is_parallel_search_active()

    recovered = cs.mcts_search_parallel_native(
        mcts,
        cs.Game(seed=42),
        _parallel_options(simulations=8),
        _valid_parallel_results,
        1.0,
    )
    assert recovered.ledger.completed == 8


def test_parallel_options_are_snapshotted_before_the_gil_is_released():
    mcts = cs.MCTS(_parallel_config(simulations=17))
    options = _parallel_options(simulations=17)
    entered = threading.Event()
    release = threading.Event()
    outcome = {}

    def blocked_bootstrap(requests):
        if not entered.is_set():
            with pytest.raises(RuntimeError, match="active search"):
                mcts.reset_replay_sequence(1, 1)
            entered.set()
            assert release.wait(timeout=5)
        return _valid_parallel_results(requests)

    def run_search():
        try:
            outcome["result"] = cs.mcts_search_parallel_native(
                mcts, cs.Game(seed=42), options, blocked_bootstrap, 1.0
            )
        except BaseException as error:
            outcome["error"] = error

    thread = threading.Thread(target=run_search)
    thread.start()
    assert entered.wait(timeout=5)
    cancelled = cs.ParallelCancellationToken()
    cancelled.request_cancel()
    options.num_simulations = 1
    options.master_seed = 999
    options.evaluator_version = 999
    options.cancellation_token = cancelled
    release.set()
    thread.join(timeout=8)

    assert not thread.is_alive()
    assert "error" not in outcome
    result = outcome["result"]
    assert result.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert result.resolved_seed == 0xC5EED
    assert result.ledger.issued == result.ledger.completed == 17
    assert sum(result.visits) == 17


def test_invalid_parallel_preflight_does_not_call_back_or_consume_replay_nonce():
    mcts = cs.MCTS(_parallel_config(simulations=4))
    mcts.reset_replay_sequence(123, 41)
    invalid = _parallel_options(simulations=4)
    invalid.master_seed = None
    invalid.num_threads = 0
    calls = 0

    def evaluator(requests):
        nonlocal calls
        calls += 1
        return _valid_parallel_results(requests)

    with pytest.raises(ValueError, match="thread and batch"):
        cs.mcts_search_parallel_native(
            mcts, cs.Game(seed=42), invalid, evaluator, 1.0
        )
    assert calls == 0
    assert not mcts.is_parallel_search_active()
    assert mcts.tree_size() == 0

    valid = _parallel_options(simulations=4)
    valid.master_seed = None
    valid.search_nonce = (1 << 64) - 1
    result = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), valid, evaluator, 1.0
    )
    assert result.resolved_seed == 123
    assert result.search_nonce == 41


def test_precancelled_root_parallel_skips_all_python_callbacks():
    config = _parallel_config(simulations=20)
    options = _parallel_options(simulations=20)
    options.mode = cs.ParallelSearchMode.ROOT_PARALLEL
    token = cs.ParallelCancellationToken()
    token.request_cancel()
    options.cancellation_token = token
    calls = 0

    def should_not_run(requests):
        nonlocal calls
        calls += 1
        return _valid_parallel_results(requests)

    result = cs.mcts_search_root_parallel_native(
        config,
        cs.Game(seed=42),
        20,
        4,
        options,
        should_not_run,
        1.0,
    )
    assert calls == 0
    assert result.merged.stop_reason == cs.ParallelSearchStopReason.CANCELLED
    assert result.merged.partial
    assert result.merged.ledger.issued == 0


def test_parallel_result_and_static_catalog_values_outlive_and_isolate_sources():
    mcts = cs.MCTS(_parallel_config(simulations=4))
    root = cs.Game(seed=42)
    options = _parallel_options(simulations=4)
    result = cs.mcts_search_parallel_native(
        mcts, root, options, _valid_parallel_results, 1.0
    )
    expected_visits = tuple(result.visits)
    detached = result.visits
    detached[0] += 999
    assert tuple(result.visits) == expected_visits

    card = cs.get_card(0)
    noble = cs.get_noble(0)
    card.id = 89
    noble.id = 11
    assert int(cs.get_card(0).id) == 0
    assert int(cs.get_noble(0).id) == 0

    del mcts, root, options
    gc.collect()
    assert tuple(result.visits) == expected_visits
    assert result.ledger.completed == 4


def test_member_references_and_value_snapshots_have_safe_python_lifetimes():
    game = cs.Game(seed=42)
    board = game.board
    board_hash = int(board.hash())
    action = game.legal_actions[0]
    action_code = int(action.pack())
    del game
    gc.collect()

    # Game.board uses reference-internal lifetime ownership, while legal
    # actions and MCTS nodes are detached value snapshots.
    assert int(board.hash()) == board_hash
    assert int(action.pack()) == action_code
    board.turn = 9
    assert int(board.turn) == 9

    mcts = cs.MCTS(_parallel_config())
    valid = np.zeros(48, dtype=np.uint8)
    valid[3] = 1
    policy = np.zeros(48, dtype=np.float32)
    policy[3] = 1.0
    mcts.expand_node(123, policy, [0.5, -0.5], valid)
    node = mcts.get_node(123)
    node_prior = node.prior
    node_prior[3] = 0.0
    del mcts
    gc.collect()
    assert node.is_expanded
    assert node.prior[3] == pytest.approx(1.0)


def test_encoder_numpy_results_own_their_memory_and_are_isolated():
    game = cs.Game(seed=42)
    mask = cs.ActionEncoderCpp.get_action_mask(game)
    scored_mask, scores = cs.ActionEncoderCpp.get_action_mask_with_scores(game)
    policy = cs.ActionEncoderCpp.get_heuristic_policy(game)
    mask_v2 = cs.ActionEncoderV2.get_action_mask(game)
    mask_v3 = cs.ActionEncoderV3.get_action_mask(game)
    arrays = [mask, scored_mask, scores, policy, mask_v2, mask_v3]
    snapshots = [array.copy() for array in arrays]

    assert all(array.flags.owndata for array in arrays)
    for seed in range(64):
        probe = cs.Game(seed=seed)
        cs.ActionEncoderCpp.get_action_mask_with_scores(probe)
        cs.ActionEncoderV2.get_action_mask(probe)
        cs.ActionEncoderV3.get_action_mask(probe)
    del game
    gc.collect()

    for array, expected in zip(arrays, snapshots):
        np.testing.assert_array_equal(array, expected)

    mask[:] = 0
    fresh = cs.ActionEncoderCpp.get_action_mask(cs.Game(seed=42))
    np.testing.assert_array_equal(fresh, snapshots[0])
