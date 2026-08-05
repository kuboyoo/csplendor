"""Cross-phase review tests for legal-action and encoder contracts."""

import random

import numpy as np
import pytest

from csplendor import (
    Action,
    ActionEncoderCpp,
    ActionEncoderV2,
    ActionEncoderV3,
    ActionType,
    Game,
    get_card,
)
from tests.support import (
    action_signature as _signature,
)
from tests.support import (
    set_current_player as _set_player,
)


@pytest.mark.parametrize(
    ("bank", "expected_combo"),
    [
        ([0, 0, 0, 0, 1, 0], 2),
        ([0, 0, 1, 0, 1, 0], 4),
    ],
)
def test_depleted_bank_take_variants_have_stable_full_encoder_ids(
    bank, expected_combo
):
    game = Game(seed=42)
    game.board.bank = bank
    _set_player(
        game,
        gems=[2, 2, 2, 2, 2, 0],
        reserved=[1, 2, 3],
    )

    takes = [
        action
        for action in game.legal_actions
        if action.type == ActionType.TAKE_DIFFERENT
    ]
    assert takes

    for encoder in (ActionEncoderV2, ActionEncoderV3):
        ids = [encoder.encode(action, game) for action in takes]
        expected_start = expected_combo * 84
        assert all(
            expected_start <= action_id < expected_start + 84
            for action_id in ids
        )
        assert len(ids) == len(set(ids))
        mask = np.asarray(encoder.get_action_mask(game), dtype=np.uint8)
        assert all(mask[action_id] == 1 for action_id in ids)
        for action_id, action in zip(ids, takes):
            assert _signature(encoder.decode_and_match(action_id, game)) == _signature(
                action
            )


def test_v3_rejects_payment_components_above_card_cost():
    game = Game(seed=42)
    for card_id in range(90):
        cost = list(map(int, get_card(card_id).cost))
        color = next(index for index, value in enumerate(cost) if value < 5)
        gold_as = [0] * 5
        gold_as[color] = cost[color] + 1

        assert ActionEncoderV3.encode_payment_for_card(gold_as, card_id) == -1

        action = Action()
        action.type = ActionType.PURCHASE
        action.card_id = card_id
        action.gold_as = gold_as
        assert ActionEncoderV3.encode(action, game) == -1


def test_terminal_game_has_no_rule_actions_or_policy_sentinel():
    game = Game(seed=42)
    formerly_legal = game.legal_actions[0]
    game.board.winner = 0

    assert game.is_game_over()
    assert game.legal_action_count == 0
    assert game.legal_actions == []
    assert game.legal_action_codes == []
    assert game.legal_action_code_at(0) == 0
    assert game.is_legal(formerly_legal) is False
    assert game.apply_legal_action_index(0, False) is False
    assert game.apply_random_action(0, False) is False

    assert int(np.asarray(ActionEncoderCpp.get_action_mask(game)).sum()) == 0
    for encoder in (ActionEncoderV2, ActionEncoderV3):
        active = np.flatnonzero(encoder.get_action_mask(game)).tolist()
        assert active == []


def test_over_token_editor_purchase_generation_matches_apply_contract():
    game = Game(seed=1)
    game.board.bank = [0, 0, 0, 0, 0, 0]
    game.board.visible = [[86, 86, 86, 86] for _ in range(3)]
    _set_player(
        game,
        gems=[5, 3, 0, 3, 3, 14],
        bonuses=[0, 0, 0, 0, 0],
        reserved=[86, 86, 86],
    )

    actions = game.legal_actions
    assert len(actions) == 2048
    assert all(action.type == ActionType.PURCHASE for action in actions)
    assert all(tuple(map(int, action.return_gems)) == (0, 0, 0, 0, 0, 0)
               for action in actions)

    for action in (actions[0], actions[len(actions) // 2], actions[-1]):
        clone = game.clone_light()
        assert clone.apply(action, False)
        assert _signature(Action.unpack(int(action.pack()))) == _signature(action)


def test_seeded_playouts_keep_pack_apply_and_full_encoder_parity():
    rng = random.Random(0xC5EED)
    checked = 0

    for seed in range(20):
        game = Game(seed=seed)
        for _ in range(50):
            if game.is_game_over():
                break

            actions = game.legal_actions
            codes = list(map(int, game.legal_action_codes))
            assert len(actions) == game.legal_action_count == len(codes)
            assert [int(action.pack()) for action in actions] == codes
            if not actions:
                break

            for action in actions:
                assert _signature(Action.unpack(int(action.pack()))) == _signature(action)
                for encoder in (ActionEncoderV2, ActionEncoderV3):
                    action_id = encoder.encode(action, game)
                    assert 0 <= action_id < encoder.ACTION_SIZE
                    assert _signature(
                        encoder.decode_and_match(action_id, game)
                    ) == _signature(action)

            checked += len(actions)
            assert game.apply_random_action(rng.randrange(2**64), False)

    assert checked > 10_000
