"""Public domain provenance-list and storage contracts."""

import pytest

import csplendor as cs

# Inline capacity of acquired_nobles: every noble of the game.
NOBLE_CAPACITY = 12


def test_acquired_nobles_is_an_ordered_uint8_list_up_to_its_capacity():
    player = cs.PlayerState()
    values = [(index * 37) % 256 for index in range(NOBLE_CAPACITY)]
    player.acquired_nobles = (value for value in values)

    assert player.acquired_nobles == values
    assert player.acquired_nobles is not player.acquired_nobles

    detached = player.acquired_nobles
    detached.append(19)
    assert player.acquired_nobles == values

    with pytest.raises(ValueError, match="too long"):
        player.acquired_nobles = values + [0]
    assert player.acquired_nobles == values


def test_acquired_nobles_setter_preserves_duplicates_bool_conversion_and_old_value_on_error():
    player = cs.PlayerState()
    player.acquired_nobles = [11, 0, 11, False, True, 255]
    assert player.acquired_nobles == [11, 0, 11, 0, 1, 255]

    for invalid in ([0, -1], [0, 256], [0, 1.5], [0, "1"], [0, None]):
        with pytest.raises(TypeError):
            player.acquired_nobles = invalid
        assert player.acquired_nobles == [11, 0, 11, 0, 1, 255]


@pytest.mark.parametrize("length", [0, 3, 4, 12])
def test_acquired_nobles_keeps_any_length_within_capacity(length):
    player = cs.PlayerState()
    values = [index % 256 for index in range(length)]
    player.acquired_nobles = values
    assert player.acquired_nobles == values


def test_purchased_cards_is_a_card_id_set_listed_in_ascending_order():
    player = cs.PlayerState()
    player.purchased_cards = (card_id for card_id in [64, 3, False, 89, True])

    assert player.purchased_cards == [0, 1, 3, 64, 89]
    assert player.purchased_cards is not player.purchased_cards

    detached = player.purchased_cards
    detached.append(19)
    assert player.purchased_cards == [0, 1, 3, 64, 89]

    every_card = list(range(89, -1, -1))
    player.purchased_cards = every_card
    assert player.purchased_cards == list(range(90))


def test_purchased_cards_setter_rejects_invalid_ids_and_keeps_old_value():
    player = cs.PlayerState()
    player.purchased_cards = [5, 2]

    for invalid in ([0, 90], [0, 255], [7, 7]):
        with pytest.raises(ValueError):
            player.purchased_cards = invalid
        assert player.purchased_cards == [2, 5]

    for invalid in ([0, -1], [0, 256], [0, 1.5], [0, "1"], [0, None]):
        with pytest.raises(TypeError):
            player.purchased_cards = invalid
        assert player.purchased_cards == [2, 5]
