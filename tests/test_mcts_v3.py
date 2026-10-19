import json
import math
from pathlib import Path

import numpy as np
import pytest

import csplendor as cs


def uniform_apply(session, features, ids, offsets, slots, value=0.0):
    priors = np.zeros(len(ids), dtype=np.float32)
    for row in range(len(slots)):
        priors[offsets[row] : offsets[row + 1]] = 1.0 / (offsets[row + 1] - offsets[row])
    session.apply(ids, offsets, priors, np.full(len(slots), value, dtype=np.float32))


def drive(session, value=0.0):
    rounds = 0
    while not session.all_done():
        features, ids, offsets, slots = session.collect()
        if len(slots) == 0:
            break
        assert features.shape == (len(slots), session.state_dim)
        assert offsets[0] == 0 and offsets[-1] == len(ids)
        assert np.isfinite(features).all()
        uniform_apply(session, features, ids, offsets, slots, value)
        rounds += 1
    return rounds


def test_session_respects_budget_legality_and_determinism():
    config = cs.V3SearchConfig()
    config.num_simulations = 96
    config.leaf_batch_size = 8
    results = []
    for _ in range(2):
        session = cs.V3SearchSession(config)
        games = [cs.Game(seed=seed) for seed in (11, 12, 13)]
        for index, game in enumerate(games):
            assert session.add_game(game, game.current_player, seed=500 + index) == index
        drive(session)
        for index, game in enumerate(games):
            visits = session.root_visits(index)
            mask = cs.ActionEncoderV5.get_action_mask(game)
            assert all(mask[action] for action in visits)
            assert sum(visits.values()) == 95  # root evaluation consumes one
            assert session.simulations(index) == 96
            assert set(session.root_priors(index)) == set(np.flatnonzero(mask).tolist())
        results.append([session.root_visits(i) for i in range(3)])
    assert results[0] == results[1]


def test_leaf_batches_and_stats_are_consistent():
    config = cs.V3SearchConfig()
    config.num_simulations = 40
    config.leaf_batch_size = 4
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=21)
    session.add_game(game, game.current_player, seed=1)
    features, ids, offsets, slots = session.collect()
    assert len(slots) == 1  # only the root until it has priors
    uniform_apply(session, features, ids, offsets, slots)
    features, ids, offsets, slots = session.collect()
    assert 1 <= len(slots) <= 4
    with pytest.raises(RuntimeError):
        session.collect()  # previous batch not applied yet
    uniform_apply(session, features, ids, offsets, slots)
    drive(session)
    stats = session.stats()
    assert stats["simulations"] == 40 == session.simulations(0)
    assert stats["leaves"] + stats["terminals"] + stats["depth_limits"] >= 40
    assert stats["nodes"] == session.node_count(0)


def test_values_flow_back_with_the_right_sign():
    config = cs.V3SearchConfig()
    config.leaf_batch_size = 1
    # Parent-Q FPU and exploration would revisit children before the last
    # unvisited ones; disable both so every child is visited exactly once.
    config.fpu_reduction = 0.0
    config.c_puct = 0.01
    game = cs.Game(seed=7)
    legal = int(cs.ActionEncoderV5.get_action_mask(game).sum())
    # Budget = root evaluation + one visit per root child. Every leaf claims
    # +0.5 for its own side to move; each child is an opponent node, so the
    # root sees -0.5 per child and +0.5 for its own evaluation.
    config.num_simulations = legal + 1
    session = cs.V3SearchSession(config)
    session.add_game(game, game.current_player, seed=3)
    drive(session, value=0.5)
    visits = session.root_visits(0)
    assert len(visits) == legal and all(v == 1 for v in visits.values())
    assert session.root_value(0) == pytest.approx((0.5 - 0.5 * legal) / (legal + 1))
    values = session.root_action_values(0)
    assert all(v == pytest.approx(-0.5) for v in values.values())


