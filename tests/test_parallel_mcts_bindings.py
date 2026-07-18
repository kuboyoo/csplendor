import gc
import math
import threading
import time

import numpy as np
import pytest

import csplendor as cs

ACTION_COUNT = 48
PLAYER_COUNT = 2
FEATURE_COUNT = 196


def _config(*, simulations=24):
    config = cs.MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    config.forced_playouts = False
    config.num_simulations = simulations
    return config


def _options(*, mode=cs.ParallelSearchMode.THROUGHPUT, threads=4,
             simulations=24, evaluator_version=1):
    options = cs.ParallelSearchOptions()
    options.mode = mode
    options.tree_backend = cs.ParallelTreeBackend.SHARDED
    options.shard_count = 8
    options.num_threads = threads
    options.batch_size = 4
    options.batch_wait_us = 50
    options.max_inflight = max(threads * 2, 4)
    options.deterministic_epoch_size = 5
    options.num_simulations = simulations
    options.master_seed = 0x12345678
    options.search_nonce = 91
    options.simulation_id_base = 500
    options.evaluator_version = evaluator_version
    options.timeout_ms = 0
    return options


def _evaluate(requests):
    results = []
    for request in requests:
        mask = np.asarray(request["valid_actions"], dtype=np.uint8)
        policy = mask.astype(np.float32)
        count = int(mask.sum())
        if count:
            policy /= np.float32(count)
        # Request-derived, exactly representable values make deterministic
        # comparisons cover value backup as well as policy selection.
        raw = (int(request["tree_key"]["position_hash"]) & 7) - 3
        value = np.array([raw / 8.0, -raw / 8.0], dtype=np.float32)
        results.append({"policy": policy, "value": value})
    return results


def _run(*, mode=cs.ParallelSearchMode.THROUGHPUT, threads=4,
         simulations=24, evaluator=None, game=None, mcts=None,
         evaluator_version=1):
    mcts = mcts or cs.MCTS(_config(simulations=simulations))
    game = game or cs.Game(seed=42)
    result = cs.mcts_search_parallel_native(
        mcts,
        game,
        _options(
            mode=mode,
            threads=threads,
            simulations=simulations,
            evaluator_version=evaluator_version,
        ),
        evaluator or _evaluate,
        1.0,
    )
    return mcts, result


def _result_digest(result):
    ledger = result.ledger
    return (
        tuple(result.visits),
        tuple(result.q_values),
        tuple(result.probabilities),
        result.resolved_seed,
        result.search_nonce,
        result.tree_generation,
        result.tree_size,
        result.stop_reason,
        result.partial,
        ledger.issued,
        ledger.selected,
        ledger.evaluation_owner,
        ledger.evaluation_waiter,
        ledger.evaluated_boards,
        ledger.completed,
        ledger.virtual_loss_added,
        ledger.virtual_loss_released,
    )


@pytest.mark.parametrize(
    "mode",
    [
        cs.ParallelSearchMode.THROUGHPUT,
        cs.ParallelSearchMode.DETERMINISTIC_EPOCH,
    ],
)
def test_parallel_options_result_and_ledger_contract(mode):
    seen_requests = []

    def evaluator(requests):
        seen_requests.extend(requests)
        return _evaluate(requests)

    mcts, result = _run(mode=mode, simulations=20, evaluator=evaluator)

    assert result.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert result.partial is False
    assert result.resolved_seed == 0x12345678
    assert result.rng_version == 1
    assert result.search_nonce == 91
    assert len(result.visits) == ACTION_COUNT
    assert len(result.q_values) == ACTION_COUNT
    assert len(result.probabilities) == ACTION_COUNT
    assert sum(result.visits) == 20
    assert all(math.isfinite(value) for value in result.q_values)
    assert sum(result.probabilities) == pytest.approx(1.0, abs=1e-6)
    assert result.ledger.issued == 20
    assert result.ledger.completed == 20
    assert result.ledger.cancelled == 0
    assert result.ledger.failed == 0
    assert result.ledger.virtual_loss_balanced is True
    assert result.ledger.virtual_loss_added == result.ledger.virtual_loss_released
    assert result.ledger.reservations_committed == result.ledger.selected
    assert not mcts.is_parallel_search_active()

    assert seen_requests
    for request in seen_requests:
        assert set(request) == {
            "pending_id", "simulation_id", "tree_key", "features",
            "valid_actions",
        }
        assert set(request["tree_key"]) == {
            "position_hash", "key_version", "observer", "domain",
            "mode_bits",
        }
        assert request["features"].shape == (FEATURE_COUNT,)
        assert request["features"].dtype == np.float32
        assert request["valid_actions"].shape == (ACTION_COUNT,)
        assert request["valid_actions"].dtype == np.uint8


