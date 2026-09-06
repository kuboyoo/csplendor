import hashlib

import pytest

import csplendor
from csplendor.api.usi_kifu import game_to_spn, spn_to_game


def _first_deck_reservation(game):
    return next(
        action
        for action in game.legal_actions
        if action.type == csplendor.ActionType.RESERVE_DECK
    )


def test_information_state_is_invariant_to_hidden_deck_permutations():
    game = csplendor.Game(seed=42)
    expected = game.serialize_information_state(0)

    for seed in range(1, 33):
        world = game.shuffled_clone(0, seed)
        assert world.serialize_information_state(0) == expected
        assert world.information_state_hash(0) == game.information_state_hash(0)


def test_information_state_hides_opponent_reservation_but_not_owner_card():
    game = csplendor.Game(seed=7)
    assert game.apply(_first_deck_reservation(game), False)

    opponent_key = game.serialize_information_state(1)
    owner_key = game.serialize_information_state(0)
    owner_keys = set()
    for seed in range(20, 80):
        world = game.shuffled_clone(1, seed)
        assert world.serialize_information_state(1) == opponent_key
        owner_keys.add(world.serialize_information_state(0))

    assert owner_key in owner_keys
    assert len(owner_keys) > 1


def test_information_state_canonicalizes_visible_and_noble_slot_order():
    game = csplendor.Game(seed=11)
    reordered = game.clone_light()
    reordered.board.visible = [list(reversed(level)) for level in game.board.visible]
    reordered.board.nobles = list(reversed(game.board.nobles))

    for observer in (0, 1):
        assert reordered.serialize_information_state(
            observer
        ) == game.serialize_information_state(observer)
        assert reordered.information_state_hash(
            observer
        ) == game.information_state_hash(observer)


def test_information_state_is_versioned_and_changes_with_public_semantics():
    game = csplendor.Game(seed=3)
    key = game.serialize_information_state(0)

    assert csplendor.Game.information_state_format_version() == 2
    assert csplendor.Game.information_state_rules_version() == 1
    assert key[:8] == b"CSPLINFO"
    assert key != game.serialize_information_state(1)

    changed = game.clone_light()
    changed.board.turn += 1
    assert changed.serialize_information_state(0) != key

    changed = game.clone_light()
    changed.simple_payment_mode = True
    assert changed.serialize_information_state(0) != key

    with pytest.raises(ValueError, match="observer"):
        game.serialize_information_state(2)


def test_information_state_accepts_unknown_usi_purchase_history():
    position = game_to_spn(csplendor.Game(seed=3)).replace(
        "bought:[]", "bought:[_,_]", 1
    )
    game = spn_to_game(position)

    assert game.board.get_player(0).purchased_count == 2
    assert game.board.get_player(0).purchased_cards == []
    assert game.serialize_information_state(0)

    one_unknown = spn_to_game(position.replace("bought:[_,_]", "bought:[_]"))
    assert one_unknown.serialize_information_state(
        0
    ) != game.serialize_information_state(0)


def test_information_state_has_a_golden_encoding():
    key = csplendor.Game(seed=42).serialize_information_state(0)
    assert len(key) < 256
    assert hashlib.sha256(key).hexdigest() == (
        "29f4c45faef7ab9b64f5ab39b0cdb80acc195df32a53bf2d1b4c35670d30c446"
    )