def test_root_noise_and_reset_change_priors_but_not_legality():
    config = cs.V3SearchConfig()
    config.num_simulations = 12
    config.leaf_batch_size = 4
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=5)
    session.add_game(game, game.current_player, seed=9, root_noise=True)
    drive(session)
    noisy = session.root_priors(0)
    session.reset_game(0, game, game.current_player, seed=9, root_noise=False)
    drive(session)
    clean = session.root_priors(0)
    assert set(noisy) == set(clean)
    assert any(abs(noisy[a] - clean[a]) > 1e-6 for a in clean)
    assert abs(sum(clean.values()) - 1.0) < 1e-5


def _walk_games(seeds=(1, 2, 3), plies=60):
    for seed in seeds:
        game = cs.Game(seed=seed)
        rng = np.random.default_rng(seed)
        for _ in range(plies):
            if game.is_game_over():
                break
            yield game
            actions = game.legal_actions
            assert game.apply(actions[int(rng.integers(len(actions)))], True)


def test_semantic_groups_follow_the_python_layout():
    assert cs.v3_semantic_group_id(0) == 0
    assert cs.v3_semantic_group_id(839) == 9
    assert cs.v3_semantic_group_id(840) == 10
    assert cs.v3_semantic_group_id(980) == 15
    assert cs.v3_semantic_group_id(1064) == 27
    assert cs.v3_semantic_group_id(1085) == 30
    assert cs.v3_semantic_group_id(3119) == 30 + 89
    assert cs.v3_semantic_group_id(3120) == 120
    assert cs.v3_semantic_group_id(3132) == 132
    # V5 ids (used by V3SearchSession): same groups, returns are 133..138.
    g = cs.v5_semantic_group_id
    assert [g(i) for i in range(980, 992)] == list(range(15, 27))
    assert (g(992), g(994), g(995), g(3029)) == (27, 29, 30, 30 + 89)
    assert g(3030) == 120 and g(3041) == 131
    assert [g(i) for i in range(3042, 3048)] == list(range(133, 139))
    assert g(3048) == 132
    for game in _walk_games():
        for action in game.legal_actions:
            v3_id = cs.ActionEncoderV3.encode(action, game)
            if v3_id >= 0:
                assert g(cs.ActionEncoderV5.encode(action, game)) == cs.v3_semantic_group_id(v3_id)
    config = cs.V3SearchConfig()
    config.num_simulations = 48
    config.semantic_groups = True
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=31)
    session.add_game(game, game.current_player, seed=2)
    drive(session)
    mask = cs.ActionEncoderV5.get_action_mask(game)
    assert all(mask[a] for a in session.root_visits(0))


def test_terminal_and_forced_pass_roots_are_rejected():
    config = cs.V3SearchConfig()
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=1)
    with pytest.raises(ValueError):
        session.add_game(game, 2, seed=1)
    with pytest.raises(IndexError):
        session.done(3)


def test_rollout_phase_follows_the_tree_budget_and_reports_returns():
    config = cs.V3SearchConfig()
    config.num_simulations = 48
    config.leaf_batch_size = 8
    config.rollout_samples = 3
    config.rollout_candidates = 4
    config.rollout_horizon = 5
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=61)
    session.add_game(game, game.current_player, seed=4)
    assert session.rollout_result(0) is None
    tree_rows = 0
    rollout_rows = 0
    while not session.all_done():
        features, ids, offsets, slots = session.collect()
        if len(slots) == 0:
            break
        if session.simulations(0) < 48:
            tree_rows += len(slots)
        else:
            rollout_rows += len(slots)
            assert len(slots) <= 3 * 4
        uniform_apply(session, features, ids, offsets, slots, value=0.25)
    result = session.rollout_result(0)
    assert result is not None
    candidates = list(result["candidates"])
    assert 2 <= len(candidates) <= 4 and len(set(candidates)) == len(candidates)
    visits = session.root_visits(0)
    assert candidates[0] == max(sorted(visits), key=lambda a: visits[a])
    groups = {cs.v5_semantic_group_id(a) for a in candidates}
    assert len(groups) == len(candidates)
    assert result["returns"].shape == (len(candidates), 3)
    assert result["terminal"].shape == (len(candidates), 3)
    assert np.all(np.abs(result["returns"]) <= 1.0)
    # Cutoff values are +-0.25 seen from the root; terminals are +-1 or 0.
    for value, terminal in zip(result["returns"].ravel(), result["terminal"].ravel()):
        assert (abs(value) == 0.25) if not terminal else (value in (-1.0, 0.0, 1.0))
    stats = session.stats()
    assert stats["rollout_games"] == 1 and stats["rollout_rows"] == rollout_rows > 0
    assert tree_rows == 48


