"""Public domain provenance-list and storage contracts."""

import pytest

import csplendor as cs


@pytest.mark.parametrize("field", ["acquired_nobles", "purchased_cards"])
def test_provenance_fields_remain_unbounded_ordered_uint8_lists(field):
    player = cs.PlayerState()
    values = [(index * 37) % 256 for index in range(300)]
    setattr(player, field, (value for value in values))

    assert getattr(player, field) == values
    assert getattr(player, field) is not getattr(player, field)

    detached = getattr(player, field)
    detached.append(19)
    assert getattr(player, field) == values


@pytest.mark.parametrize("field", ["acquired_nobles", "purchased_cards"])
def test_provenance_setter_preserves_duplicates_bool_conversion_and_old_value_on_error(
    field,
):
    player = cs.PlayerState()
    setattr(player, field, [11, 0, 11, False, True, 255])
    assert getattr(player, field) == [11, 0, 11, 0, 1, 255]

    for invalid in ([0, -1], [0, 256], [0, 1.5], [0, "1"], [0, None]):
        with pytest.raises(TypeError):
            setattr(player, field, invalid)
        assert getattr(player, field) == [11, 0, 11, 0, 1, 255]


@pytest.mark.parametrize("field", ["acquired_nobles", "purchased_cards"])
@pytest.mark.parametrize("length", [0, 3, 4, 12, 300])
def test_provenance_inline_boundaries_do_not_become_public_limits(field, length):
    player = cs.PlayerState()
    values = [index % 256 for index in range(length)]
    setattr(player, field, values)
    assert getattr(player, field) == values
