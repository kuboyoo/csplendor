import csplendor as cs


def _pack(values):
    return sum(int(value) << (12 * index) for index, value in enumerate(values))


def test_card_packed_costs_match_cost_arrays():
    cards = cs.get_all_cards()
    assert len(cards) == 90
    for card in cards:
        assert card.packed_cost == _pack(card.cost)


def test_noble_packed_requirements_match_requirement_arrays():
    nobles = cs.get_all_nobles()
    assert len(nobles) == 12
    for noble in nobles:
        assert noble.packed_requirement == _pack(noble.requirement)
