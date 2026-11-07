import pytest

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, parse_kifu_text
from scripts import dfpn_mate_solver
from scripts.dfpn_mate_solver import (
    compact_proof_dag_to_v1,
    principal_line_to_kifu_text,
    proof_dag_to_compact,
    solve_visible_only_winner,
    strategy_dag_size_report,
)
from scripts.mate_solver import (
    MATE,
    SolverOptions,
    load_game_from_usi_text,
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


def test_cli_accepts_simple_payment(monkeypatch, capsys):
    captured = {}

    def fake_solve(game, attacker, options=None, **kwargs):
        captured["simple_payment_mode"] = bool(game.simple_payment_mode)
        return dfpn_mate_solver.SearchResult(
            dfpn_mate_solver.UNKNOWN,
            None,
            None,
            None,
            dfpn_mate_solver.SearchStats(),
        )

    monkeypatch.setattr(dfpn_mate_solver, "solve_reveal_verified_mate", fake_solve)

    code = dfpn_mate_solver.main([
        "--position",
        "position startpos 2",
        "--attacker",
        "0",
        "--simple-payment",
    ])

    assert code == 2
    assert captured["simple_payment_mode"] is True
    assert '"status": "Unknown"' in capsys.readouterr().out


def _fast_options(**overrides):
    values = {
        "max_nodes": 10000,
        "time_limit": 1.0,
        "include_proof": False,
    }
    values.update(overrides)
    return SolverOptions(**values)


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


def test_cli_kifu_output_writes_the_reveal_verified_line(tmp_path, capsys, monkeypatch):
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


def test_cli_reveal_depth_range_stops_at_first_mate(monkeypatch, capsys):
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


def test_cli_reveal_anytime_uses_positive_proof_search(monkeypatch, capsys):
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


def test_cli_visible_only_winner(monkeypatch, capsys):
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


