import numpy as np
import pytest

from csplendor import ActionEncoderV2, ActionEncoderV3, Game, get_all_cards


def _signature(action):
    return (
        int(action.type),
        tuple(int(v) for v in action.take),
        int(action.card_id),
        int(action.deck_level),
        bool(action.from_reserved),
        tuple(int(v) for v in action.gold_as),
        tuple(int(v) for v in action.return_gems),
        int(action.noble_choice),
    )


def _set_player_gems(game, gems):
    player = game.board.players[game.current_player]
    player.gems = list(gems)
    game.board.set_player(game.current_player, player)


def test_action_encoder_sizes_match_current_cpp_implementation():
    assert ActionEncoderV2.ACTION_SIZE == 4869
    assert ActionEncoderV2.TAKE_DIFF_RETURN_PATTERNS == 84
    assert ActionEncoderV2.TAKE_SAME_RETURN_PATTERNS == 28
    assert ActionEncoderV2.RESERVE_RETURN_PATTERNS == 7
    assert ActionEncoderV2.PURCHASE_PAYMENT_PATTERNS == 252

    assert ActionEncoderV3.ACTION_SIZE == 3133
    assert ActionEncoderV3.TOTAL_PURCHASE == 2035
    assert ActionEncoderV3.OFFSET_PURCHASE == 1085
    assert ActionEncoderV3.OFFSET_VISIT_NOBLE == 3120
    assert ActionEncoderV3.OFFSET_PASS == 3132


@pytest.mark.parametrize("encoder", [ActionEncoderV2, ActionEncoderV3])
@pytest.mark.parametrize(
    "gems",
    [
        [0, 0, 0, 0, 0, 0],
        [2, 2, 2, 1, 1, 0],
        [2, 2, 2, 2, 2, 0],
        [2, 2, 2, 2, 1, 1],
    ],
)
def test_action_masks_cover_every_encoded_legal_action(encoder, gems):
    game = Game(seed=42)
    _set_player_gems(game, gems)

    mask = np.asarray(encoder.get_action_mask(game), dtype=np.uint8)
    assert mask.shape == (encoder.ACTION_SIZE,)
    assert mask.sum() > 0

    encoded_signatures = {}
    for action in game.legal_actions:
        action_id = encoder.encode(action, game)
        assert 0 <= action_id < encoder.ACTION_SIZE
        assert mask[action_id] == 1

        signature = _signature(action)
        assert encoded_signatures.setdefault(action_id, signature) == signature
        assert _signature(encoder.decode_and_match(action_id, game)) == signature

    assert int(mask.sum()) == len(encoded_signatures)


def test_action_encoder_v3_card_payment_tables_are_self_consistent():
    total = 0
    for card in get_all_cards():
        card_id = int(card.id)
        stored_count = ActionEncoderV3.get_card_pattern_count(card_id)
        computed_count = ActionEncoderV3.compute_pattern_count(card_id)
        assert stored_count == computed_count

        seen = set()
        for pattern in range(stored_count):
            gold_as = ActionEncoderV3.decode_payment_for_card(pattern, card_id)
            encoded = ActionEncoderV3.encode_payment_for_card(gold_as, card_id)
            assert encoded == pattern
            seen.add(tuple(gold_as))
        assert len(seen) == stored_count
        total += stored_count

    assert total == ActionEncoderV3.TOTAL_PURCHASE
