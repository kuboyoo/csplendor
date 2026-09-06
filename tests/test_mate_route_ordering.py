from pathlib import Path

import pytest

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi, spn_to_game


POSITION = Path(__file__).parent / "fixtures/mate_routes_bga_910749228_ply50.spn"


def solve(game, *, attacker=0, depth=2, ordering=True, cooperative=False, candidates=False):
    return cs.solve_reveal_verified_mate_cpp(
        game, attacker=attacker, depth=depth, max_nodes=0, time_limit_seconds=10,
        exhaustive_attacker_actions=not candidates, exact_reveal_search=True,
        include_proof_dag=False, use_route_ordering=ordering,
        cooperative_reveals=cooperative,
    )


def chance_fixture():
    game = cs.Game(seed=0)
    game.board.visible = [[7, -1, -1, -1], [-1] * 4, [-1] * 4]
    game.board.decks = [[0, 15], [], []]
    game.board.nobles = []
    game.board.bank = [0, 0, 0, 0, 0, 1]
    for pid in range(2):
        player = game.board.get_player(pid)
        player.gems = [0] * 6
        player.bonuses = [0] * 5 if pid == 0 else [10] * 5
        player.points = 0 if pid == 0 else 14
        game.board.set_player(pid, player)
    return game


def outcomes(game, action):
    level = None
    if action.type == cs.ActionType.RESERVE_DECK:
        level = action.deck_level
    elif action.type == cs.ActionType.RESERVE_VISIBLE or (
        action.type == cs.ActionType.PURCHASE and not action.from_reserved
    ):
        level = next((i for i, row in enumerate(game.board.visible) if action.card_id in row), None)
    draws = list(game.board.decks[level]) if level is not None else []
    for card in draws or [None]:
        child = game.clone()
        if card is not None:
            decks = [list(deck) for deck in child.board.decks]
            decks[level].remove(card)
            decks[level].append(card)
            child.board.decks = decks
        assert child.apply(action, False)
        yield child


def exhaustive_oracle(game, attacker, depth, cooperative):
    """Independent tiny-tree check: no solver memo, ordering or score shortcuts."""
    if game.is_game_over():
        return game.winner == attacker
    if game.current_player == attacker and depth <= 0:
        return False
    results = []
    for action in game.legal_actions:
        children = [
            exhaustive_oracle(
                child, attacker,
                depth - int(game.current_player == attacker and child.current_player != attacker),
                cooperative,
            )
            for child in outcomes(game, action)
        ]
        results.append(any(children) if cooperative else bool(children) and all(children))
    return any(results) if game.current_player == attacker else bool(results) and all(results)


@pytest.mark.parametrize("depth", [1, 2, 3])
def test_bga_ply51_keeps_exact_result_and_reduces_work(depth):
    game = spn_to_game(POSITION.read_text())
    assert game.current_player == 0
    assert [game.board.get_player(p).points for p in range(2)] == [8, 10]
    assert len(game.legal_actions) == 78
    before = cs.encode_mate_frontier_state(game)
    old = solve(game, depth=depth, ordering=False)
    new = solve(game, depth=depth)
    assert old["unknown_reason"] is new["unknown_reason"] is None
    assert old["proven"] is new["proven"] is False
    assert new["stats"]["nodes"] < old["stats"]["nodes"]
    assert cs.encode_mate_frontier_state(game) == before


@pytest.mark.parametrize("cooperative", [False, True])
def test_chance_quantifier_matches_independent_exhaustive_tree(cooperative):
    game = chance_fixture()
    expected = exhaustive_oracle(game, 1, 1, cooperative)
    assert expected is cooperative
    for ordering in [False, True]:
        result = solve(game, attacker=1, depth=1, ordering=ordering, cooperative=cooperative)
        assert result["unknown_reason"] is None
        assert result["proven"] == expected
        if cooperative:
            assert result["reason"] == "cooperative_reveals_verified"


