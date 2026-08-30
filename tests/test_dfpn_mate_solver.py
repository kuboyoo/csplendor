import pytest

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, game_to_spn, parse_kifu_text
from scripts import dfpn_mate_solver
from scripts.dfpn_mate_solver import (
    DFPNMateSolver,
    compact_proof_dag_to_v1,
    principal_line_to_kifu_text,
    proof_dag_to_compact,
    proof_tree_to_kifu_text,
    solve_game_dfpn,
    solve_visible_only_winner,
    strategy_dag_size_report,
)
from scripts.mate_solver import (
    MATE,
    NO_MATE,
    SolverOptions,
    SolverState,
    load_game_from_usi_text,
    solve_game,
)

BENCH_POSITION = (
    "position bank:W1U3G3R3K0D4 | "
    "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
    "decks:36,23,15 | nobles:[1,10,6] | "
    "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;points:5;"
    "nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
    "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;points:8;"
    "nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
)


def _fast_options(**overrides):
    values = {
        "max_nodes": 10000,
        "time_limit": 1.0,
        "include_proof": False,
    }
    values.update(overrides)
    return SolverOptions(**values)


def test_dfpn_proof_tree_serializes_replayable_principal_line_kifu():
    game = cs.Game(seed=0)
    action = game.legal_actions[0]
    usi = action_to_usi(action, game=game)
    proof = {
        "kind": "state",
        "children": [{
            "kind": "action",
            "current_player": 0,
            "action": {"usi": usi},
            "children": [{
                "kind": "outcome",
                "reveal_card": 12,
            }],
        }],
    }

    parsed = parse_kifu_text(proof_tree_to_kifu_text(game, proof, attacker=0))

    assert parsed["position"] == game_to_spn(game)
    assert parsed["moves"] == [{
        "player": 0,
        "usi": usi,
        "comment": "reveal:C12",
    }]
    assert parsed["result"] == "P0_WIN"


def test_solver_line_serializes_kifu_moves():
    game = cs.Game(seed=0)
    usi = action_to_usi(game.legal_actions[0], game=game)

    parsed = parse_kifu_text(principal_line_to_kifu_text(
        game,
        [{"player": 0, "action": {"usi": usi}}],
        attacker=0,
    ))

    assert parsed["moves"] == [{"player": 0, "usi": usi}]


def test_verified_solver_line_serializes_concrete_reveal_card():
    game = cs.Game(seed=0)
    usi = action_to_usi(game.legal_actions[0], game=game)

    parsed = parse_kifu_text(principal_line_to_kifu_text(
        game,
        [{"player": 0, "action": {"usi": usi}, "reveal_card": 12}],
        attacker=0,
    ))

    assert parsed["moves"] == [{
        "player": 0,
        "usi": usi,
        "comment": "reveal:C12",
    }]


def test_compact_strategy_dag_groups_reveals_without_losing_cards():
    reveal_edges = [
        {"action_code": 123, "reveal_card": card, "child": 1}
        for card in range(40)
    ]
    v1 = {
        "format": "strategy_dag_v1",
        "requested": True,
        "complete": True,
        "validated": True,
        "omitted_reason": None,
        "root": 0,
        "nodes": [
            {
                "id": 0,
                "player": 0,
                "depth": 3,
                "kind": "state",
                "resolution": None,
                "children": reveal_edges + [
                    {"action_code": 456, "reveal_card": None, "child": 2}
                ],
            },
            {
                "id": 1,
                "player": 1,
                "depth": 2,
                "kind": "terminal",
                "resolution": "attacker_win",
                "children": [],
            },
            {
                "id": 2,
                "player": 1,
                "depth": 2,
                "kind": "terminal",
                "resolution": "attacker_win",
                "children": [],
            },
        ],
    }

    compact = proof_dag_to_compact(v1)
    expanded = compact_proof_dag_to_v1(compact)
    size = strategy_dag_size_report(v1, compact)

    root_edges = expanded["nodes"][0]["children"]
    recovered_reveals = sorted(
        edge["reveal_card"]
        for edge in root_edges
        if edge["action_code"] == 123
    )
    assert compact["format"] == "strategy_dag_compact_v1"
    assert len(compact["edges"]) == 2
    assert recovered_reveals == list(range(40))
    assert any(edge["reveal_card"] is None for edge in root_edges)
    assert size["compact_json_bytes"] < size["v1_json_bytes"]