def test_none_master_seed_uses_mcts_replay_sequence_and_zero_is_explicit():
    mcts = cs.MCTS(_config(simulations=4))
    mcts.reset_replay_sequence(0, 17)
    options = _options(threads=1, simulations=4)
    options.master_seed = None
    options.search_nonce = (1 << 64) - 1

    first = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), options, _evaluate, 1.0
    )
    second = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), options, _evaluate, 1.0
    )

    assert first.resolved_seed == 0
    assert second.resolved_seed == 0
    assert first.search_nonce == 17
    assert second.search_nonce == 18
    assert first.rng_version == second.rng_version == 1


def test_python_cooperative_cancellation_token_drains_and_reuses_mcts():
    mcts = cs.MCTS(_config(simulations=24))
    options = _options(threads=4, simulations=24, evaluator_version=73)
    token = cs.ParallelCancellationToken()
    options.cancellation_token = token
    callback_count = 0

    def evaluator(requests):
        nonlocal callback_count
        callback_count += 1
        # The first call bootstraps the root. Cancel with logical tickets in
        # flight at the next callback boundary.
        if callback_count == 2:
            token.request_cancel()
        return _evaluate(requests)

    result = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), options, evaluator, 1.0
    )
    assert token.is_cancelled
    assert result.stop_reason == cs.ParallelSearchStopReason.CANCELLED
    assert result.partial
    assert result.ledger.issued == (
        result.ledger.completed + result.ledger.cancelled
    )
    assert result.ledger.virtual_loss_balanced
    assert not mcts.is_parallel_search_active()

    recovered_options = _options(
        threads=2, simulations=8, evaluator_version=73
    )
    recovered = cs.mcts_search_parallel_native(
        mcts, cs.Game(seed=42), recovered_options, _evaluate, 1.0
    )
    assert recovered.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert recovered.ledger.completed == 8


def test_deterministic_epoch_result_is_identical_for_1_2_4_8_threads():
    reference = _result_digest(
        _run(
            mode=cs.ParallelSearchMode.DETERMINISTIC_EPOCH,
            threads=1,
            simulations=31,
        )[1]
    )
    for threads in (2, 4, 8):
        candidate = _run(
            mode=cs.ParallelSearchMode.DETERMINISTIC_EPOCH,
            threads=threads,
            simulations=31,
        )[1]
        assert _result_digest(candidate) == reference


def test_root_parallel_fallback_is_exposed_and_preserves_exact_budget():
    config = _config(simulations=99)
    options = _options(
        mode=cs.ParallelSearchMode.ROOT_PARALLEL,
        threads=4,
        simulations=99,
        evaluator_version=7,
    )
    active = 0
    max_active = 0

    def serialized_evaluator(requests):
        nonlocal active, max_active
        active += 1
        max_active = max(max_active, active)
        try:
            time.sleep(0.0005)
            return _evaluate(requests)
        finally:
            active -= 1

    result = cs.mcts_search_root_parallel_native(
        config, cs.Game(seed=42), 17, 4, options,
        serialized_evaluator, 1.0
    )

    assert isinstance(result, cs.RootParallelSearchResult)
    assert len(result.workers) == 4
    assert result.duplicate_root_evaluations_avoided == 3
    assert result.merged.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert result.merged.partial is False
    assert result.merged.ledger.issued == 17
    assert result.merged.ledger.completed == 17
    assert sum(result.merged.visits) == 17
    assert result.merged.ledger.virtual_loss_balanced
    assert sum(worker.ledger.issued for worker in result.workers) == 17
    assert max_active == 1