def test_refutation_never_ignores_an_opponent_winning_action():
    game = chance_fixture()
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10] * 5
    game.board.set_player(0, player)
    assert any(action_to_usi(a, game=game).startswith("buy:C7/") for a in game.legal_actions)
    assert exhaustive_oracle(game, 1, 1, True) is False
    result = solve(game, attacker=1, depth=1, cooperative=True)
    assert result["proven"] is False
    assert result["unknown_reason"] is None


def test_single_turn_bound_respects_explicit_final_round_state():
    game = chance_fixture()
    game.board.final_round = True
    game.board.decks = [[], [], []]
    game.board.visible = [[-1] * 4 for _ in range(3)]
    game.board.bank = [1, 1, 1, 1, 1, 1]
    for pid, points in [(0, 14), (1, 10)]:
        player = game.board.get_player(pid)
        player.points = points
        game.board.set_player(pid, player)
    for ordering in [False, True]:
        result = solve(game, depth=1, ordering=ordering)
        assert result["proven"] is True


def test_last_turn_bound_includes_cards_the_opponent_can_reveal():
    game = chance_fixture()
    game.board.visible = [[0, -1, -1, -1], [-1] * 4, [-1] * 4]
    game.board.decks = [[7], [], []]
    game.board.bank = [0] * 6
    player = game.board.get_player(0)
    player.bonuses = list(cs.get_card(0).cost)
    player.reserved = [71, 80, 86]
    game.board.set_player(0, player)
    assert len(game.legal_actions) == 1
    assert action_to_usi(game.legal_actions[0], game=game).startswith("buy:C0/")
    assert exhaustive_oracle(game, 1, 1, False) is True
    for ordering in [False, True]:
        assert solve(game, attacker=1, depth=1, ordering=ordering)["proven"] is True


def test_cooperative_mode_cannot_be_misrepresented_as_a_guaranteed_mate_dag():
    with pytest.raises(ValueError, match="cooperative reveals require"):
        cs.solve_reveal_verified_mate_cpp(
            chance_fixture(), attacker=1, depth=1,
            exhaustive_attacker_actions=True, exact_reveal_search=True,
            cooperative_reveals=True, include_proof_dag=True,
        )


def test_counterstrategy_candidates_cannot_certify_a_negative_result():
    game = chance_fixture()
    player = game.board.get_player(0)
    player.points = 14
    player.bonuses = [10] * 5
    game.board.set_player(0, player)
    result = solve(game, attacker=1, depth=1, cooperative=True, candidates=True)
    assert result["proven"] is False
    assert result["unknown_reason"] == "cooperative_candidate_not_verified"


def test_iterative_progress_is_latest_root_not_a_sum_of_action_codes():
    result = cs.search_reveal_verified_mate_depths(
        spn_to_game(POSITION.read_text()), attacker=0, min_depth=1, max_depth=3,
        max_nodes=0, time_limit_seconds=10,
    )
    last = result["attempts"][-1]["stats"]
    assert last["root_actions"] == 78
    for key in ["root_actions", "root_actions_completed", "root_action"]:
        assert result["stats"][key] == last[key]


@pytest.mark.parametrize("current", [0, 1])
@pytest.mark.parametrize("attacker", [0, 1])
@pytest.mark.parametrize("cooperative", [False, True])
def test_race_shortcuts_match_exhaustive_tiny_endgames(current, attacker, cooperative):
    for points, nobles, depth in [((12, 14), [], 2), ((14, 12), [10], 1), ((15, 15), [], 1)]:
        game = chance_fixture()
        game.board.current_player = current
        game.board.nobles = nobles
        game.board.bank = [0] * 6
        game.board.visible = [[0, 7, -1, -1], [-1] * 4, [-1] * 4]
        game.board.decks = [[15], [], []]
        game.board.final_round = min(points) >= 15
        for pid in range(2):
            player = game.board.get_player(pid)
            player.points = points[pid]
            player.bonuses = [3] * 5
            player.gems = [0, 0, 0, 1, 0, 0]
            game.board.set_player(pid, player)
        expected = exhaustive_oracle(game, attacker, depth, cooperative)
        for ordering in [False, True]:
            result = solve(game, attacker=attacker, depth=depth, ordering=ordering, cooperative=cooperative)
            assert result["unknown_reason"] is None
            assert result["proven"] == expected