def test_compact_strategy_dag_rejects_oracle_edges():
    v1 = {
        "format": "strategy_dag_v1",
        "requested": True,
        "complete": True,
        "validated": False,
        "omitted_reason": None,
        "root": 0,
        "nodes": [
            {
                "id": 0,
                "player": 0,
                "depth": 1,
                "kind": "state",
                "resolution": None,
                "children": [
                    {
                        "action_code": 0,
                        "reveal_card": None,
                        "oracle_card": 68,
                        "oracle_reserve": False,
                        "child": 1,
                    }
                ],
            },
            {
                "id": 1,
                "player": 1,
                "depth": 0,
                "kind": "terminal",
                "resolution": "attacker_win",
                "children": [],
            },
        ],
    }

    with pytest.raises(ValueError, match="non-replayable oracle edge"):
        proof_dag_to_compact(v1)


def test_dfpn_cli_kifu_output_defaults_to_reveal_verified(tmp_path, capsys, monkeypatch):
    output = tmp_path / "mate.kifu"
    expected_game = load_game_from_usi_text(BENCH_POSITION)
    expected_usi = action_to_usi(expected_game.legal_actions[0], game=expected_game)
    calls = []

    def fake_reveal_verified(game, attacker, options=None, **kwargs):
        calls.append((attacker, kwargs))
        return dfpn_mate_solver.SearchResult(
            MATE,
            1,
            {
                "mode": "reveal_verified_mate",
                "attacker": attacker,
                "line": [
                    {
                        "player": int(game.board.current_player),
                        "action": {"usi": expected_usi},
                    }
                ],
            },
            None,
            dfpn_mate_solver.SearchStats(),
        )

    monkeypatch.setattr(
        dfpn_mate_solver,
        "solve_reveal_verified_mate",
        fake_reveal_verified,
    )

    code = dfpn_mate_solver.main([
        "--position",
        BENCH_POSITION,
        "--attacker",
        "0",
        "--kifu-output",
        str(output),
    ])

    parsed = parse_kifu_text(output.read_text(encoding="utf-8"))
    assert code == 0
    assert '"status": "Mate"' in capsys.readouterr().out
    assert calls == [(0, {
        "include_proof_dag": False,
        "proof_dag_node_limit": 100000,
        "proof_dag_edge_limit": 500000,
        "proof_dag_format": "compact",
    })]
    assert [move["usi"] for move in parsed["moves"]] == [expected_usi]
    assert parsed["total_turns"] == 1


def test_dfpn_terminal_winner_is_used_for_mate_status():
    game = cs.Game(seed=0)
    game.board.winner = 0

    assert solve_game_dfpn(game, attacker=0, max_depth=0, options=_fast_options()).status == MATE
    assert solve_game_dfpn(game, attacker=1, max_depth=0, options=_fast_options()).status == NO_MATE


def test_dfpn_loads_same_usi_position_text_as_mate_solver():
    original = cs.Game(seed=4)
    loaded = load_game_from_usi_text(f"position {game_to_spn(original)}")

    result = solve_game_dfpn(loaded, attacker=0, max_depth=0, options=_fast_options())

    assert result.status in (MATE, NO_MATE)
    assert [[int(card_id) for card_id in row] for row in loaded.board.visible] == [
        [int(card_id) for card_id in row] for row in original.board.visible
    ]


def test_dfpn_matches_depth_limited_solver_on_small_branching_state():
    game = cs.Game(seed=0)
    game.board.bank = [0, 0, 0, 0, 0, 0]
    game.board.visible = [
        [0, 1, -1, -1],
        [-1, -1, -1, -1],
        [-1, -1, -1, -1],
    ]
    game.board.decks = [[], [], []]
    game.board.nobles = []
    game.board.current_player = 0

    minimax = solve_game(game, attacker=0, max_depth=1, options=_fast_options())
    dfpn = solve_game_dfpn(
        game,
        attacker=0,
        max_depth=1,
        options=_fast_options(),
        use_upper_bound_pruning=False,
    )

    assert dfpn.status == minimax.status
    assert dfpn.depth == minimax.depth
    assert dfpn.stats.nodes > 0


