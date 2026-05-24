from collections import defaultdict

import pytest

from csplendor import ActionEncoderV2, ActionEncoderV3, ActionType, Game


def setup_game_with_gems(gems, seed=42):
    game = Game(seed=seed)
    player = game.board.players[0]
    player.gems = list(gems)
    game.board.set_player(0, player)
    return game


def _return_tuple(action):
    return tuple(int(v) for v in action.return_gems)


def _take_tuple(action):
    return tuple(int(v) for v in action.take)


def _action_signature(action):
    return (
        int(action.type),
        _take_tuple(action),
        int(action.card_id),
        int(action.deck_level),
        bool(action.from_reserved),
        tuple(int(v) for v in action.gold_as),
        _return_tuple(action),
        int(action.noble_choice),
    )


def _expected_return_combos(available, excess):
    results = set()
    current = [0] * 6

    def rec(color, remaining):
        if remaining == 0:
            results.add(tuple(current))
            return
        if color == 6:
            return
        for amount in range(min(remaining, int(available[color])) + 1):
            current[color] = amount
            rec(color + 1, remaining - amount)
            current[color] = 0

    rec(0, excess)
    return results


def _next_gems_after_base_action(player_gems, action, board):
    next_gems = list(player_gems)
    if action.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
        for i in range(5):
            next_gems[i] += int(action.take[i])
    elif action.type in (ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK):
        if board.bank[5] > 0:
            next_gems[5] += 1
    return next_gems


def verify_exchange_actions(game, action_type, expected_excess):
    player_gems = list(game.board.players[0].gems)
    groups = defaultdict(list)
    for action in game.legal_actions:
        if action.type != action_type:
            continue
        if action_type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
            key = _take_tuple(action)
        elif action_type == ActionType.RESERVE_VISIBLE:
            key = ("visible", int(action.card_id))
        else:
            key = ("deck", int(action.deck_level))
        groups[key].append(action)

    assert groups
    for key, actions in groups.items():
        next_gems = _next_gems_after_base_action(player_gems, actions[0], game.board)
        excess = max(0, sum(next_gems) - 10)
        assert excess == expected_excess, key

        seen = {_return_tuple(action) for action in actions}
        expected = _expected_return_combos(next_gems, expected_excess)
        assert seen == expected, key
        assert len(actions) == len(seen), key

        for action in actions:
            assert sum(action.return_gems) == expected_excess
            assert sum(next_gems) - sum(action.return_gems) == 10


@pytest.mark.parametrize(
    ("gems", "action_type", "expected_excess"),
    [
        ([2, 2, 2, 1, 1, 0], ActionType.TAKE_DIFFERENT, 1),
        ([2, 2, 2, 2, 1, 0], ActionType.TAKE_DIFFERENT, 2),
        ([2, 2, 2, 2, 2, 0], ActionType.TAKE_DIFFERENT, 3),
        ([1, 1, 1, 1, 1, 5], ActionType.TAKE_DIFFERENT, 3),
        ([2, 2, 2, 2, 1, 0], ActionType.TAKE_SAME, 1),
        ([2, 2, 2, 2, 2, 0], ActionType.TAKE_SAME, 2),
        ([1, 1, 1, 1, 3, 3], ActionType.TAKE_SAME, 2),
        ([2, 2, 2, 2, 2, 0], ActionType.RESERVE_VISIBLE, 1),
        ([2, 2, 2, 2, 2, 0], ActionType.RESERVE_DECK, 1),
        ([1, 1, 1, 1, 1, 5], ActionType.RESERVE_VISIBLE, 1),
    ],
)
def test_exchange_generation_has_no_missing_or_duplicate_returns(gems, action_type, expected_excess):
    verify_exchange_actions(setup_game_with_gems(gems), action_type, expected_excess)


@pytest.mark.parametrize("encoder", [ActionEncoderV2, ActionEncoderV3])
@pytest.mark.parametrize(
    "gems",
    [
        [0, 0, 0, 0, 0, 0],
        [2, 2, 2, 1, 1, 0],
        [2, 2, 2, 2, 2, 0],
        [1, 1, 1, 1, 1, 5],
    ],
)
def test_full_action_encoders_cover_exchange_states_without_collisions(encoder, gems):
    game = setup_game_with_gems(gems)
    encoded = {}

    for action in game.legal_actions:
        action_id = encoder.encode(action, game)
        assert 0 <= action_id < encoder.ACTION_SIZE, (encoder, action)

        signature = _action_signature(action)
        assert encoded.setdefault(action_id, signature) == signature

        decoded = encoder.decode_and_match(action_id, game)
        assert _action_signature(decoded) == signature


def test_take_different_return_three_is_encoded_by_current_v2():
    game = setup_game_with_gems([2, 2, 2, 2, 2, 0])
    return_three = [
        action for action in game.legal_actions
        if action.type == ActionType.TAKE_DIFFERENT and sum(action.return_gems) == 3
    ]

    assert return_three
    ids = {ActionEncoderV2.encode(action, game) for action in return_three}
    assert len(ids) == len(return_three)
    assert all(0 <= action_id < ActionEncoderV2.ACTION_SIZE for action_id in ids)
