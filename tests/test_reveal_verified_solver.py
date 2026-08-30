import pytest

import csplendor as cs
from scripts.dfpn_mate_solver import (
    compact_proof_dag_to_v1,
    solve_reveal_verified_mate,
)
from scripts.mate_solver import MATE, SolverOptions, load_game_from_usi_text

BENCH_POSITION = (
    "position bank:W1U3G3R3K0D4 | "
    "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
    "decks:36,23,15 | nobles:[1,10,6] | "
    "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;points:5;"
    "nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
    "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;points:8;"
    "nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
)


def _six_move_mate_fixture():
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [0, 0, 0, 0, 0, 0]
    board.visible = [
        [-1, -1, -1, -1],
        [42, 48, 45, 51],
        [70, 74, -1, -1],
    ]
    board.decks = [[], [], []]
    board.nobles = []
    board.current_player = 0
    board.turn = 0

    for player_index in range(2):
        player = board.get_player(player_index)
        player.gems = [0, 0, 0, 0, 0, 0]
        player.bonuses = (
            [10, 10, 10, 10, 10]
            if player_index == 0
            else [0, 0, 0, 0, 0]
        )
        player.points = 0
        player.acquired_nobles = []
        player.reserved = (
            [-1, -1, -1] if player_index == 0 else [0, 1, 2]
        )
        player.reserved_is_hidden = [False, False, False]
        player.purchased_cards = []
        board.set_player(player_index, player)
    return game


def _seven_move_mate_fixture():
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [0, 0, 0, 0, 0, 0]
    board.visible = [
        [7, -1, -1, -1],
        [42, 43, 44, 48],
        [70, 74, -1, -1],
    ]
    board.decks = [[], [], []]
    board.nobles = []
    board.current_player = 0
    board.turn = 0

    for player_index in range(2):
        player = board.get_player(player_index)
        player.gems = [0, 0, 0, 0, 0, 0]
        player.bonuses = (
            [10, 10, 10, 10, 10]
            if player_index == 0
            else [0, 0, 0, 0, 0]
        )
        player.points = 0
        player.acquired_nobles = []
        player.reserved = (
            [-1, -1, -1] if player_index == 0 else [0, 1, 2]
        )
        player.reserved_is_hidden = [False, False, False]
        player.purchased_cards = []
        board.set_player(player_index, player)
    return game


def _five_move_mate_fixture():
    game = _six_move_mate_fixture()
    visible = game.board.visible
    visible[1][0] = -1
    game.board.visible = visible
    player = game.board.get_player(0)
    player.points = 1
    game.board.set_player(0, player)
    return game


def _newly_revealed_purchase_fixture():
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [0, 0, 0, 0, 0, 0]
    board.visible = [[0, -1, -1, -1], [-1, -1, -1, -1], [-1, -1, -1, -1]]
    board.decks = [[7], [], []]
    board.nobles = []
    board.current_player = 0
    board.turn = 0

    for player_index in range(2):
        player = board.get_player(player_index)
        player.gems = [0, 0, 0, 0, 0, 0]
        player.bonuses = (
            [10, 10, 10, 10, 10]
            if player_index == 0
            else [0, 0, 0, 0, 0]
        )
        player.points = 14 if player_index == 0 else 0
        player.acquired_nobles = []
        player.reserved = (
            [-1, -1, -1] if player_index == 0 else [1, 2, 3]
        )
        player.reserved_is_hidden = [False, False, False]
        player.purchased_cards = []
        board.set_player(player_index, player)
    return game


def test_reveal_verified_solver_proves_known_five_move_mate_within_node_budget():
    result = solve_reveal_verified_mate(
        load_game_from_usi_text(BENCH_POSITION),
        attacker=0,
        options=SolverOptions(
            max_nodes=650000,
            time_limit=10.0,
            include_proof=True,
        ),
    )

    assert result.status == MATE
    assert result.depth == 5
    assert result.stats.nodes < 650000
    assert result.proof_tree["verification"]["all_reveals_verified"] is True
    assert result.proof_tree["verification"]["line"]


def test_reveal_verified_solver_proves_six_move_mate_at_exact_depth():
    game = _six_move_mate_fixture()

    too_shallow = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=0,
        depth=5,
        max_nodes=200000,
        time_limit_seconds=5.0,
        include_proof_dag=False,
    )
    exact = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=0,
        depth=6,
        max_nodes=200000,
        time_limit_seconds=5.0,
        include_proof_dag=False,
    )
    visible = cs.solve_visible_only_winner_cpp(
        game,
        max_nodes=200000,
        time_limit_seconds=5.0,
    )

    assert too_shallow["proven"] is False
    assert exact["proven"] is True
    assert visible["winner"] == 0
    assert visible["forced_win_depth"] == 6