def test_parallel_dfpn_matches_sequential_on_small_branching_state():
    game = cs.Game(seed=0)
    game.board.bank = [0, 0, 0, 0, 0, 0]
    game.board.visible = [
        [0, 1, -1, -1],
        [-1, -1, -1, -1],
        [-1, -1, -1, -1],
    ]
    game.board.decks = [[], [], []]
    game.board.nobles = []
    game.board.current_player = 0

    sequential = solve_game_dfpn(
        game,
        attacker=0,
        max_depth=1,
        options=_fast_options(jobs=1),
        use_upper_bound_pruning=False,
    )
    parallel = solve_game_dfpn(
        game,
        attacker=0,
        max_depth=1,
        options=_fast_options(jobs=2),
        use_upper_bound_pruning=False,
    )

    assert parallel.status == sequential.status
    assert parallel.depth == sequential.depth
    assert parallel.stats.nodes > 0


def test_dfpn_keeps_every_non_dangerous_reveal_card():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    level = int(cs.get_card(int(action.card_id)).level) - 1

    outcomes = solver._transition_outcomes(state, action)

    assert {outcome.reveal_card for outcome in outcomes} == set(
        state.unseen_by_level[level]
    )
    assert len(outcomes) == len(state.unseen_by_level[level])
    assert solver.stats.safe_reveal_collapses == 0
    assert solver.stats.threat_pruned_reveals == 0


def test_dfpn_orders_but_does_not_collapse_dangerous_reveals():
    game = cs.Game(seed=1)
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(0, player)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
        and int(cs.get_card(int(action.card_id)).level) == 1
    )

    outcomes = solver._transition_outcomes(state, action)

    assert solver.stats.dangerous_reveal_collapses == 0
    assert len(outcomes) == solver.stats.dangerous_reveals
    assert {outcome.reveal_card for outcome in outcomes} == set(
        state.unseen_by_level[0]
    )


def test_dfpn_splits_root_action_tasks_by_reveal_outcome():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    solver.use_lazy_reveal_pruning = False
    solver.use_upper_bound_pruning = False
    root = solver._state_node(state, 1)
    solver._expand(root)
    action_child = next(child for child in root.children if child.kind == "action")

    tasks = solver._root_tasks_from_child(0, action_child)

    assert tasks
    assert all(task["kind"] in {"outcome", "state_after_root", "defender_outcome"} for task in tasks)
    assert all(task["group_index"] == 0 for task in tasks)


def test_dfpn_root_parallel_materializes_omitted_defender_actions(monkeypatch):
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    solver.use_lazy_reveal_pruning = False
    root_action = solver._helper._legal_actions(state)[0]
    defender_game = game.clone_light()
    defender_game.board.current_player = 1
    defender_state = SolverState.from_game(defender_game)
    defender_actions = solver._helper._legal_actions(defender_state)
    assert len(defender_actions) >= 2

    monkeypatch.setattr(
        solver,
        "_transition_outcomes",
        lambda state, action: [dfpn_mate_solver._Outcome(None, None, (defender_state,))],
    )
    monkeypatch.setattr(
        solver,
        "_ordered_actions_with_omissions",
        lambda state, actions, depth: ([defender_actions[0]], [defender_actions[1]]),
    )
    action_child = solver._action_node(state, 1, root_action, actor_is_attacker=True)

    tasks = solver._root_tasks_from_child(0, action_child)

    assert {task["defender_action_code"] for task in tasks} == {
        int(defender_actions[0].pack()),
        int(defender_actions[1].pack()),
    }


