import hashlib

import pytest

import csplendor


def assert_same_position(first, second):
    assert second.board_hash() == first.board_hash()
    assert second.current_player == first.current_player
    assert second.turn == first.turn
    assert second.winner == first.winner
    assert second.simple_payment_mode == first.simple_payment_mode
    assert second.blank_refill_mode == first.blank_refill_mode
    assert list(second.legal_action_codes) == list(first.legal_action_codes)
    assert second.board.bank == first.board.bank
    assert second.board.visible == first.board.visible
    assert second.board.decks == first.board.decks
    assert second.board.nobles == first.board.nobles
    assert second.board.final_round == first.board.final_round
    assert second.board.waiting_noble == first.board.waiting_noble
    for player_id in (0, 1):
        expected = first.board.get_player(player_id)
        actual = second.board.get_player(player_id)
        assert actual.gems == expected.gems
        assert actual.bonuses == expected.bonuses
        assert actual.points == expected.points
        assert actual.reserved == expected.reserved
        assert actual.reserved_is_hidden == expected.reserved_is_hidden
        assert actual.reserved_count == expected.reserved_count
        assert actual.purchased_count == expected.purchased_count
        assert actual.purchased_cards == expected.purchased_cards
        assert actual.acquired_nobles == expected.acquired_nobles
    for observer in (0, 1):
        assert csplendor.StateEncoder.encode(
            second, observer
        ) == csplendor.StateEncoder.encode(first, observer)


def test_game_snapshot_round_trips_every_root_without_undo_history():
    game = csplendor.Game(seed=42)
    game.simple_payment_mode = True

    for ply in range(24):
        snapshot = game.serialize_snapshot()
        assert isinstance(snapshot, bytes)
        assert len(snapshot) < 512

        restored = csplendor.Game.deserialize_snapshot(snapshot)
        assert_same_position(game, restored)
        assert restored.serialize_snapshot() == snapshot
        assert not restored.undo()

        if game.is_game_over() or not game.legal_action_codes:
            break
        action_index = (ply * 7 + 3) % len(game.legal_action_codes)
        assert game.apply_action_code_trusted(
            game.legal_action_codes[action_index], True
        )


def test_game_snapshot_preserves_hidden_reservation_and_future_deck_order():
    game = csplendor.Game(seed=7)
    reserve_deck = next(
        action
        for action in game.legal_actions
        if action.type == csplendor.ActionType.RESERVE_DECK
    )
    assert game.apply(reserve_deck, False)

    snapshot = game.serialize_snapshot()
    restored = csplendor.Game.deserialize_snapshot(snapshot)
    assert_same_position(game, restored)

    for ply in range(10):
        if game.is_game_over() or not game.legal_action_codes:
            break
        action_index = (ply * 5 + 1) % len(game.legal_action_codes)
        action_code = game.legal_action_codes[action_index]
        assert restored.is_legal(csplendor.Action.unpack(action_code))
        assert game.apply_action_code_trusted(action_code, False)
        assert restored.apply_action_code_trusted(action_code, False)
        assert_same_position(game, restored)


def test_game_snapshot_rejects_corruption_and_has_a_versioned_golden_encoding():
    snapshot = csplendor.Game(seed=42).serialize_snapshot()
    assert csplendor.Game.snapshot_format_version() == 1
    assert csplendor.Game.snapshot_rules_version() == 1
    assert len(snapshot) == 190
    assert hashlib.sha256(snapshot).hexdigest() == (
        "e9986b8a5db6a7e20ac8a797d0c44d71b08a802e6b059b7ea7f35674c22c360c"
    )

    for broken in (
        snapshot[:-1],
        b"broken" + snapshot[6:],
        snapshot[:30] + bytes([snapshot[30] ^ 1]) + snapshot[31:],
    ):
        with pytest.raises(ValueError):
            csplendor.Game.deserialize_snapshot(broken)

    incompatible_rules = snapshot[:10] + b"\x02\x00" + snapshot[12:]
    with pytest.raises(ValueError, match="rules version"):
        csplendor.Game.deserialize_snapshot(incompatible_rules)


def test_game_snapshot_preserves_editor_visible_history_fields_and_modes():
    game = csplendor.Game(seed=3)
    game.blank_refill_mode = True
    game.board.final_round = True
    game.board.waiting_noble = True
    player = game.board.get_player(0)
    player.gems = [1, 2, 3, 4, 5, 1]
    player.bonuses = [1, 2, 3, 4, 5]
    player.points = 11
    player.reserved = [7, 8, -1]
    player.reserved_is_hidden = [True, False, False]
    player.reserved_count = 2
    player.purchased_count = 3
    player.purchased_cards = [1, 2, 3]
    player.acquired_nobles = [0, 1]
    game.board.set_player(0, player)

    restored = csplendor.Game.deserialize_snapshot(game.serialize_snapshot())
    assert_same_position(game, restored)