def test_reveal_verified_solver_proves_seven_move_mate_at_exact_depth():
    game = _seven_move_mate_fixture()

    too_shallow = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=0,
        depth=6,
        max_nodes=200000,
        time_limit_seconds=5.0,
        include_proof_dag=False,
    )
    exact = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=0,
        depth=7,
        max_nodes=200000,
        time_limit_seconds=5.0,
        include_proof_dag=False,
    )
    visible = cs.solve_visible_only_winner_cpp(
        game,
        max_nodes=200000,
        time_limit_seconds=5.0,
    )

    assert too_shallow["proven"] is False
    assert exact["proven"] is True
    assert visible["winner"] == 0
    assert visible["forced_win_depth"] == 7


@pytest.mark.parametrize(
    ("depth", "fixture"),
    [
        (5, _five_move_mate_fixture),
        (7, _seven_move_mate_fixture),
    ],
)
def test_parallel_exact_iterative_search_proves_five_and_seven_move_mates(
    depth,
    fixture,
):
    result = cs.search_reveal_verified_mate_depths(
        fixture(),
        attacker=0,
        min_depth=depth - 1,
        max_depth=depth,
        max_nodes=400000,
        time_limit_seconds=5.0,
        jobs=4,
    )

    assert result["status"] == "mate"
    assert result["mate_depth"] == depth
    assert result["verified_no_mate_through_depth"] == depth - 1
    assert all(attempt["parallel"]["exact"] for attempt in result["attempts"])