def test_rollouts_are_reproducible_and_skipped_without_two_candidates():
    config = cs.V3SearchConfig()
    config.num_simulations = 96
    config.rollout_samples = 2
    config.rollout_horizon = 3
    outputs = []
    for _ in range(2):
        session = cs.V3SearchSession(config)
        game = cs.Game(seed=62)
        session.add_game(game, game.current_player, seed=9)
        drive(session, value=0.1)
        outputs.append(session.rollout_result(0)["returns"].tolist())
    assert outputs[0] == outputs[1]
    config.rollout_min_visits = 10_000  # no second candidate qualifies
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=62)
    session.add_game(game, game.current_player, seed=9)
    drive(session)
    assert session.done(0) and session.rollout_result(0) is None


def test_redeterminization_and_dynamic_cpuct_keep_results_legal_and_reproducible():
    config = cs.V3SearchConfig()
    config.num_simulations = 64
    config.leaf_batch_size = 8
    config.opponent_redeterminization = True
    config.dynamic_cpuct = True
    outputs = []
    for _ in range(2):
        session = cs.V3SearchSession(config)
        game = cs.Game(seed=71)
        session.add_game(game, game.current_player, seed=6)
        drive(session, value=0.3)
        mask = cs.ActionEncoderV5.get_action_mask(game)
        visits = session.root_visits(0)
        assert all(mask[a] for a in visits) and sum(visits.values()) == 63
        outputs.append(visits)
    assert outputs[0] == outputs[1]


def test_tree_reuse_re_roots_along_played_actions():
    config = cs.V3SearchConfig()
    config.num_simulations = 200
    config.leaf_batch_size = 8
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=81)
    observer = game.current_player
    session.add_game(game, observer, seed=3)
    drive(session, value=0.2)
    first = session.root_visits(0)
    own = max(sorted(first), key=lambda a: first[a])
    # Play the observer's best move, then the opponent's most visited reply.
    game.apply(cs.ActionEncoderV5.decode_and_match(own, game))
    reply_mask = cs.ActionEncoderV5.get_action_mask(game)
    reply = int(np.flatnonzero(reply_mask)[0])
    game.apply(cs.ActionEncoderV5.decode_and_match(reply, game))
    assert game.current_player == observer
    reused = session.advance_game(0, game, [own, reply], observer, seed=4)
    # The reply may or may not have been expanded; either way the search
    # must continue from a consistent root with a fresh budget.
    drive(session, value=0.2)
    visits = session.root_visits(0)
    mask = cs.ActionEncoderV5.get_action_mask(game)
    assert all(mask[a] for a in visits)
    assert sum(visits.values()) >= 199
    if reused:
        assert session.reused_visits(0) > 0
        assert sum(visits.values()) == 199 + session.reused_visits(0) - 1 or sum(visits.values()) >= 199
    else:
        assert session.reused_visits(0) == 0
    # A mismatching action sequence always falls back to a fresh tree.
    other = cs.Game(seed=82)
    assert not session.advance_game(0, other, [1, 2], other.current_player, seed=5)
    drive(session)
    assert session.reused_visits(0) == 0


def test_root_visit_floor_forces_compulsory_comparisons():
    config = cs.V3SearchConfig()
    config.num_simulations = 60
    config.leaf_batch_size = 4
    session = cs.V3SearchSession(config)
    game = cs.Game(seed=101)
    session.add_game(game, game.current_player, seed=2)
    features, ids, offsets, slots = session.collect()  # root
    uniform_apply(session, features, ids, offsets, slots)
    legal = np.flatnonzero(cs.ActionEncoderV5.get_action_mask(game)).tolist()
    forced = [legal[-1], legal[-2]]
    session.set_root_visit_floor(0, forced, [15, 10])
    with pytest.raises(ValueError):
        session.set_root_visit_floor(0, [legal[0]], [1000])
    session.set_root_visit_floor(0, forced, [15, 10])
    drive(session)
    visits = session.root_visits(0)
    assert visits[forced[0]] >= 15 and visits[forced[1]] >= 10
    allocated = session.root_visit_floor_allocated(0)
    assert allocated == {forced[0]: 15, forced[1]: 10}
    clean = session.root_clean_priors(0)
    assert set(clean) == set(legal) and abs(sum(clean.values()) - 1) < 1e-5


