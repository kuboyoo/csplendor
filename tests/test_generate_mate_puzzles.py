import json
from itertools import islice
from types import SimpleNamespace

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, parse_kifu_text, spn_to_game

import scripts.generate_mate_puzzles as generate_mate_puzzles
from scripts.generate_mate_puzzles import (
    ProgressReporter,
    GenerationStats,
    find_countermate_blunders,
    find_verified_winning_actions,
    generate_candidate_position,
    generate_candidate_positions,
    is_tactical_candidate,
    is_suspicious_position,
    report_rejected_position,
    save_puzzle,
    threat_summary,
    try_save_candidate,
)
from scripts.mate_solver import MATE, UNKNOWN, SearchResult, SearchStats


class FirstActionPlayer:
    def __init__(self):
        self.calls = 0
        self.simple_payment_modes = []

    def select_action(self, game):
        self.calls += 1
        self.simple_payment_modes.append(bool(game.simple_payment_mode))
        return game.legal_actions[0]


def test_rejected_position_progress_contains_reason_and_spn(capsys):
    game = cs.Game(seed=0)

    report_rejected_position(
        ProgressReporter(10.0),
        game,
        attempt=3,
        reason="balance_filter",
    )

    output = capsys.readouterr().err
    assert "[progress] stage=rejected attempt=3 reason=balance_filter position=" in output
    assert "bank:W4U4G4R4K4D5" in output


def test_candidate_position_uses_two_players_and_simple_payment_mode():
    players = [FirstActionPlayer(), FirstActionPlayer()]

    game = generate_candidate_position(
        generate_mate_puzzles.random.Random(0),
        players=players,
        game_seed=0,
        min_playout_plies=2,
        max_playout_plies=2,
    )

    assert game is not None
    assert game.simple_payment_mode is False
    assert [player.calls for player in players] == [1, 1]
    assert [player.simple_payment_modes for player in players] == [[True], [True]]


def test_candidate_positions_continue_until_endgame_after_first_candidate():
    players = [FirstActionPlayer(), FirstActionPlayer()]

    games = list(islice(generate_candidate_positions(
        generate_mate_puzzles.random.Random(0),
        players=players,
        game_seed=0,
        min_playout_plies=2,
        max_playout_plies=2,
    ), 3))

    assert len(games) == 3
    assert [int(game.board.current_player) for game in games] == [0, 1, 0]
    assert [bool(game.simple_payment_mode) for game in games] == [False, False, False]
    assert [player.calls for player in players] == [2, 2]


def test_candidate_search_applies_balance_filter_before_mate_search(monkeypatch, tmp_path):
    game = cs.Game(seed=0)
    stats = GenerationStats()
    called = False

    def fake_solve(*args, **kwargs):
        nonlocal called
        called = True
        raise AssertionError("mate search should not run")

    monkeypatch.setattr(generate_mate_puzzles, "solve_reveal_verified_mate", fake_solve)

    mate_found = try_save_candidate(
        SimpleNamespace(
            node_limit=0,
            time_limit=1.0,
            proof_dag_node_limit=100,
            proof_dag_edge_limit=100,
            min_attacker_points=8,
            max_attacker_points=14,
            min_defender_points=8,
            max_score_gap=3,
            allow_final_round=False,
        ),
        output_dir=tmp_path,
        stats=stats,
        progress=ProgressReporter(0),
        game=game,
        game_seed=0,
        attempt=1,
    )

    assert mate_found is False
    assert called is False
    assert stats.mates == 0
    assert stats.balance_filtered == 1
    assert stats.filtered == 1


def test_candidate_search_applies_visible_prefilter_before_verified_search(monkeypatch, tmp_path):
    game = spn_to_game(
        "bank:W3U2G3R4K4D3 | "
        "visible:L1[11,37,7,28]L2[57,42,69,56]L3[79,70,76,72] | "
        "decks:16,16,14 | nobles:[7,4,8] | "
        "P0:gems:W0U0G0R0K0D1;bonuses:W5U1G1R5K4;points:10;"
        "reserved:[66,59,81];bought:[38,27,3,15,17,25,19,14,20,10,62,54,29,46,49,60] | "
        "P1:gems:W1U2G1R0K0D1;bonuses:W2U4G4R1K1;points:8;"
        "reserved:[53];bought:[2,34,36,6,33,0,23,31,50,4,64,82] | 1"
    )
    stats = GenerationStats()
    monkeypatch.setattr(
        generate_mate_puzzles,
        "visible_only_prefilter",
        lambda *args, **kwargs: SearchResult(UNKNOWN, None, None, None, SearchStats()),
    )
    monkeypatch.setattr(
        generate_mate_puzzles,
        "solve_reveal_verified_mate",
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("verified search should not run")),
    )

    mate_found = try_save_candidate(
        SimpleNamespace(
            min_attacker_points=8,
            max_attacker_points=14,
            min_defender_points=8,
            max_score_gap=3,
            allow_final_round=False,
            min_legal_actions=12,
            threat_turns=3,
            min_optimistic_score=15,
            min_threat_score=0,
            require_both_threats=True,
            visible_prefilter=True,
            min_depth=3,
            max_depth=0,
        ),
        output_dir=tmp_path,
        stats=stats,
        progress=ProgressReporter(0),
        game=game,
        game_seed=0,
        attempt=1,
    )

    assert mate_found is False
    assert stats.visible_prefiltered == 1
    assert stats.filtered == 1


