"""Independent V3 binding regression oracle retained after the 5D DP trial."""
import itertools

import pytest

from csplendor import ActionEncoderV3 as V3, ActionType, Game, get_card


def _patterns(card):
    cost = list(get_card(card).cost)
    values = itertools.product(*(range(min(int(v), 5) + 1) for v in cost))
    return sorted((p for p in values if sum(p) <= 5), key=lambda p: (sum(p), p))


def test_all_v3_payment_patterns_through_python():
    total = 0
    for card in range(90):
        expected = _patterns(card)
        assert V3.get_card_payment_offset(card) == total
        assert V3.get_card_pattern_count(card) == len(expected)
        assert V3.compute_pattern_count(card) == len(expected)
        for rank, pattern in enumerate(expected):
            assert tuple(V3.decode_payment_for_card(rank, card)) == pattern
            assert V3.encode_payment_for_card(list(pattern), card) == rank
        total += len(expected)
        for invalid in (-2147483648, -1, len(expected), 2147483647):
            assert V3.decode_payment_for_card(invalid, card) == [0] * 5
    assert total == 2035


def test_v3_payment_binding_boundaries_and_vector_contract():
    for card in (-2147483648, -1, 90, 255, 2147483647):
        assert V3.encode_payment_for_card([0] * 5, card) == -1
        assert V3.decode_payment_for_card(0, card) == [0] * 5
        assert V3.compute_pattern_count(card) == -1
    for card in range(90):
        expected = {p: i for i, p in enumerate(_patterns(card))}
        for pos in range(5):
            for value in range(256):
                gold = [0] * 5
                gold[pos] = value
                assert V3.encode_payment_for_card(gold, card) == expected.get(tuple(gold), -1)
        # Existing binding pads short input and truncates after five bytes.
        assert V3.encode_payment_for_card([], card) == 0
        assert V3.encode_payment_for_card([0] * 5 + [255], card) == 0
    for invalid in (-1, 256):
        with pytest.raises(TypeError):
            V3.encode_payment_for_card([invalid, 0, 0, 0, 0], 0)


def test_all_v3_action_ids_keep_payment_order_and_schema():
    game = Game(seed=42)
    expected = [_patterns(card) for card in range(90)]
    assert V3.ACTION_SIZE == 3133
    assert V3.schema_fingerprint() == "csplendor.action.v3;size=3133;layout=840,140,84,21,2035,12,1"
    for action_id in range(V3.ACTION_SIZE):
        action = V3.decode(action_id, game)
        assert V3.encode(action, game) == action_id
        if action.type == ActionType.PURCHASE:
            card = int(action.card_id)
            rank = action_id - V3.OFFSET_PURCHASE - V3.get_card_payment_offset(card)
            assert tuple(action.gold_as) == expected[card][rank]