def test_dfpn_lazy_reveal_generates_one_concrete_branch_at_a_time():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    action_node = solver._action_node(state, 1, action, actor_is_attacker=True)

    solver._expand(action_node)
    lazy = action_node.children[0]
    assert lazy.kind == "lazy_reveal"
    assert lazy.reveal_candidates

    solver._expand(lazy)
    assert len(lazy.children) == 1
    assert lazy.children[0].outcome.reveal_card is not None
    assert lazy.reveal_next_index == 1
    assert solver.stats.lazy_reveal_branches == 1

    lazy.children[0].proof = 0
    lazy.children[0].disproof = dfpn_mate_solver.INF
    solver._update(lazy)

    assert solver.stats.lazy_reveal_refinements == 1
    assert len(lazy.children) == 2
    assert not lazy.lazy_reveal_materialized
    assert all(child.outcome.reveal_card is not None for child in lazy.children)


def test_dfpn_lazy_reveal_proves_only_after_every_card_is_proven():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action
        for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    lazy = solver._lazy_reveal_node(state, 1, action, actor_is_attacker=True)
    assert lazy is not None
    solver._expand(lazy)

    while not lazy.lazy_reveal_materialized:
        lazy.children[-1].proof = 0
        lazy.children[-1].disproof = dfpn_mate_solver.INF
        solver._update(lazy)
        if not lazy.lazy_reveal_materialized:
            assert lazy.proof > 0

    lazy.children[-1].proof = 0
    lazy.children[-1].disproof = dfpn_mate_solver.INF
    solver._update(lazy)

    assert len(lazy.children) == len(lazy.reveal_candidates)
    assert lazy.proof == 0
    assert lazy.disproof == dfpn_mate_solver.INF


def test_dfpn_refines_defender_depth_zero_reveals_before_proof():
    game = cs.Game(seed=1)
    game.board.current_player = 1
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    action_node = solver._action_node(state, 0, action, actor_is_attacker=False)

    solver._expand(action_node)
    lazy = action_node.children[0]
    solver._expand(lazy)

    lazy.children[0].proof = 0
    lazy.children[0].disproof = dfpn_mate_solver.INF
    solver._update(lazy)

    assert not lazy.lazy_reveal_materialized
    assert solver.stats.lazy_reveal_refinements == 1
    assert len(lazy.children) == 2
    assert all(child.outcome.reveal_card is not None for child in lazy.children)


def test_dfpn_root_parallel_keeps_lazy_reveal_inside_action_task():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    solver.use_upper_bound_pruning = False
    root = solver._state_node(state, 1)
    solver._expand(root)
    action_child = next(
        child
        for child in root.children
        if child.kind == "action" and solver._can_use_lazy_reveal(child.state, child.action)
    )

    tasks = solver._root_tasks_from_child(0, action_child)

    assert len(tasks) == 1
    assert tasks[0]["kind"] == "action"


def test_dfpn_lazy_attacker_actions_refine_before_disproof():
    game = cs.Game(seed=4)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    root = solver._state_node(state, 4)
    solver._expand(root)

    assert root.node_type == "OR"
    assert root.omitted_actions

    for child in root.children:
        child.proof = dfpn_mate_solver.INF
        child.disproof = 0
    before = len(root.children)
    omitted = len(root.omitted_actions)
    solver._update(root)

    assert root.lazy_actions_materialized is (omitted == 1)
    assert solver.stats.lazy_action_refinements == 1
    assert len(root.children) == before + 1
    assert len(root.omitted_actions) == omitted - 1


def test_dfpn_lazy_attacker_actions_disprove_only_after_all_moves_are_read():
    game = cs.Game(seed=4)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    root = solver._state_node(state, 4)
    solver._expand(root)
    assert root.omitted_actions

    while root.omitted_actions:
        for child in root.children:
            child.proof = dfpn_mate_solver.INF
            child.disproof = 0
        solver._update(root)
        if root.omitted_actions:
            assert root.disproof > 0

    for child in root.children:
        child.proof = dfpn_mate_solver.INF
        child.disproof = 0
    solver._update(root)

    assert root.disproof == 0
    assert root.proof == dfpn_mate_solver.INF