def _purchase_position(seed=3):
    """A position where the side to move can buy a visible card (reveal)."""
    game = cs.Game(seed=seed)
    rng = np.random.default_rng(seed)
    for _ in range(200):
        actions = game.legal_actions
        if any(a.type == cs.ActionType.PURCHASE and not a.from_reserved for a in actions):
            return game
        game.apply(actions[int(rng.integers(len(actions)))])
        if game.is_game_over():
            game = cs.Game(seed=seed + 1)
    raise AssertionError("no purchase position found")


def test_chance_enumeration_evaluates_every_unseen_reveal_once():
    game = _purchase_position()
    observer = game.current_player
    baseline = cs.V3SearchConfig()
    baseline.num_simulations = 200
    baseline.leaf_batch_size = 8
    enumerated = cs.V3SearchConfig()
    enumerated.num_simulations = 200
    enumerated.leaf_batch_size = 8
    enumerated.chance_enumeration_depth = 1
    enumerated.chance_control_variate = True
    rows = {}
    for name, config in (("baseline", baseline), ("enumerated", enumerated)):
        session = cs.V3SearchSession(config)
        session.add_game(game, observer, seed=7)
        total = 0
        while not session.all_done():
            features, ids, offsets, slots = session.collect()
            if len(slots) == 0:
                break
            assert np.isfinite(features).all()
            total += len(slots)
            uniform_apply(session, features, ids, offsets, slots, 0.1)
        rows[name] = (total, session.stats(), session.simulations(0), sum(session.root_visits(0).values()))
    base_total, base_stats, base_sims, base_visits = rows["baseline"]
    enum_total, enum_stats, enum_sims, enum_visits = rows["enumerated"]
    assert base_stats["chance_enumerations"] == 0
    assert enum_stats["chance_enumerations"] >= 1
    # every enumeration evaluates the whole unseen pool of the tier (<= 36 cards)
    assert enum_stats["chance_rows"] >= enum_stats["chance_enumerations"]
    assert enum_stats["chance_rows"] <= 36 * enum_stats["chance_enumerations"]
    assert enum_total > base_total
    # the budget still counts simulations, not evaluated rows
    assert base_sims == 200 and enum_sims == 200
    assert base_visits == 199 and enum_visits == 199


def test_chance_enumeration_with_risk_weight_prefers_safer_reveals_consistently():
    game = _purchase_position()
    observer = game.current_player
    outcomes = []
    for risk in (0.0, 1.0):
        config = cs.V3SearchConfig()
        config.num_simulations = 120
        config.leaf_batch_size = 8
        config.chance_enumeration_depth = 2
        config.chance_risk_weight = risk
        session = cs.V3SearchSession(config)
        session.add_game(game, observer, seed=9)
        drive(session, 0.05)
        assert session.stats()["chance_enumerations"] >= 1
        outcomes.append(session.root_visits(0))
    # both settings run to completion with a full visit budget
    assert all(sum(visits.values()) == 119 for visits in outcomes)


def _threaded_trace(threads):
    config = cs.V3SearchConfig()
    config.num_simulations = 64
    config.leaf_batch_size = 8
    config.num_threads = threads
    session = cs.V3SearchSession(config)
    games = [cs.Game(seed=seed) for seed in (21, 22, 23, 24, 25)]
    for index, game in enumerate(games):
        session.add_game(game, game.current_player, seed=900 + index)
    batches = []
    while not session.all_done():
        features, ids, offsets, slots = session.collect()
        if len(slots) == 0:
            break
        batches.append((features.tobytes(), ids.tolist(), offsets.tolist(), slots.tolist()))
        uniform_apply(session, features, ids, offsets, slots, 0.25)
    return batches, [session.root_visits(i) for i in range(len(games))], session.stats()


