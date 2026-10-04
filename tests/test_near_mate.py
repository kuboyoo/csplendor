
import csplendor as cs
from tests.test_reveal_verified_solver import (
    _five_move_mate_fixture,
    _six_move_mate_fixture,
)


def test_near_mate_matches_exact_proofs_on_forced_lines():
    five = _five_move_mate_fixture()
    assert cs.near_mate_probability_cpp(five, attacker=0, depth=4, time_limit_seconds=10)["value"] == 0.0
    exact = cs.near_mate_probability_cpp(five, attacker=0, depth=5, time_limit_seconds=10)
    assert exact["value"] == 1.0 and exact["complete"]
    proof = cs.search_reveal_verified_mate_depths(five, attacker=0, min_depth=5, max_depth=5, time_limit_seconds=10)
    assert int(exact["best_action"]) == int(proof["winning_root_action"])
    six = _six_move_mate_fixture()
    assert cs.near_mate_probability_cpp(six, attacker=0, depth=5, time_limit_seconds=10)["value"] == 0.0
    assert cs.near_mate_probability_cpp(six, attacker=0, depth=6, time_limit_seconds=10)["value"] == 1.0


def test_near_mate_sweep_brackets_the_probability_and_reports_mates():
    game = _six_move_mate_fixture()
    result = cs.search_near_mate(game, attacker=0, min_depth=3, max_depth=6, time_limit_seconds=20)
    assert result["is_mate"] and result["lower"] == 1.0 and result["upper"] == 1.0
    assert result["lower_depth"] == 6 and result["stop_reason"] == "mate_proven"
    assert result["best_action_usi"] and result["losing_reveals"] == []
    verdicts = {(a["depth"], a["threshold"]): a["verdict"] for a in result["attempts"]}
    assert verdicts[(6, 1.0)] == "proven"
    assert all(v == "refuted" for (d, _t), v in verdicts.items() if d < 6)
    shallow = cs.search_near_mate(game, attacker=0, min_depth=3, max_depth=5, time_limit_seconds=20)
    assert shallow["lower"] == 0.0 and shallow["upper"] == 0.5 and not shallow["is_mate"]
    assert shallow["upper_depth"] == 5 and shallow["stop_reason"] == "max_depth_reached"
    assert all(entry["decided"] for entry in shallow["bounds_by_depth"].values())


def _random_late_position(seed):
    import random

    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    game.simple_payment_mode = False
    while not game.is_game_over():
        actions = game.legal_actions
        if max(int(p.points) for p in game.board.players) >= 11:
            return game
        game.apply(actions[rng.randrange(len(actions))], False)
    return None


def test_near_mate_values_stay_in_unit_interval_and_reveals_are_weighted():
    checked = 0
    for seed in range(1, 40):
        game = _random_late_position(seed)
        if game is None:
            continue
        attacker = int(game.current_player)
        result = cs.near_mate_probability_cpp(game, attacker=attacker, depth=2, time_limit_seconds=5)
        assert 0.0 <= result["value"] <= 1.0
        for item in result["best_action_reveals"]:
            assert 0.0 <= item["value"] <= 1.0 and item["weight"] >= 1
        if result["best_action_reveals"] and result["best_action_reveals"][0]["card"] is not None:
            level = cs.get_card(int(result["best_action_reveals"][0]["card"])).level - 1
            # class weights cover every unseen card of that tier's deck
            assert sum(item["weight"] for item in result["best_action_reveals"]) == len(game.board.decks[level])
            checked += 1
        if checked >= 3:
            break
    assert checked >= 1


def test_near_mate_bounds_the_exact_sweep():
    # Whenever the exact sweep proves a mate at depth d, the probability at d is 1;
    # whenever it refutes depth d conclusively, the probability at d is below 1.
    seen_mate = seen_refuted = False
    for seed in range(1, 60):
        game = _random_late_position(seed)
        if game is None:
            continue
        attacker = int(game.current_player)
        exact = cs.search_reveal_verified_mate_depths(game, attacker=attacker, min_depth=2, max_depth=2, time_limit_seconds=5)
        prob = cs.near_mate_probability_cpp(game, attacker=attacker, depth=2, time_limit_seconds=5)
        if not prob["complete"] or exact["status"] == "unknown":
            continue
        if exact["status"] == "mate":
            assert prob["value"] == 1.0
            seen_mate = True
        elif exact["status"] in ("no_mate_within_max_depth", "permanent_no_mate"):
            assert prob["value"] < 1.0
            seen_refuted = True
        if seen_mate and seen_refuted:
            break
    assert seen_refuted
