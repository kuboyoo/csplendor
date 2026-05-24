import csplendor as cs


def _all_card_ids(board):
    ids = []
    for row in board.visible:
        ids.extend(int(card_id) for card_id in row if int(card_id) >= 0)
    for deck in board.decks:
        ids.extend(int(card_id) for card_id in deck)
    for player in board.players:
        ids.extend(int(card_id) for card_id in player.reserved if int(card_id) >= 0)
        ids.extend(int(card_id) for card_id in player.purchased_cards if int(card_id) >= 0)
    return ids


def assert_card_partition_is_complete(board):
    card_ids = _all_card_ids(board)
    assert len(card_ids) == 90
    assert len(set(card_ids)) == 90
    assert min(card_ids) == 0
    assert max(card_ids) == 89


def test_initial_board_layout_and_resources():
    game = cs.Game(seed=42)
    board = game.board

    assert list(board.bank) == [4, 4, 4, 4, 4, 5]
    assert board.current_player == 0
    assert board.turn == 0
    assert board.winner == -1
    assert len(board.nobles) == 3
    assert [len(deck) for deck in board.decks] == [36, 26, 16]
    assert all(len(row) == 4 for row in board.visible)
    assert all(card_id >= 0 for row in board.visible for card_id in row)
    assert all(player.total_gems() == 0 for player in board.players)
    assert all(player.reserved_count == 0 for player in board.players)
    assert_card_partition_is_complete(board)


def test_apply_and_undo_restore_board_state_and_hash():
    game = cs.Game(seed=7)
    initial_hash = game.board_hash()
    initial_repr = repr(game.board)
    action = game.legal_actions[0]

    assert game.is_legal(action)
    assert game.apply(action) is True
    assert game.board_hash() != initial_hash
    assert repr(game.board) != initial_repr

    assert game.undo() is True
    assert game.board_hash() == initial_hash
    assert repr(game.board) == initial_repr
    assert game.undo() is False
    assert_card_partition_is_complete(game.board)


def test_clone_light_keeps_state_but_drops_history():
    game = cs.Game(seed=11)
    assert game.apply(game.legal_actions[0]) is True

    full_clone = game.clone()
    light_clone = game.clone_light()

    assert full_clone.board_hash() == game.board_hash()
    assert light_clone.board_hash() == game.board_hash()
    assert full_clone.undo() is True
    assert light_clone.undo() is False
