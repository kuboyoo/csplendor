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
    assert csplendor.Game.snapshot_format_version() == 2
    assert csplendor.Game.snapshot_rules_version() == 2
    assert len(snapshot) == 191
    assert hashlib.sha256(snapshot).hexdigest() == (
        "2e79f11629cfa5a070fb55457f0332e4a2b09657d6bfceed4ccc10034f2ceb1f"
    )

    for broken in (
        snapshot[:-1],
        b"broken" + snapshot[6:],
        snapshot[:30] + bytes([snapshot[30] ^ 1]) + snapshot[31:],
    ):
        with pytest.raises(ValueError):
            csplendor.Game.deserialize_snapshot(broken)

    incompatible_rules = snapshot[:10] + b"\x03\x00" + snapshot[12:]
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


# Version 1 snapshots (rules before the post-deck-reservation return phase),
# written by the previous engine: Game(seed=42), and seed 7 after 23 plies.
V1_INITIAL = bytes.fromhex(
    "4353504c534e5000010001005a000c0001b7ca4404d4b5d49a000000000000000000ff04"
    "04040404050f1c17022d413e334752534d242318161d0522152512131e1f1a030a270708"
    "0e20000b100c0626241921140d1b041109011a3739343c3f2e45402b2c352a2932434442"
    "3b3d28383a2f31303610565554484e58504f514b46594a4c574903080701000000000000"
    "000000000000ffffff00000000000000000000000000000000000000ffffff0000000000"
    "00007b5530a86091f669"
)
V1_MIDGAME = bytes.fromhex(
    "4353504c534e5000010001005a000c0001b7ca4404d4b5d49400000000010b000000ff00"
    "01020200040423241c3f282a2c535646542022171d0801121600201b0a14181e03051013"
    "2509260e15270b0f060c1a2119021734373d4538402b2e414336303a44313e3932353c29"
    "2f3b0e4c4d4852584a5059574b4e4f494703010a070001010003010001000101012d331f"
    "0000000303030d07110004020102010000000000000042515500000003000000dee8efa5"
    "55662152"
)


def test_version_1_snapshots_are_rejected_but_upgrade_exactly():
    with pytest.raises(ValueError, match="upgrade_snapshot_v1"):
        csplendor.Game.deserialize_snapshot(V1_INITIAL)

    upgraded = csplendor.Game.upgrade_snapshot_v1(V1_INITIAL)
    assert upgraded == csplendor.Game(seed=42).serialize_snapshot()

    midgame = csplendor.Game.upgrade_snapshot_v1(V1_MIDGAME)
    restored = csplendor.Game.deserialize_snapshot(midgame)
    assert not restored.board.waiting_return
    assert restored.serialize_snapshot() == midgame
    assert int(restored.turn) == 11 and int(restored.current_player) == 1

    with pytest.raises(ValueError):
        csplendor.Game.upgrade_snapshot_v1(upgraded)  # already version 2


def test_snapshot_preserves_a_pending_return():
    game = csplendor.Game(seed=42)
    player = game.board.get_player(0)
    player.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, player)
    bank = list(game.board.bank)
    game.board.bank = [b - 2 for b in bank[:5]] + [bank[5]]
    deck = next(
        a for a in game.legal_actions if a.type == csplendor.ActionType.RESERVE_DECK
    )
    assert game.apply(deck, False) and game.board.waiting_return
    restored = csplendor.Game.deserialize_snapshot(game.serialize_snapshot())
    assert restored.board.waiting_return and restored.board.pending_decision == 1
    assert restored.board.hash() == game.board.hash()
    assert [a.pack() for a in restored.legal_actions] == [
        a.pack() for a in game.legal_actions
    ]