def test_session_threads_reproduce_the_sequential_search():
    assert cs.V3SearchConfig().num_threads == 1
    sequential = _threaded_trace(1)
    for threads in (2, 4):
        assert _threaded_trace(threads) == sequential


def test_session_rejects_non_positive_thread_count():
    config = cs.V3SearchConfig()
    config.num_threads = 0
    with pytest.raises(ValueError):
        cs.V3SearchSession(config)


def _deck_reserve_root(seed=81):
    """The opening position and the observer's tier-1 deck reservation id."""
    game = cs.Game(seed=seed)
    deck = cs.ActionEncoderV5.OFFSET_RESERVE_DECK
    legal = np.flatnonzero(cs.ActionEncoderV5.get_action_mask(game)).tolist()
    reserve = next(a for a in legal if deck <= a < cs.ActionEncoderV5.OFFSET_PURCHASE)
    return game, reserve


def _search_through_deck_reserve(merge, enumeration_depth=0, simulations=121, max_depth=300, widening=(0.0, 0.0)):
    """Every root simulation goes through the observer's deck reservation."""
    config = cs.V3SearchConfig()
    config.max_depth = max_depth
    config.num_simulations = simulations
    config.leaf_batch_size = 4
    config.merge_own_deck_reveal = merge
    config.chance_enumeration_depth = enumeration_depth
    config.chance_widening_base, config.chance_widening_alpha = widening
    game, reserve = _deck_reserve_root()
    session = cs.V3SearchSession(config)
    session.add_game(game, game.current_player, seed=5)
    features, ids, offsets, slots = session.collect()  # root
    uniform_apply(session, features, ids, offsets, slots, 0.1)
    session.set_root_visit_floor(0, [reserve], [simulations - 1])
    drive(session, value=0.1)
    assert session.root_visits(0) == {reserve: simulations - 1}
    return session, game, reserve


def test_merge_own_deck_reveal_shares_the_nodes_below_the_reservation():
    # One ply deep: each simulation evaluates the opponent's node below the
    # reservation once, then stops at the depth limit.
    split, _, _ = _search_through_deck_reserve(merge=False, max_depth=1)
    merged, _, _ = _search_through_deck_reserve(merge=True, max_depth=1)
    # Split: one opponent node per drawn card (the worlds draw different cards).
    assert split.stats()["leaves"] > 2
    # Merged: the root and a single opponent node shared by every world.
    assert merged.stats()["leaves"] == 2


def test_merge_own_deck_reveal_leaves_the_reservation_out_of_chance_enumeration():
    split, _, _ = _search_through_deck_reserve(merge=False, enumeration_depth=1)
    merged, _, _ = _search_through_deck_reserve(merge=True, enumeration_depth=1)
    assert split.stats()["chance_enumerations"] >= 1
    assert merged.stats()["chance_enumerations"] == 0


def test_merge_own_deck_reveal_does_not_reuse_a_tree_across_the_reservation():
    session, game, reserve = _search_through_deck_reserve(merge=True)
    observer = game.current_player
    game.apply(cs.ActionEncoderV5.decode_and_match(reserve, game))
    assert game.current_player != observer  # no token return in the opening
    reply = int(np.flatnonzero(cs.ActionEncoderV5.get_action_mask(game))[0])
    played = [reserve, reply]
    game.apply(cs.ActionEncoderV5.decode_and_match(reply, game))
    assert not session.advance_game(0, game, played, observer, seed=6)
    drive(session)
    assert session.reused_visits(0) == 0


def test_merge_own_deck_reveal_is_off_by_default_and_keeps_the_search():
    assert cs.V3SearchConfig().merge_own_deck_reveal is False
    traces = []
    for explicit in (False, True):
        config = cs.V3SearchConfig()
        config.num_simulations = 96
        config.leaf_batch_size = 8
        if explicit:
            config.merge_own_deck_reveal = False
        session = cs.V3SearchSession(config)
        game = cs.Game(seed=33)
        session.add_game(game, game.current_player, seed=9)
        drive(session, value=0.1)
        traces.append((session.root_visits(0), session.stats()))
    assert traces[0] == traces[1]


GOLDEN_SEARCH = Path(__file__).parent / "data" / "v3_search_golden_aaa9578.json"


