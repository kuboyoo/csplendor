import csplendor as cs

from scripts.mate_solver import MATE, NO_MATE, MateSolver, SolverOptions, SolverState, solve_game


def _fast_options(**overrides):
    values = {
        "max_nodes": 10000,
        "time_limit": 1.0,
        "include_proof": False,
    }
    values.update(overrides)
    return SolverOptions(**values)


def test_terminal_winner_is_used_for_mate_status():
    game = cs.Game(seed=0)
    game.board.winner = 0

    assert solve_game(game, attacker=0, max_depth=0, options=_fast_options()).status == MATE
    assert solve_game(game, attacker=1, max_depth=0, options=_fast_options()).status == NO_MATE


def test_deck_reserve_is_filtered_by_default():
    game = cs.Game(seed=0)
    state = SolverState.from_game(game)
    solver = MateSolver(attacker=0, max_depth=1, options=_fast_options())

    assert any(int(action.type) == int(cs.ActionType.RESERVE_DECK) for action in game.legal_actions)
    assert all(
        int(action.type) != int(cs.ActionType.RESERVE_DECK)
        for action in solver._legal_actions(state)
    )


def test_visible_reserve_branches_over_every_unseen_card_at_level():
    game = cs.Game(seed=1)
    state = SolverState.from_game(game)
    solver = MateSolver(attacker=0, max_depth=1, options=_fast_options())

    action = next(
        action for action in solver._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )
    level = int(cs.get_card(int(action.card_id)).level) - 1
    outcomes = solver._transition_outcomes(state, action)

    expected_cards = set(int(card_id) for card_id in state.unseen_by_level[level])
    assert {outcome.reveal_card for outcome in outcomes} == expected_cards
    assert len(outcomes) == len(expected_cards)

    for outcome in outcomes:
        assert outcome.reveal_level == level
        assert all(outcome.reveal_card in child.game.board.visible[level] for child in outcome.children)
        assert all(outcome.reveal_card not in child.unseen_by_level[level] for child in outcome.children)


def test_reveal_branching_does_not_mutate_source_state():
    game = cs.Game(seed=2)
    state = SolverState.from_game(game)
    original_visible = [[int(card_id) for card_id in row] for row in state.game.board.visible]
    original_unseen = state.unseen_by_level
    solver = MateSolver(attacker=0, max_depth=1, options=_fast_options())
    action = next(
        action for action in solver._legal_actions(state)
        if int(action.type) == int(cs.ActionType.RESERVE_VISIBLE)
    )

    solver._transition_outcomes(state, action)

    assert [[int(card_id) for card_id in row] for row in state.game.board.visible] == original_visible
    assert state.unseen_by_level == original_unseen
