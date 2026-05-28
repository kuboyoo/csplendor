import csplendor as cs
from csplendor.api.usi_kifu import game_to_spn

from scripts.dfpn_mate_solver import DFPNMateSolver, solve_game_dfpn
from scripts import dfpn_mate_solver
from scripts.mate_solver import (
    MATE,
    NO_MATE,
    SolverOptions,
    SolverState,
    load_game_from_usi_text,
    solve_game,
)


def _fast_options(**overrides):
    values = {
        "max_nodes": 10000,
        "time_limit": 1.0,
        "include_proof": False,
    }
    values.update(overrides)
    return SolverOptions(**values)


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


def test_dfpn_collapses_non_dangerous_reveal_cards():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._helper._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    level = int(cs.get_card(int(action.card_id)).level) - 1

    outcomes = solver._transition_outcomes(state, action)

    assert len(outcomes) < len(state.unseen_by_level[level])
    assert solver.stats.safe_reveal_collapses == 1
    assert solver.stats.threat_pruned_reveals > 0


def test_dfpn_splits_root_action_tasks_by_reveal_outcome():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
    solver.use_lazy_reveal_pruning = False
    root = solver._state_node(state, 1)
    solver._expand(root)
    action_child = next(child for child in root.children if child.kind == "action")

    tasks = solver._root_tasks_from_child(0, action_child)

    assert tasks
    assert all(task["kind"] in {"outcome", "state_after_root", "defender_outcome"} for task in tasks)
    assert all(task["group_index"] == 0 for task in tasks)


def test_dfpn_lazy_reveal_starts_with_blank_then_refines():
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
    assert lazy.children[0].outcome.reveal_card is None
    assert solver.stats.lazy_reveal_branches == 1

    lazy.children[0].proof = 0
    lazy.children[0].disproof = dfpn_mate_solver.INF
    solver._update(lazy)

    assert lazy.lazy_reveal_materialized
    assert solver.stats.lazy_reveal_refinements == 1
    assert len(lazy.children) >= 1
    assert all(child.outcome.reveal_card is not None for child in lazy.children)


def test_dfpn_root_parallel_keeps_lazy_reveal_inside_action_task():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())
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

    assert root.lazy_actions_materialized
    assert solver.stats.lazy_action_refinements == 1
    assert len(root.children) == before + omitted


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

    assert root.lazy_actions_materialized
    assert solver.stats.lazy_action_refinements == 1
    assert len(root.children) == before + omitted


def test_dfpn_collapses_take_actions_by_net_token_delta():
    game = load_game_from_usi_text(
        "position bank:W1U3G3R3K0D4 | "
        "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
        "decks:36,23,15 | nobles:[1,10,6] | "
        "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;points:5;"
        "nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
        "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;points:8;"
        "nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
    )
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


def test_dfpn_uses_threat_equivalence_key_when_enabled():
    game = cs.Game(seed=0)
    state = SolverState.from_game(game)
    solver = DFPNMateSolver(attacker=0, max_depth=1, options=_fast_options())

    equivalence_key = solver._state_table_key(state)
    solver.use_equivalence_hash = False
    exact_key = solver._state_table_key(state)

    assert equivalence_key[0] == "threat-v1"
    assert exact_key == solver._helper._canonical_key(state)


def test_dfpn_keeps_proof_and_disproof_numbers_in_stats():
    game = cs.Game(seed=0)
    solver = DFPNMateSolver(attacker=0, max_depth=0, options=_fast_options())
    solver_state = SolverState.from_game(game)

    result = solver.solve(solver_state)

    assert result.stats.root_proof_number >= 0
    assert result.stats.root_disproof_number >= 0
    assert solver_state.game.board.current_player == game.board.current_player