def test_candidate_search_stops_playout_after_visible_only_short_mate(monkeypatch, tmp_path):
    game = cs.Game(seed=0)
    stats = GenerationStats()
    monkeypatch.setattr(generate_mate_puzzles, "is_suspicious_position", lambda *args, **kwargs: True)
    monkeypatch.setattr(generate_mate_puzzles, "is_tactical_candidate", lambda *args, **kwargs: True)
    monkeypatch.setattr(
        generate_mate_puzzles,
        "visible_only_prefilter",
        lambda *args, **kwargs: SearchResult(
            generate_mate_puzzles.PLAYER0_WIN,
            1,
            {"forced_win_depth": 1},
            None,
            SearchStats(),
        ),
    )
    monkeypatch.setattr(
        generate_mate_puzzles,
        "solve_reveal_verified_mate",
        lambda *args, **kwargs: (_ for _ in ()).throw(AssertionError("verified search should not run")),
    )

    stop_playout = try_save_candidate(
        SimpleNamespace(
            visible_prefilter=True,
            min_depth=3,
            max_depth=0,
        ),
        output_dir=tmp_path,
        stats=stats,
        progress=ProgressReporter(0),
        game=game,
        game_seed=0,
        attempt=1,
    )

    assert stop_playout is True
    assert stats.visible_prefiltered == 1
    assert stats.filtered == 1


def test_save_puzzle_writes_depth_grouped_answer_and_complete_strategy(tmp_path):
    game = cs.Game(seed=0)
    usi = action_to_usi(game.legal_actions[0], game=game)
    line = [{"player": 0, "action": {"usi": usi}}]
    result = SearchResult(
        MATE,
        3,
        {
            "forced_win_depth": 3,
            "assumptions": {"all_reveal_shapes_verified": True},
            "line": line,
            "verification": {
                "all_reveals_verified": True,
                "reason": "pytest",
                "stats": {"nodes": 1},
                "proof_dag": {
                    "format": "strategy_dag_v1",
                    "complete": True,
                    "root": 0,
                    "nodes": [{"id": 0, "children": []}],
                },
            },
        },
        None,
        SearchStats(nodes=1),
    )

    puzzle_dir = save_puzzle(tmp_path, game, result, game_seed=42, attempt=7)

    assert puzzle_dir is not None
    assert puzzle_dir.parent.name == "depth_03"
    problem = json.loads((puzzle_dir / "problem.json").read_text(encoding="utf-8"))
    strategy = json.loads((puzzle_dir / "strategy.json").read_text(encoding="utf-8"))
    kifu = parse_kifu_text((puzzle_dir / "answer.kifu").read_text(encoding="utf-8"))
    manifest = [
        json.loads(line)
        for line in (tmp_path / "manifest.jsonl").read_text(encoding="utf-8").splitlines()
    ]

    assert problem["format"] == "csplendor_mate_problem_v1"
    assert problem["forced_win_depth"] == 3
    assert strategy["format"] == "csplendor_mate_strategy_v1"
    assert strategy["strategy_dag"]["complete"] is True
    assert kifu["moves"] == [{"player": 0, "usi": usi}]
    assert manifest == [{
        "attacker": 0,
        "forced_win_depth": 3,
        "generated_at": problem["generated_at"],
        "id": problem["id"],
        "initial_scores": [0, 0],
        "path": f"depth_03/{problem['id']}",
    }]

    assert save_puzzle(tmp_path, game, result, game_seed=42, attempt=8) is None