def test_dfpn_lazy_defender_actions_refine_before_proof():
    game = cs.Game(seed=0)
    game.board.current_player = 1
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    root = solver._state_node(state, 4)
    solver._expand(root)

    assert root.node_type == "AND"
    assert root.omitted_actions

    for child in root.children:
        child.proof = 0
        child.disproof = dfpn_mate_solver.INF
    before = len(root.children)
    omitted = len(root.omitted_actions)
    solver._update(root)

    assert root.lazy_actions_materialized is (omitted == 1)
    assert solver.stats.lazy_action_refinements == 1
    assert len(root.children) == before + 1
    assert len(root.omitted_actions) == omitted - 1


def test_dfpn_collapses_take_actions_by_net_token_delta():
    game = load_game_from_usi_text(BENCH_POSITION)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    actions = [
        action
        for action in solver._helper._legal_actions(state)
        if int(action.type) in (int(cs.ActionType.TAKE_DIFFERENT), int(cs.ActionType.TAKE_SAME))
    ]
    groups = {}
    for action in actions:
        groups.setdefault(solver._take_net_delta(action), []).append(action)
    equivalent = next(group for group in groups.values() if len(group) > 1)
    collapsed = solver._collapse_equivalent_take_actions(actions)

    action_shapes = {
        (
            int(action.type),
            tuple(solver._fixed_ints(action.take, 6)),
            tuple(solver._fixed_ints(action.return_gems, 6)),
        )
        for action in equivalent
    }
    child_keys = set()
    for action in equivalent:
        child = game.clone_light()
        assert child.apply(action, False)
        child_keys.add(solver._helper._canonical_key(SolverState.from_game(child)))

    assert len(action_shapes) > 1
    assert len(child_keys) == 1
    assert len(collapsed) < len(actions)


def test_dfpn_keeps_distinct_return_patterns_as_separate_actions():
    game = load_game_from_usi_text(BENCH_POSITION)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=5, options=_fast_options())
    actions = solver._helper._legal_actions(state)
    grouped = {}
    for action in actions:
        if int(action.type) != int(cs.ActionType.TAKE_DIFFERENT):
            continue
        take = tuple(solver._fixed_ints(action.take, 6))
        grouped.setdefault(take, []).append(action)
    distinct_returns = next(group for group in grouped.values() if len(group) >= 5)

    targets = solver._target_card_scores(state, 0, 5)
    representatives = solver._representative_payment_and_return_actions(
        state,
        actions,
        0,
        5,
        targets,
    )
    representative_codes = {int(action.pack()) for action in representatives}

    assert {int(action.pack()) for action in distinct_returns}.issubset(
        representative_codes
    )
    child_keys = set()
    for action in distinct_returns:
        child = game.clone_light()
        assert child.apply(action, False)
        child_keys.add(solver._helper._canonical_key(SolverState.from_game(child)))
    assert len(child_keys) == len(distinct_returns)


def test_dfpn_action_cap_defers_instead_of_dropping_legal_moves():
    game = load_game_from_usi_text(BENCH_POSITION)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=5, options=_fast_options())
    solver.use_attacker_dependency_pruning = False
    solver.max_actions_per_node = 2

    ordered, omitted = solver._ordered_actions_with_omissions(
        state,
        solver._helper._legal_actions(state),
        depth=5,
    )

    assert len(ordered) == 2
    assert omitted
    assert {int(action.pack()) for action in ordered}.isdisjoint(
        int(action.pack()) for action in omitted
    )
    assert solver.stats.action_pruned == len(omitted)


def test_dfpn_lazily_enumerates_every_deck_reserve_card():
    game = cs.Game(seed=3)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(
        attacker=0,
        max_depth=2,
        options=_fast_options(allow_deck_reserve=True),
    )
    action = next(
        action
        for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_DECK)
    )
    action_node = solver._action_node(state, 2, action, actor_is_attacker=True)

    solver._expand(action_node)
    lazy = action_node.children[0]
    solver._expand(lazy)

    assert lazy.kind == "lazy_reveal"
    assert set(lazy.reveal_candidates) == set(
        state.unseen_by_level[int(action.deck_level)]
    )
    assert lazy.children[0].outcome.reveal_card in lazy.reveal_candidates


