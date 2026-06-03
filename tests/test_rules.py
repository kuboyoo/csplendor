from csplendor import ActionType, Game, get_noble


def test_max_gems_rule_requires_exact_return_count():
    game = Game(seed=123)
    player = game.board.players[0]
    player.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, player)

    for action in game.legal_actions:
        if action.type not in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
            continue
        total_after_take = sum(player.gems) + sum(action.take)
        assert sum(action.return_gems) == total_after_take - 10
        assert total_after_take - sum(action.return_gems) == 10


def test_reserve_limit_removes_all_reserve_actions():
    game = Game(seed=1)
    player = game.board.players[0]
    player.reserved = [1, 2, 3]
    game.board.set_player(0, player)

    assert game.board.players[0].reserved_count == 3
    assert all(
        action.type not in (ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK)
        for action in game.legal_actions
    )


def test_noble_visit_is_automatic_or_requires_explicit_choice():
    game = Game(seed=5)
    noble_id = int(game.board.nobles[0])
    noble = get_noble(noble_id)

    player = game.board.players[0]
    player.bonuses = [int(v) for v in noble.requirement]
    game.board.set_player(0, player)

    regular_action = next(a for a in game.legal_actions if a.type == ActionType.TAKE_DIFFERENT)
    assert game.apply(regular_action) is True

    if game.board.waiting_noble:
        noble_action = next(
            a for a in game.legal_actions
            if a.type == ActionType.VISIT_NOBLE and int(a.noble_choice) == noble_id
        )
        assert game.apply(noble_action) is True

    player_after = game.board.players[0]
    assert noble_id in [int(n) for n in player_after.acquired_nobles]
    assert noble_id not in [int(n) for n in game.board.nobles]
    assert player_after.points >= 3


def test_trusted_action_code_applies_explicit_noble_choice_points():
    game = Game(seed=5)
    noble_ids = [int(noble_id) for noble_id in game.board.nobles[:2]]
    requirements = [get_noble(noble_id).requirement for noble_id in noble_ids]

    player = game.board.players[0]
    player.bonuses = [
        max(int(requirement[color]) for requirement in requirements)
        for color in range(5)
    ]
    game.board.set_player(0, player)

    regular_action = next(a for a in game.legal_actions if a.type == ActionType.TAKE_DIFFERENT)
    assert game.apply_action_code_trusted(int(regular_action.pack()), False) is True
    assert game.board.waiting_noble is True
    assert game.current_player == 0

    noble_action = next(
        a for a in game.legal_actions
        if a.type == ActionType.VISIT_NOBLE and int(a.noble_choice) == noble_ids[1]
    )
    assert game.apply_action_code_trusted(int(noble_action.pack()), False) is True

    player_after = game.board.players[0]
    assert game.board.waiting_noble is False
    assert game.current_player == 1
    assert int(player_after.points) == int(get_noble(noble_ids[1]).points)
    assert noble_ids[1] in [int(n) for n in player_after.acquired_nobles]
    assert noble_ids[1] not in [int(n) for n in game.board.nobles]


def test_final_round_winner_by_points_after_both_players_move():
    game = Game(seed=9)
    player0 = game.board.players[0]
    player0.points = 15
    game.board.set_player(0, player0)

    assert game.apply(game.legal_actions[0]) is True
    assert not game.is_game_over()
    assert game.current_player == 1

    assert game.apply(game.legal_actions[0]) is True
    assert game.is_game_over()
    assert game.winner == 0