def test_save_puzzle_keeps_exact_hidden_reserved_card_ids_for_replay(tmp_path):
    game = cs.Game(seed=0)
    reserve = next(
        action for action in game.legal_actions
        if action_to_usi(action, game=game).startswith("reserve:L")
    )
    assert game.apply(reserve, False)
    usi = action_to_usi(game.legal_actions[0], game=game)
    result = SearchResult(
        MATE,
        1,
        {
            "forced_win_depth": 1,
            "line": [{"player": int(game.board.current_player), "action": {"usi": usi}}],
            "verification": {
                "proof_dag": {
                    "complete": True,
                    "root": 0,
                    "nodes": [{"id": 0, "children": []}],
                },
            },
        },
        None,
        SearchStats(),
    )

    puzzle_dir = save_puzzle(tmp_path, game, result, game_seed=42, attempt=7)

    problem = json.loads((puzzle_dir / "problem.json").read_text(encoding="utf-8"))
    strategy = json.loads((puzzle_dir / "strategy.json").read_text(encoding="utf-8"))
    kifu = parse_kifu_text((puzzle_dir / "answer.kifu").read_text(encoding="utf-8"))
    assert "?C" in problem["position"]
    assert strategy["position"] == problem["position"]
    assert kifu["position"] == problem["position"]
    reloaded = spn_to_game(problem["position"])
    assert any(bool(value) for value in reloaded.board.get_player(0).reserved_is_hidden)


def test_suspicious_position_requires_close_scores():
    game = cs.Game(seed=0)
    player0 = game.board.get_player(0)
    player0.points = 12
    game.board.set_player(0, player0)
    player1 = game.board.get_player(1)
    player1.points = 10
    game.board.set_player(1, player1)

    assert is_suspicious_position(
        game,
        min_attacker_points=8,
        max_attacker_points=14,
        min_defender_points=8,
        max_score_gap=3,
        allow_final_round=False,
    )
    assert not is_suspicious_position(
        game,
        min_attacker_points=8,
        max_attacker_points=14,
        min_defender_points=8,
        max_score_gap=1,
        allow_final_round=False,
    )


def test_tactical_candidate_requires_both_players_to_have_near_term_score_ceiling():
    game = spn_to_game(
        "bank:W3U2G3R4K4D3 | "
        "visible:L1[11,37,7,28]L2[57,42,69,56]L3[79,70,76,72] | "
        "decks:16,16,14 | nobles:[7,4,8] | "
        "P0:gems:W0U0G0R0K0D1;bonuses:W5U1G1R5K4;points:10;"
        "reserved:[66,59,81];bought:[38,27,3,15,17,25,19,14,20,10,62,54,29,46,49,60] | "
        "P1:gems:W1U2G1R0K0D1;bonuses:W2U4G4R1K1;points:8;"
        "reserved:[53];bought:[2,34,36,6,33,0,23,31,50,4,64,82] | 1"
    )
    args = SimpleNamespace(
        min_attacker_points=8,
        max_attacker_points=14,
        min_defender_points=8,
        max_score_gap=3,
        allow_final_round=False,
        min_legal_actions=12,
        threat_turns=3,
        min_optimistic_score=15,
        min_threat_score=0,
        require_both_threats=True,
    )

    assert threat_summary(game, 0, turns=3).optimistic_score >= 15
    assert threat_summary(game, 1, turns=3).optimistic_score >= 15
    assert is_tactical_candidate(game, args)


def test_verified_winning_action_filter_finds_unique_root_move():
    game = spn_to_game(
        "bank:W3U2G3R4K4D3 | "
        "visible:L1[11,37,7,28]L2[57,42,69,56]L3[79,70,76,72] | "
        "decks:16,16,14 | nobles:[7,4,8] | "
        "P0:gems:W0U0G0R0K0D1;bonuses:W5U1G1R5K4;points:10;"
        "reserved:[66,59,81];bought:[38,27,3,15,17,25,19,14,20,10,62,54,29,46,49,60] | "
        "P1:gems:W1U2G1R0K0D1;bonuses:W2U4G4R1K1;points:8;"
        "reserved:[53];bought:[2,34,36,6,33,0,23,31,50,4,64,82] | 1"
    )

    result = find_verified_winning_actions(
        game,
        attacker=1,
        depth=3,
        max_winning_actions=1,
        node_limit=0,
        time_limit=5.0,
    )

    assert result["complete"] is True
    assert result["checks"] == len(game.legal_actions)
    assert result["winning_actions"] == ["buy:C28/pay:W1U0G0R0K0D0"]


def test_countermate_filter_accepts_wrong_move_that_allows_opponent_mate(monkeypatch):
    game = cs.Game(seed=0)
    correct = game.legal_actions[0]
    result = SearchResult(
        MATE,
        1,
        {"line": [{"player": 0, "action": {"pack": int(correct.pack())}}]},
        None,
        SearchStats(),
    )

    def fake_solve(child, attacker, options):
        assert int(child.board.current_player) == attacker == 1
        return SearchResult(MATE, 2, {}, None, SearchStats())

    monkeypatch.setattr(generate_mate_puzzles, "solve_reveal_verified_mate", fake_solve)

    blunders, checks = find_countermate_blunders(
        game,
        result,
        min_losing_alternatives=1,
        action_limit=1,
        node_limit=0,
        time_limit=1.0,
    )

    assert checks == 1
    assert len(blunders) == 1
    assert blunders[0]["opponent"] == 1
    assert blunders[0]["forced_win_depth"] == 2