def test_dfpn_limits_dependency_target_candidates():
    game = load_game_from_usi_text(BENCH_POSITION)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    solver.target_candidate_limit = 5
    limited = solver._candidate_target_cards(state, 0)

    solver.target_candidate_limit = 0
    unlimited = solver._candidate_target_cards(state, 0)

    assert len(limited) == 5
    assert set(limited).issubset(set(unlimited))
    assert len(unlimited) > len(limited)


def test_dfpn_defender_take_relevance_requires_exhausting_target_color():
    game = load_game_from_usi_text(BENCH_POSITION)
    game.board.current_player = 1
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=4, options=_fast_options())
    targets = solver._target_card_scores(state, 0, solver._remaining_player_turns(0, 4))
    target_colors = solver._target_dependency_colors(state, 0, targets)
    take_actions = [
        action
        for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.TAKE_DIFFERENT)
    ]
    exhausts_white = next(
        action
        for action in take_actions
        if tuple(solver._fixed_ints(action.take, 6)[:4]) == (1, 1, 1, 0)
    )
    does_not_exhaust = next(
        action
        for action in take_actions
        if tuple(solver._fixed_ints(action.take, 6)[:4]) == (0, 1, 1, 1)
    )

    assert solver._defender_take_exhausts_attacker_target_color(state, exhausts_white, target_colors)
    assert not solver._defender_take_exhausts_attacker_target_color(state, does_not_exhaust, target_colors)


def test_dfpn_move_ordering_prioritizes_high_value_purchase():
    game = cs.Game(seed=0)
    player = game.board.get_player(0)
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(0, player)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    actions = solver._helper._legal_actions(state)

    ordered = solver._ordered_actions(state, actions, depth=1)
    purchases = [action for action in actions if int(action.type) == int(cs.ActionType.PURCHASE)]

    assert purchases
    assert int(ordered[0].type) == int(cs.ActionType.PURCHASE)
    assert solver._card_expected_score(state, 0, int(ordered[0].card_id))[0] == max(
        solver._card_expected_score(state, 0, int(action.card_id))[0]
        for action in purchases
    )


def test_dfpn_cli_accepts_simple_payment(monkeypatch, capsys):
    captured = {}

    def fake_solve(game, attacker, max_depth, options=None, **kwargs):
        captured["simple_payment_mode"] = bool(game.simple_payment_mode)
        return dfpn_mate_solver.SearchResult(
            dfpn_mate_solver.UNKNOWN,
            None,
            None,
            None,
            dfpn_mate_solver.SearchStats(),
        )

    monkeypatch.setattr(dfpn_mate_solver, "solve_game_dfpn", fake_solve)

    code = dfpn_mate_solver.main([
        "--position",
        "position startpos 2",
        "--attacker",
        "0",
        "--max-depth",
        "1",
        "--simple-payment",
    ])

    assert code == 2
    assert captured["simple_payment_mode"] is True
    assert '"status": "Unknown"' in capsys.readouterr().out


def test_dfpn_cli_reveal_depth_range_stops_at_first_mate(monkeypatch, capsys):
    captured = {}

    def fake_depth_search(game, **kwargs):
        captured.update(kwargs)
        return {
            "format": "csplendor_mate_depth_search_v1",
            "status": "mate",
            "stop_reason": "mate_proven",
            "mate_depth": 6,
            "attempts": [
                {"depth": 5, "status": "no_mate"},
                {"depth": 6, "status": "mate"},
            ],
        }

    monkeypatch.setattr(
        dfpn_mate_solver.cs,
        "search_reveal_verified_mate_depths",
        fake_depth_search,
    )

    code = dfpn_mate_solver.main([
        "--position",
        "position startpos 2",
        "--attacker",
        "0",
        "--reveal-verified",
        "--reveal-depth-range",
        "5",
        "8",
        "--required-root-action",
        "take:WUG",
        "--jobs",
        "4",
    ])

    assert code == 0
    assert captured["min_depth"] == 5
    assert captured["max_depth"] == 8
    assert captured["required_root_action"] is not None
    assert captured["jobs"] == 4
    assert '"mate_depth": 6' in capsys.readouterr().out