def test_root_parallel_timeout_does_not_drain_serialized_callback_waiters():
    config = _config(simulations=99)
    options = _options(
        mode=cs.ParallelSearchMode.ROOT_PARALLEL,
        threads=8,
        simulations=99,
        evaluator_version=8,
    )
    options.timeout_ms = 20
    callback_count = 0
    main_callback_count = 0

    def slow_after_bootstrap(requests):
        nonlocal callback_count, main_callback_count
        callback_count += 1
        is_bootstrap = all(
            request["simulation_id"] == (1 << 64) - 1
            for request in requests
        )
        if not is_bootstrap:
            main_callback_count += 1
            time.sleep(0.05)
        return _evaluate(requests)

    started = time.monotonic()
    result = cs.mcts_search_root_parallel_native(
        config, cs.Game(seed=43), 64, 8, options,
        slow_after_bootstrap, 1.0
    )
    elapsed = time.monotonic() - started

    assert result.merged.partial
    assert result.merged.stop_reason == cs.ParallelSearchStopReason.TIMED_OUT
    assert callback_count <= 2  # one bootstrap plus at most one overrun
    assert main_callback_count <= 1
    assert elapsed < 0.25
    assert result.merged.ledger.virtual_loss_balanced


def test_root_parallel_failure_stops_serialized_callback_waiters():
    config = _config(simulations=99)
    options = _options(
        mode=cs.ParallelSearchMode.ROOT_PARALLEL,
        threads=8,
        simulations=99,
        evaluator_version=9,
    )
    callback_count = 0
    main_callback_count = 0

    def fail_first_worker_callback(requests):
        nonlocal callback_count, main_callback_count
        callback_count += 1
        is_bootstrap = all(
            request["simulation_id"] == (1 << 64) - 1
            for request in requests
        )
        if not is_bootstrap:
            main_callback_count += 1
            time.sleep(0.02)
            raise RuntimeError("intentional serialized root failure")
        return _evaluate(requests)

    started = time.monotonic()
    with pytest.raises(RuntimeError, match="intentional serialized root failure"):
        cs.mcts_search_root_parallel_native(
            config, cs.Game(seed=44), 64, 8, options,
            fail_first_worker_callback, 1.0
        )
    elapsed = time.monotonic() - started

    assert callback_count <= 2  # one bootstrap plus the failing callback
    assert main_callback_count == 1
    assert elapsed < 0.2


def test_exact_tree_reuse_separates_root_observers_across_turns():
    config = _config(simulations=1)
    mcts = cs.MCTS(config)
    game = cs.Game(seed=42)
    options = _options(threads=1, simulations=1, evaluator_version=17)

    first_requests = []

    def first_evaluator(requests):
        first_requests.extend(requests)
        return _evaluate(requests)

    first = cs.mcts_search_parallel_native(
        mcts, game, options, first_evaluator, 1.0
    )
    selected = max(range(ACTION_COUNT), key=lambda action: first.visits[action])
    assert first.visits[selected] == 1
    assert game.apply_trusted(cs.ActionEncoderCpp.decode(selected, game))
    assert game.current_player == 1
    assert len(first_requests) >= 2
    previous_leaf_key = first_requests[-1]["tree_key"]
    assert previous_leaf_key["observer"] == 0

    second_requests = []

    def second_evaluator(requests):
        second_requests.extend(requests)
        return _evaluate(requests)

    cs.mcts_search_parallel_native(mcts, game, options, second_evaluator, 1.0)
    assert second_requests  # the player-0 leaf was not reused as player-1 root
    new_root_key = second_requests[0]["tree_key"]
    assert new_root_key["position_hash"] == previous_leaf_key["position_hash"]
    assert new_root_key["domain"] == previous_leaf_key["domain"]
    assert new_root_key["observer"] == 1


