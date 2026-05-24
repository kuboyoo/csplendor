import csplendor as cs


def test_hidden_reserved_card_randomizes_only_from_opponent_perspective():
    game = cs.Game(seed=42)
    reserve_deck = next(
        action for action in game.legal_actions
        if action.type == cs.ActionType.RESERVE_DECK and action.deck_level == 0
    )
    assert game.apply(reserve_deck) is True

    player0 = game.board.get_player(0)
    assert player0.reserved_count == 1
    assert player0.reserved_is_hidden[0] is True
    original_id = int(player0.reserved[0])
    original_level = cs.get_card(original_id).level
    original_observable_hash = game.board.observable_hash(1)

    randomized_ids = set()
    for seed in range(20):
        clone = game.clone()
        clone.board.randomize_hidden_information(1, seed=seed)
        randomized = clone.board.get_player(0)
        new_id = int(randomized.reserved[0])
        randomized_ids.add(new_id)
        assert randomized.reserved_is_hidden[0] is True
        assert cs.get_card(new_id).level == original_level
        assert clone.board.observable_hash(1) == original_observable_hash

    assert int(game.board.get_player(0).reserved[0]) == original_id
    assert len(randomized_ids) > 1


def test_visible_reserved_card_is_not_randomized():
    game = cs.Game(seed=42)
    assert game.apply(next(a for a in game.legal_actions if a.type == cs.ActionType.TAKE_DIFFERENT))

    reserve_visible = next(a for a in game.legal_actions if a.type == cs.ActionType.RESERVE_VISIBLE)
    visible_card_id = int(reserve_visible.card_id)
    assert game.apply(reserve_visible) is True

    player1 = game.board.get_player(1)
    assert player1.reserved_count == 1
    assert player1.reserved_is_hidden[0] is False
    assert int(player1.reserved[0]) == visible_card_id

    for seed in range(10):
        clone = game.clone()
        clone.board.randomize_hidden_information(0, seed=seed)
        assert int(clone.board.get_player(1).reserved[0]) == visible_card_id
