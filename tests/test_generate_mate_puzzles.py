import json

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, parse_kifu_text

from scripts.generate_mate_puzzles import save_puzzle
from scripts.mate_solver import MATE, SearchResult, SearchStats


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
