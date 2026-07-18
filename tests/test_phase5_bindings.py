import gc

import numpy as np

from csplendor import MCTS, Game, MCTSConfig


def _board_value_signature(board):
    return (
        tuple(tuple(level) for level in board.visible),
        tuple(tuple(level) for level in board.decks),
        tuple(board.nobles),
        tuple(
            (
                int(player.points),
                tuple(player.gems),
                tuple(player.bonuses),
                tuple(player.reserved),
                tuple(player.purchased_cards),
                tuple(player.acquired_nobles),
            )
            for player in board.players
        ),
    )


def _batch_mcts():
    config = MCTSConfig()
    config.use_determinization = False
    config.use_dirichlet_noise = False
    return MCTS(config)


def test_board_collection_getters_are_independent_value_copies_and_round_trip():
    game = Game(seed=42)
    board = game.board
    player = board.get_player(0)
    player.points = 7
    player.purchased_cards = [1, 2]
    player.acquired_nobles = [0]
    board.set_player(0, player)
    expected_hash = board.hash()
    expected = _board_value_signature(board)

    visible = board.visible
    decks = board.decks
    nobles = board.nobles
    players = board.players
    assert all(isinstance(value, list) for value in (visible, decks, nobles, players))
    assert visible[0] is not board.visible[0]
    assert decks[0] is not board.decks[0]
    assert players[0] is not board.players[0]

    visible[0][0] = -1
    decks[0].clear()
    nobles.clear()
    players[0].points = 0
    assert _board_value_signature(board) == expected
    assert board.hash() == expected_hash

    board.visible = [list(level) for level in expected[0]]
    board.decks = [list(level) for level in expected[1]]
    board.nobles = list(expected[2])
    for index, copied_player in enumerate(board.players):
        board.set_player(index, copied_player)
    assert _board_value_signature(board) == expected
    assert board.hash() == expected_hash


def test_batch_request_arrays_are_owned_independent_and_applyable():
    mcts = _batch_mcts()
    request = mcts.prepare_batch_simulations(Game(seed=42), 0, 2, 1, None)

    assert set(request) == {
        "flat_boards", "flat_valids", "leaf_world_counts", "leaf_hashes",
        "leaf_paths", "terminals", "total_boards", "num_leaves",
        "tree_generation",
    }
    assert request["num_leaves"] == len(request["leaf_hashes"])
    assert request["num_leaves"] == len(request["leaf_world_counts"])
    assert request["num_leaves"] == len(request["leaf_paths"])
    assert request["total_boards"] == len(request["flat_boards"])
    assert request["total_boards"] == len(request["flat_valids"])
    assert request["total_boards"] == sum(request["leaf_world_counts"])

    for board, valid in zip(request["flat_boards"], request["flat_valids"]):
        assert board.shape == (196,)
        assert board.dtype == np.float32
        assert board.flags["OWNDATA"] and board.flags["WRITEABLE"]
        assert board.flags["C_CONTIGUOUS"]
        assert board.base is None
        assert valid.shape == (48,)
        assert valid.dtype == np.uint8
        assert valid.flags["OWNDATA"] and valid.flags["WRITEABLE"]
        assert valid.flags["C_CONTIGUOUS"]
        assert valid.base is None

    for path in request["leaf_paths"]:
        assert isinstance(path, list)
        assert all(isinstance(entry, tuple) and len(entry) == 3 for entry in path)

    if len(request["flat_boards"]) > 1:
        second_board = request["flat_boards"][1].copy()
        second_valid = request["flat_valids"][1].copy()
        request["flat_boards"][0][0] += 1.0
        request["flat_valids"][0][0] ^= 1
        np.testing.assert_array_equal(request["flat_boards"][1], second_board)
        np.testing.assert_array_equal(request["flat_valids"][1], second_valid)

    retained_board = request["flat_boards"][0]
    retained_snapshot = retained_board.copy()
    del request
    gc.collect()
    np.testing.assert_array_equal(retained_board, retained_snapshot)

    request = mcts.prepare_batch_simulations(Game(seed=42), 0, 2, 1, None)
    policies = [np.ones(48, dtype=np.float32) for _ in request["flat_boards"]]
    values = [np.array([0.2, -0.2], dtype=np.float32) for _ in request["flat_boards"]]
    mcts.apply_batch_results(request, policies, values)
    assert mcts.tree_size() >= 1