def test_python_callback_is_serial_and_request_arrays_remain_owned():
    active = 0
    max_active = 0
    callback_threads = set()
    retained = []
    retained_copies = []
    caller_thread = threading.get_ident()

    def evaluator(requests):
        nonlocal active, max_active
        active += 1
        max_active = max(max_active, active)
        callback_threads.add(threading.get_ident())
        for request in requests:
            features = request["features"]
            valid = request["valid_actions"]
            assert features.flags["OWNDATA"] and features.base is None
            assert valid.flags["OWNDATA"] and valid.base is None
            assert features.flags["C_CONTIGUOUS"]
            assert valid.flags["C_CONTIGUOUS"]
            retained.extend((features, valid))
            retained_copies.extend((features.copy(), valid.copy()))
        # If callbacks were dispatched on multiple native threads, sleeping
        # releases the GIL and exposes overlap to the active counter.
        time.sleep(0.001)
        result = _evaluate(requests)
        active -= 1
        return result

    _run(simulations=32, evaluator=evaluator)
    gc.collect()
    assert active == 0
    assert max_active == 1
    assert callback_threads == {caller_thread}
    assert retained
    for array, expected in zip(retained, retained_copies):
        np.testing.assert_array_equal(array, expected)


def test_native_search_releases_gil_for_python_heartbeat():
    stop = threading.Event()
    started = threading.Event()
    heartbeats = 0

    def heartbeat():
        nonlocal heartbeats
        started.set()
        while not stop.is_set():
            heartbeats += 1

    thread = threading.Thread(target=heartbeat)
    thread.start()
    assert started.wait(timeout=2)
    before = heartbeats
    try:
        _run(simulations=96)
    finally:
        stop.set()
        thread.join(timeout=2)
    assert not thread.is_alive()
    assert heartbeats > before


def test_active_callback_rejects_clear_and_config_change_immediately():
    mcts = cs.MCTS(_config(simulations=12))
    observations = []

    def evaluator(requests):
        observations.append(mcts.is_parallel_search_active())
        with pytest.raises(RuntimeError, match="active search"):
            mcts.clear()
        replacement = mcts.get_config_snapshot()
        replacement.cpuct += 1.0
        with pytest.raises(RuntimeError, match="active search"):
            mcts.set_config(replacement)
        return _evaluate(requests)

    _, result = _run(mcts=mcts, simulations=12, evaluator=evaluator)
    assert result.ledger.completed == 12
    assert observations and all(observations)


def test_python_callback_exception_cleans_up_and_mcts_is_reusable():
    mcts = cs.MCTS(_config(simulations=12))
    callback_count = 0

    def failing(requests):
        nonlocal callback_count
        callback_count += 1
        if callback_count == 2:
            raise RuntimeError("intentional Python evaluator failure")
        return _evaluate(requests)

    with pytest.raises(RuntimeError, match="intentional Python evaluator failure"):
        _run(mcts=mcts, simulations=12, evaluator=failing,
             evaluator_version=31)
    assert not mcts.is_parallel_search_active()

    _, recovered = _run(mcts=mcts, simulations=12, evaluator_version=31)
    assert recovered.stop_reason == cs.ParallelSearchStopReason.COMPLETED
    assert recovered.ledger.completed == 12
    assert recovered.ledger.virtual_loss_balanced