def test_dfpn_cli_reveal_anytime_uses_positive_proof_search(monkeypatch, capsys):
    captured = {}

    def fake_anytime(game, **kwargs):
        captured.update(kwargs)
        return {
            "format": "csplendor_mate_anytime_search_v1",
            "status": "mate",
            "stop_reason": "mate_proven",
            "mate_depth": 7,
            "winning_root_action": 1,
            "winning_root_action_usi": "take:WUG",
            "attempts": [],
        }

    monkeypatch.setattr(
        dfpn_mate_solver.cs,
        "search_reveal_verified_mate_anytime",
        fake_anytime,
    )

    code = dfpn_mate_solver.main([
        "--position",
        "position startpos 2",
        "--attacker",
        "0",
        "--reveal-verified",
        "--reveal-depth-range",
        "5",
        "8",
        "--reveal-anytime",
        "--jobs",
        "16",
    ])

    assert code == 0
    assert captured["min_depth"] == 5
    assert captured["max_depth"] == 8
    assert captured["jobs"] == 16
    assert '"winning_root_action_usi": "take:WUG"' in capsys.readouterr().out


def test_visible_only_winner_ignores_depth_and_decks():
    game = cs.Game(seed=0)
    game.board.visible = [[7, -1, -1, -1], [-1, -1, -1, -1], [-1, -1, -1, -1]]
    game.board.decks = [[15], [], []]
    game.board.bank = [0, 0, 0, 0, 0, 0]
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(0, player)

    result = solve_visible_only_winner(
        game,
        options=_fast_options(max_nodes=100, time_limit=1.0, include_proof=True),
    )

    assert result.status == dfpn_mate_solver.PLAYER0_WIN
    assert result.depth is None
    assert result.proof_tree["mode"] == "visible_only_winner"
    assert result.proof_tree["assumptions"]["hidden_decks_ignored"] is True
    assert result.proof_tree["assumptions"]["max_depth_ignored"] is True
    assert result.proof_tree["assumptions"]["policy"] == "bounded_forced_win_with_all_defender_responses"
    assert result.proof_tree["assumptions"]["mate_proof"] is True
    assert result.proof_tree["assumptions"]["attacker_candidate_policy"] == "heuristic_subset_for_bounded_proof"
    assert result.proof_tree["assumptions"]["all_visible_only_responses_read"] is True
    assert result.proof_tree["forced_win_depth"] == 1
    assert result.proof_tree["line"]


def test_visible_only_winner_reports_unknown_on_search_limit():
    result = solve_visible_only_winner(
        cs.Game(seed=1),
        options=_fast_options(max_nodes=1, time_limit=1.0, include_proof=True),
    )

    assert result.status == dfpn_mate_solver.UNKNOWN
    assert result.stats.unknown_reason == "node limit exceeded"
    assert result.proof_tree["unknown_reason"] == "node limit exceeded"


def test_visible_only_winner_finds_bench_forced_line():
    result = solve_visible_only_winner(
        load_game_from_usi_text(BENCH_POSITION),
        options=_fast_options(max_nodes=0, time_limit=30.0, include_proof=True),
    )

    assert result.status == dfpn_mate_solver.PLAYER0_WIN
    assert result.proof_tree["assumptions"]["mate_proof"] is True
    assert result.proof_tree["assumptions"]["all_visible_only_responses_read"] is True
    assert result.proof_tree["assumptions"]["purchase_payments"] == "all_legal_patterns"
    assert result.proof_tree["forced_win_depth"] <= 5
    assert result.proof_tree["line"]
    assert result.proof_tree["line"][-1]["scores_after"][0] >= 15
    assert result.stats.elapsed_ms < 30000


def test_visible_only_winner_respects_explicit_simple_payment_mode():
    game = cs.Game(seed=0)
    game.simple_payment_mode = True
    game.board.winner = 0

    result = solve_visible_only_winner(
        game,
        options=_fast_options(include_proof=True),
    )

    assert (
        result.proof_tree["assumptions"]["purchase_payments"]
        == "canonical_minimal_gold_only"
    )


