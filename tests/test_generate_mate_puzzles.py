import json

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, parse_kifu_text

import scripts.generate_mate_puzzles as generate_mate_puzzles
from scripts.generate_mate_puzzles import (
    ProgressReporter,
    find_countermate_blunders,
    generate_candidate_position,
    is_suspicious_position,
    report_rejected_position,
    save_puzzle,
)
from scripts.mate_solver import MATE, SearchResult, SearchStats


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