def test_reveal_verified_depth_search_advances_after_refutation_and_stops_on_mate():
    result = cs.search_reveal_verified_mate_depths(
        _six_move_mate_fixture(),
        attacker=0,
        min_depth=5,
        max_depth=7,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert result["status"] == "mate"
    assert result["stop_reason"] == "mate_proven"
    assert result["mate_depth"] == 6
    assert result["verified_no_mate_through_depth"] == 5
    assert result["last_completed_depth"] == 6
    assert [attempt["status"] for attempt in result["attempts"]] == [
        "no_mate",
        "mate",
    ]


def test_reveal_verified_depth_search_stops_on_unknown_without_skipping_depth():
    result = cs.search_reveal_verified_mate_depths(
        _six_move_mate_fixture(),
        attacker=0,
        min_depth=5,
        max_depth=7,
        max_nodes=1,
        time_limit_seconds=5.0,
    )

    assert result["status"] == "unknown"
    assert result["mate_depth"] is None
    assert result["last_completed_depth"] is None
    assert result["permanent_no_mate_proven"] is False
    assert len(result["attempts"]) == 1
    assert result["attempts"][0]["depth"] == 5
    assert result["attempts"][0]["status"] == "unknown"


def test_reveal_verified_depth_search_stops_on_permanent_score_certificate():
    game = cs.Game(seed=0)
    game.board.visible = [[-1, -1, -1, -1] for _ in range(3)]
    game.board.decks = [[], [], []]
    game.board.nobles = []
    attacker = game.board.get_player(0)
    attacker.points = 14
    game.board.set_player(0, attacker)

    result = cs.search_reveal_verified_mate_depths(
        game,
        attacker=0,
        min_depth=5,
        max_depth=20,
        max_nodes=1,
        time_limit_seconds=0.001,
    )

    assert result["status"] == "permanent_no_mate"
    assert result["permanent_no_mate_proven"] is True
    assert result["permanent_no_mate_certificate"]["score_ceiling"] == 14
    assert result["verified_no_mate_through_depth"] == 20
    assert result["attempts"] == []
    assert result["stats"]["nodes"] == 0


def test_reveal_verified_depth_search_does_not_use_score_certificate_at_15():
    game = cs.Game(seed=0)
    game.board.visible = [[7, -1, -1, -1], [-1] * 4, [-1] * 4]
    game.board.decks = [[], [], []]
    game.board.nobles = []
    attacker = game.board.get_player(0)
    attacker.points = 14
    game.board.set_player(0, attacker)

    result = cs.search_reveal_verified_mate_depths(
        game,
        attacker=0,
        min_depth=5,
        max_depth=20,
        max_nodes=1,
        time_limit_seconds=5.0,
    )

    assert result["status"] == "unknown"
    assert result["permanent_no_mate_proven"] is False
    assert len(result["attempts"]) == 1


def test_reveal_verified_depth_search_validates_required_root_action():
    game = _six_move_mate_fixture()
    action_code = int(game.legal_actions[0].pack())

    result = cs.search_reveal_verified_mate_depths(
        game,
        attacker=0,
        min_depth=5,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
        required_root_action=action_code,
    )

    assert result["required_root_action"] == action_code
    assert result["required_root_action_usi"]
    assert result["attempts"][0]["depth"] == 5

    with pytest.raises(ValueError, match="must be legal"):
        cs.search_reveal_verified_mate_depths(
            game,
            attacker=0,
            min_depth=5,
            max_depth=6,
            required_root_action=2**63,
        )


def test_exact_depth_search_parallelizes_concrete_root_reveals():
    game = _newly_revealed_purchase_fixture()
    game.board.decks = [[7, 15], [], []]
    root_action = next(
        action
        for action in game.legal_actions
        if int(action.type) == int(cs.ActionType.PURCHASE)
        and int(action.card_id) == 0
    )

    sequential = cs.search_reveal_verified_mate_depths(
        game,
        attacker=0,
        min_depth=2,
        max_depth=2,
        max_nodes=100000,
        time_limit_seconds=5.0,
        required_root_action=int(root_action.pack()),
        jobs=1,
    )
    parallel = cs.search_reveal_verified_mate_depths(
        game,
        attacker=0,
        min_depth=2,
        max_depth=2,
        max_nodes=100000,
        time_limit_seconds=5.0,
        required_root_action=int(root_action.pack()),
        jobs=2,
    )

    assert sequential["status"] == parallel["status"] == "mate"
    assert parallel["winning_root_action"] == int(root_action.pack())
    assert parallel["attempts"][0]["parallel"] == {
        "jobs": 2,
        "branches": 2,
        "action_groups": 1,
        "exact": True,
        "exhaustive": True,
        "conclusive_refutations": True,
        "split_ply": 1,
        "positive_portfolio": False,
    }


def test_exact_depth_search_honors_external_cancellation():
    token = cs.MateSearchCancellationToken()
    token.request_cancel()

    result = cs.search_reveal_verified_mate_depths(
        cs.Game(seed=0),
        attacker=0,
        min_depth=1,
        max_depth=3,
        time_limit_seconds=5.0,
        cancellation_token=token,
    )

    assert result["status"] == "unknown"
    assert result["stop_reason"] == "search cancelled"
    assert len(result["attempts"]) == 1


def test_mate_search_session_reuses_descendant_transposition_entries():
    game = _six_move_mate_fixture()
    session = cs.MateSearchSession(attacker=0, jobs=1)

    root = session.search(
        game,
        min_depth=6,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )
    child = game.clone_light()
    assert root["status"] == "mate"
    assert child.apply_action_code(int(root["winning_root_action"]), False)

    continued = session.search(
        child,
        min_depth=5,
        max_depth=5,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert continued["status"] == "mate"
    assert continued["stats"]["reused_memo_hits"] >= 1
    assert continued["stats"]["nodes"] == 1
    assert (
        continued["stats"]["memoized_states_before"]
        == continued["stats"]["memoized_states_after"]
    )


def test_mate_search_session_reuses_position_after_opponent_response():
    game = _six_move_mate_fixture()
    session = cs.MateSearchSession(attacker=0, jobs=1)
    root = session.search(
        game,
        min_depth=6,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )
    line = root["attempts"][0]["line"]

    assert game.apply_action_code(int(line[0]["action_code"]), False)
    assert game.apply_action_code(int(line[1]["action_code"]), False)
    assert int(game.current_player) == 0

    next_turn = session.search(
        game,
        min_depth=5,
        max_depth=5,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert next_turn["status"] == "mate"
    assert next_turn["stats"]["nodes"] == 1
    assert next_turn["stats"]["reused_memo_hits"] == 1


def test_mate_search_session_uses_shallower_result_for_iterative_ordering():
    game = _six_move_mate_fixture()
    session = cs.MateSearchSession(attacker=0, jobs=1)
    assert session.search(
        game,
        min_depth=6,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )["status"] == "mate"

    deeper = session.search(
        game,
        min_depth=7,
        max_depth=7,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert deeper["status"] == "mate"
    assert deeper["stats"]["iterative_order_hits"] > 0


def test_mate_search_session_resumes_incomplete_same_depth_search():
    game = _six_move_mate_fixture()
    session = cs.MateSearchSession(attacker=0, jobs=1)
    interrupted = session.search(
        game,
        min_depth=5,
        max_depth=5,
        max_nodes=500,
        time_limit_seconds=5.0,
    )

    assert interrupted["status"] == "unknown"
    assert interrupted["stats"]["memoized_states_after"] > 0

    resumed = session.search(
        game,
        min_depth=5,
        max_depth=5,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert resumed["status"] == "no_mate_within_max_depth"
    assert resumed["stats"]["reused_memo_hits"] > 0
    assert resumed["stats"]["nodes"] < 10858


def test_mate_search_session_bounds_persistent_cache():
    session = cs.MateSearchSession(
        attacker=0,
        jobs=1,
        max_cache_states=10,
    )

    result = session.search(
        _six_move_mate_fixture(),
        min_depth=5,
        max_depth=5,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert result["stats"]["memoized_states_after"] <= 10
    assert session.memoized_states <= 10


def test_anytime_search_advances_after_inconclusive_depth_without_claiming_refutation():
    result = cs.search_reveal_verified_mate_anytime(
        _six_move_mate_fixture(),
        attacker=0,
        min_depth=5,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
        jobs=2,
    )

    assert result["status"] == "mate"
    assert result["mate_depth"] == 6
    assert result["minimal_depth_proven"] is False
    assert result["verified_no_mate_through_depth"] is None
    assert result["attempts"][0]["status"] == "inconclusive"
    assert result["attempts"][1]["status"] == "mate"
    assert result["winning_root_action_usi"]


def test_session_anytime_mode_uses_cached_descendant_proof():
    game = _six_move_mate_fixture()
    session = cs.MateSearchSession(attacker=0, jobs=2)
    root = session.search(
        game,
        min_depth=6,
        max_depth=6,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )
    child = game.clone_light()
    assert child.apply_action_code(int(root["winning_root_action"]), False)

    continued = session.search_anytime(
        child,
        min_depth=5,
        max_depth=7,
        max_nodes=400000,
        time_limit_seconds=5.0,
    )

    assert continued["status"] == "mate"
    assert continued["mate_depth"] == 5
    assert continued["attempts"][0]["phase"] == "warm_cache"
    assert continued["stats"]["reused_memo_hits"] >= 1


def test_session_anytime_mode_populates_cache_during_live_search():
    session = cs.MateSearchSession(attacker=0, jobs=2)

    result = session.search_anytime(
        _six_move_mate_fixture(),
        min_depth=5,
        max_depth=5,
        max_nodes=400000,
        time_limit_seconds=1.0,
    )

    assert result["status"] == "no_mate_found"
    assert result["stats"]["memoized_states_before"] == 0
    assert result["stats"]["memoized_states_after"] > 0


def test_reveal_verified_solver_can_buy_a_newly_revealed_card():
    game = _newly_revealed_purchase_fixture()

    too_shallow = cs.solve_reveal_verified_mate_cpp(game, attacker=0, depth=1)
    exact = cs.solve_reveal_verified_mate_cpp(game, attacker=0, depth=2)
    line_actions = [
        cs.Action.unpack(int(entry["action_code"]))
        for entry in exact["line"]
    ]

    assert too_shallow["proven"] is False
    assert exact["proven"] is True
    assert any(
        int(action.type) == int(cs.ActionType.PURCHASE)
        and int(action.card_id) == 7
        for action in line_actions
    )


def test_oracle_actions_are_internal_only_for_blank_visible_slots():
    game = cs.Game(seed=0)
    visible = game.board.visible
    visible[0][0] = -1
    game.board.visible = visible
    game.board.current_player = 1

    result = cs.solve_reveal_verified_mate_cpp(game, attacker=0, depth=0)

    assert (
        result["stats"]["oracle_purchase_actions"]
        + result["stats"]["oracle_reserve_actions"]
        > 0
    )


def test_reveal_verified_solver_does_not_emit_oracle_edges():
    result = cs.solve_reveal_verified_mate_cpp(
        load_game_from_usi_text(BENCH_POSITION),
        attacker=0,
        depth=5,
        time_limit_seconds=1.0,
        include_proof_dag=True,
        proof_dag_node_limit=200000,
    )

    dag = result["proof_dag"]
    oracle_edges = [
        edge
        for node in dag["nodes"]
        for edge in node["children"]
        if edge["oracle_card"] is not None or edge["oracle_reserve"]
    ]
    assert oracle_edges == []


def test_reveal_verified_solver_validates_complete_proof_dag():
    game = cs.Game(seed=0)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(1, player)

    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=1,
        depth=1,
        include_proof_dag=True,
        proof_dag_node_limit=1000,
        proof_dag_edge_limit=5000,
    )

    dag = result["proof_dag"]
    assert result["proven"] is True
    assert dag["complete"] is True
    assert dag["validated"] is True
    assert dag["omitted_reason"] is None
    assert all(
        edge["oracle_card"] is None and not edge["oracle_reserve"]
        for node in dag["nodes"]
        for edge in node["children"]
    )


def test_reveal_verified_solver_can_emit_compact_proof_dag():
    game = cs.Game(seed=0)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 14
    player.bonuses = [10, 10, 10, 10, 10]
    game.board.set_player(1, player)

    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=1,
        depth=1,
        include_proof_dag=True,
        proof_dag_node_limit=1000,
        proof_dag_edge_limit=5000,
        proof_dag_format="compact",
    )

    compact = result["proof_dag"]
    dag = compact_proof_dag_to_v1(compact)
    assert compact["format"] == "strategy_dag_compact_v1"
    assert dag["complete"] is True
    assert dag["validated"] is True
    assert all(
        edge["oracle_card"] is None and not edge["oracle_reserve"]
        for node in dag["nodes"]
        for edge in node["children"]
    )


def test_reveal_verified_frontier_expands_only_one_verified_layer():
    game = _six_move_mate_fixture()

    root = cs.solve_reveal_verified_frontier_cpp(
        game,
        attacker=0,
        depth=6,
        max_nodes=200000,
        time_limit_seconds=5.0,
    )

    assert root["proven"] is True
    assert root["complete"] is True
    assert root["player"] == 0
    assert len(root["edges"]) == 1
    root_edge = root["edges"][0]
    assert root_edge["child_depth"] == 5

    child = root_edge["child_game"]
    defender = cs.solve_reveal_verified_frontier_cpp(
        child,
        attacker=0,
        depth=root_edge["child_depth"],
        max_nodes=200000,
        time_limit_seconds=5.0,
    )
    assert defender["proven"] is True
    assert defender["complete"] is True
    assert {
        int(edge["action_code"]) for edge in defender["edges"]
    } == {int(code) for code in child.legal_action_codes}


def test_reveal_verified_frontier_preserves_concrete_deck_reserve_outcome():
    game = _newly_revealed_purchase_fixture()

    result = cs.solve_reveal_verified_frontier_cpp(
        game,
        attacker=0,
        depth=2,
        max_nodes=200000,
        time_limit_seconds=5.0,
    )

    assert result["proven"] is True
    assert result["complete"] is True
    assert len(result["edges"]) == 1
    edge = result["edges"][0]
    assert edge["reveal_card"] == 7
    child = edge["child_game"]
    assert 7 in child.board.get_player(0).reserved


def test_json_mate_frontier_state_round_trip_preserves_phase_fields():
    game = cs.Game(seed=0)
    game.board.current_player = 1
    game.board.turn = 9
    game.board.final_round = True
    game.board.waiting_noble = True

    token = cs.encode_mate_frontier_state(game)
    restored = cs.decode_mate_frontier_state(token)

    assert restored.current_player == 1
    assert restored.turn == 9
    assert restored.board.final_round is True
    assert restored.board.waiting_noble is True


def test_lazy_frontier_reaches_terminal_for_five_and_seven_move_mates():
    for depth, game in (
        (5, _five_move_mate_fixture()),
        (7, _seven_move_mate_fixture()),
    ):
        remaining = depth
        for _ply in range(depth * 2 + 2):
            player = int(game.current_player)
            frontier = cs.solve_reveal_verified_frontier_cpp(
                game,
                attacker=0,
                depth=remaining,
                max_nodes=200000,
                time_limit_seconds=5.0,
                edge_limit=10000,
            )
            assert frontier["proven"] is True
            assert frontier["complete"] is True
            if game.is_game_over():
                break
            if player != 0:
                assert {
                    int(edge["action_code"]) for edge in frontier["edges"]
                } == {int(code) for code in game.legal_action_codes}
            edge = frontier["edges"][0]
            game = edge["child_game"]
            remaining = int(edge["child_depth"])
        assert game.is_game_over()
        assert game.winner == 0