def test_reveal_verified_mate_does_not_emit_oracle_purchase_actions():
    result = cs.solve_reveal_verified_mate_cpp(
        load_game_from_usi_text(BENCH_POSITION),
        attacker=0,
        depth=5,
        time_limit_seconds=1.0,
        include_proof_dag=True,
        proof_dag_node_limit=100000,
    )

    dag = result["proof_dag"]
    assert not any(
        edge["oracle_card"] is not None or edge["oracle_reserve"]
        for node in dag["nodes"]
        for edge in node["children"]
    )


def test_dfpn_cli_visible_only_winner_does_not_require_max_depth(monkeypatch, capsys):
    def fake_visible_only(game, options=None):
        return dfpn_mate_solver.SearchResult(
            dfpn_mate_solver.PLAYER0_WIN,
            None,
            {"mode": "visible_only_winner", "winner": 0},
            None,
            dfpn_mate_solver.SearchStats(),
        )

    monkeypatch.setattr(dfpn_mate_solver, "solve_visible_only_winner", fake_visible_only)

    code = dfpn_mate_solver.main([
        "--position",
        "position startpos 2",
        "--visible-only",
    ])

    assert code == 0
    assert '"status": "Player0Win"' in capsys.readouterr().out


def test_dfpn_uses_exact_slot_independent_position_key_when_enabled():
    game = cs.Game(seed=0)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())

    equivalence_key = solver._state_table_key(state)
    permuted = game.clone_light()
    visible = [list(row) for row in permuted.board.visible]
    visible[0][0], visible[0][1] = visible[0][1], visible[0][0]
    permuted.board.visible = visible
    permuted.board.turn = int(permuted.board.turn) + 7
    permuted_key = solver._state_table_key(SolverState.from_game(permuted))
    solver.use_equivalence_hash = False
    exact_key = solver._state_table_key(state)

    assert equivalence_key[0] == "position-v2"
    assert permuted_key == equivalence_key
    assert exact_key == solver._helper._canonical_key(state)


def test_dfpn_per_call_options_do_not_change_later_solver_defaults():
    defaults = dict(dfpn_mate_solver._DFPN_DEFAULT_PRUNING)
    game = cs.Game(seed=0)
    game.board.winner = 0

    solve_game_dfpn(
        game,
        attacker=0,
        max_depth=1,
        options=_fast_options(),
        use_lazy_reveal_pruning=False,
        use_threat_reveal_pruning=False,
        use_return_pattern_pruning=False,
        use_upper_bound_pruning=False,
        max_actions_per_node=1,
    )
    fresh = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())

    assert dfpn_mate_solver._DFPN_DEFAULT_PRUNING == defaults
    assert fresh.use_lazy_reveal_pruning is defaults["lazy_reveal"]
    assert fresh.use_threat_reveal_pruning is defaults["threat_reveal"]
    assert fresh.use_return_pattern_pruning is defaults["return_pattern"]
    assert fresh.use_upper_bound_pruning is defaults["upper_bound"]
    assert fresh.max_actions_per_node == defaults["max_actions_per_node"]


def test_dfpn_keeps_proof_and_disproof_numbers_in_stats():
    game = cs.Game(seed=0)
    solver = DFPNMateSolver(attacker=0, max_depth=0, options=_fast_options())
    solver_state = SolverState.from_game(game)

    result = solver.solve(solver_state)

    assert result.stats.root_proof_number >= 0
    assert result.stats.root_disproof_number >= 0
    assert solver_state.game.board.current_player == game.board.current_player


def test_dfpn_resolves_forced_final_round_response_without_expanding():
    game = cs.Game(seed=0)
    game.board.final_round = True
    game.board.current_player = 1
    player0 = game.board.get_player(0)
    player0.points = 15
    game.board.set_player(0, player0)
    player1 = game.board.get_player(1)
    player1.points = 0
    game.board.set_player(1, player1)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())

    node = solver._state_node(SolverState.from_game(game), depth=1)

    assert node.terminal
    assert node.terminal_winner == 0
    assert node.reason == "forced_final_round_resolution"
    assert solver.stats.final_round_prunes == 1