def test_same_mcts_second_search_fails_fast_while_first_is_active():
    mcts = cs.MCTS(_config(simulations=8))
    entered = threading.Event()
    release = threading.Event()
    outcome = {}

    def blocked(requests):
        if not entered.is_set():
            entered.set()
            assert release.wait(timeout=5)
        return _evaluate(requests)

    def first_search():
        try:
            outcome["result"] = _run(
                mcts=mcts, simulations=8, evaluator=blocked,
                evaluator_version=41,
            )[1]
        except BaseException as error:  # surfaced in the main test thread
            outcome["error"] = error

    thread = threading.Thread(target=first_search)
    thread.start()
    assert entered.wait(timeout=5)
    try:
        with pytest.raises(RuntimeError, match="already active"):
            _run(mcts=mcts, simulations=1, evaluator_version=41)
    finally:
        release.set()
        thread.join(timeout=5)
    assert not thread.is_alive()
    assert "error" not in outcome
    assert outcome["result"].ledger.completed == 8


def test_distinct_mcts_instances_can_search_concurrently():
    barrier = threading.Barrier(2)
    instances = [cs.MCTS(_config(simulations=8)) for _ in range(2)]
    results = [None, None]
    errors = []
    saw_active = [False, False]

    def run_one(index):
        first_callback = True

        def callback(requests):
            nonlocal first_callback
            if first_callback:
                first_callback = False
                saw_active[index] = instances[index].is_parallel_search_active()
                barrier.wait(timeout=5)
            return _evaluate(requests)

        try:
            results[index] = _run(
                mcts=instances[index],
                game=cs.Game(seed=42 + index),
                simulations=8,
                evaluator=callback,
                evaluator_version=50 + index,
            )[1]
        except BaseException as error:
            errors.append(error)

    threads = [threading.Thread(target=run_one, args=(index,)) for index in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=8)
    assert all(not thread.is_alive() for thread in threads)
    assert not errors
    assert all(saw_active)
    assert all(result.ledger.completed == 8 for result in results)


def test_root_is_snapshotted_before_callbacks_and_isolation_is_stable():
    root = cs.Game(seed=99)
    original_hash = root.board_hash()
    entered = threading.Event()
    release = threading.Event()
    outcome = {}

    def blocked(requests):
        if not entered.is_set():
            entered.set()
            assert release.wait(timeout=5)
        return _evaluate(requests)

    def search_mutated_root():
        try:
            outcome["result"] = _run(
                game=root,
                simulations=19,
                mode=cs.ParallelSearchMode.DETERMINISTIC_EPOCH,
                evaluator=blocked,
                evaluator_version=61,
            )[1]
        except BaseException as error:
            outcome["error"] = error

    thread = threading.Thread(target=search_mutated_root)
    thread.start()
    assert entered.wait(timeout=5)
    assert root.apply_legal_action_index(0, False)
    assert root.board_hash() != original_hash
    release.set()
    thread.join(timeout=8)
    assert not thread.is_alive()
    assert "error" not in outcome

    baseline = _run(
        game=cs.Game(seed=99),
        simulations=19,
        mode=cs.ParallelSearchMode.DETERMINISTIC_EPOCH,
        evaluator_version=61,
    )[1]
    assert _result_digest(outcome["result"]) == _result_digest(baseline)


def test_mcts_config_getters_are_detached_until_explicit_set():
    mcts = cs.MCTS(_config())
    original_cpuct = mcts.config.cpuct
    detached = mcts.config
    detached.cpuct = original_cpuct + 2.0
    assert mcts.config.cpuct == original_cpuct

    mcts.config = detached
    assert mcts.config.cpuct == pytest.approx(original_cpuct + 2.0)

    second = mcts.get_config_snapshot()
    second.cpuct += 3.0
    assert mcts.get_config_snapshot().cpuct == pytest.approx(original_cpuct + 2.0)
    mcts.set_config(second)
    assert mcts.get_config_snapshot().cpuct == pytest.approx(original_cpuct + 5.0)