def _golden_search(widening=(0.0, 0.0), simulations=64):
    """Root statistics of four fixed searches (three openings, one purchase position)."""
    config = cs.V3SearchConfig()
    config.num_simulations = simulations
    config.leaf_batch_size = 8
    config.chance_widening_base, config.chance_widening_alpha = widening
    session = cs.V3SearchSession(config)
    games = [cs.Game(seed=seed) for seed in (11, 12, 13)] + [_purchase_position()]
    for index, game in enumerate(games):
        session.add_game(game, game.current_player, seed=500 + index)
    drive(session, 0.1)
    rows = []
    for index in range(len(games)):
        visits = {int(k): int(v) for k, v in session.root_visits(index).items()}
        values = {int(k): float(np.float32(v)) for k, v in session.root_action_values(index).items()}
        rows.append({
            "visits": visits,
            "values": values,
            "root_value": float(np.float32(session.root_value(index))),
            "chosen": min(visits, key=lambda a: (-visits[a], a)),
        })
    return rows, {k: int(v) for k, v in session.stats().items()}


def test_chance_widening_is_off_by_default_and_reproduces_the_search_before_it():
    """Default 0/0 must be bit-identical to the search recorded at csplendor aaa9578 (no widening code)."""
    config = cs.V3SearchConfig()
    assert config.chance_widening_base == 0.0 and config.chance_widening_alpha == 0.0
    golden = json.loads(GOLDEN_SEARCH.read_text())
    for widening in ((0.0, 0.0), (1.0, 0.0), (0.0, 0.5)):  # either value 0 disables it
        rows, stats = _golden_search(widening)
        assert stats.pop("chance_widenings") == 0
        assert stats == golden["stats"]
        for row, expected in zip(rows, golden["games"]):
            assert row["visits"] == {int(k): v for k, v in expected["visits"].items()}
            assert row["values"] == {int(k): v for k, v in expected["values"].items()}
            assert row["root_value"] == expected["root_value"]
            assert row["chosen"] == expected["chosen"]


def test_chance_widening_changes_the_search_and_rejects_negative_settings():
    rows, stats = _golden_search((1.0, 0.5), simulations=400)
    assert stats["chance_widenings"] > 0 and stats["simulations"] == 4 * 400
    assert [row["visits"] for row in rows] != [row["visits"] for row in _golden_search(simulations=400)[0]]
    for widening in ((-1.0, 0.5), (1.0, -0.5)):
        config = cs.V3SearchConfig()
        config.chance_widening_base, config.chance_widening_alpha = widening
        with pytest.raises(ValueError):
            cs.V3SearchSession(config)


@pytest.mark.parametrize("widening", [(1.0, 0.5), (1.0, 0.3), (2.0, 0.5)])
def test_chance_widening_caps_the_reveals_of_a_sampled_chance_node(widening):
    # One ply deep: the leaves are the root plus one opponent node per distinct
    # drawn card below the reservation. Without widening the 120 worlds draw
    # far more than k(n) distinct cards (all deck cards can be forced at the
    # opening, so every forced reveal succeeds).
    simulations = 121
    split, _, _ = _search_through_deck_reserve(merge=False, max_depth=1, simulations=simulations)
    widened, _, _ = _search_through_deck_reserve(merge=False, max_depth=1, simulations=simulations, widening=widening)
    base, alpha = widening
    cap = math.ceil(base * (simulations - 2) ** alpha)  # n of the last simulation
    assert 2 < widened.stats()["leaves"] <= 1 + cap < split.stats()["leaves"]
    assert widened.stats()["chance_widenings"] > 0
    assert widened.simulations(0) == simulations


def test_chance_widening_leaves_enumerated_and_merged_chance_nodes_alone():
    enumerated, _, _ = _search_through_deck_reserve(merge=False, enumeration_depth=1, max_depth=1, widening=(1.0, 0.5))
    assert enumerated.stats()["chance_enumerations"] >= 1
    assert enumerated.stats()["chance_widenings"] == 0
    merged, _, _ = _search_through_deck_reserve(merge=True, max_depth=1, widening=(1.0, 0.5))
    assert merged.stats()["leaves"] == 2 and merged.stats()["chance_widenings"] == 0
