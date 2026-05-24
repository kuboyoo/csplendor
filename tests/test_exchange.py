from collections import defaultdict
from itertools import product

import pytest

from csplendor import ActionEncoderV2, ActionEncoderV3, ActionType, Game


EXCHANGE_TYPES = {
    ActionType.TAKE_DIFFERENT,
    ActionType.TAKE_SAME,
    ActionType.RESERVE_VISIBLE,
    ActionType.RESERVE_DECK,
}


def setup_game_with_gems(gems, seed=42, bank=None):
    game = Game(seed=seed)
    if bank is not None:
        game.board.bank = list(bank)
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


def _base_action_key(action):
    if action.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
        return action.type, _take_tuple(action)
    if action.type == ActionType.RESERVE_VISIBLE:
        return action.type, int(action.card_id)
    if action.type == ActionType.RESERVE_DECK:
        return action.type, int(action.deck_level)
    return action.type, int(action.card_id), bool(action.from_reserved)


def _gem_distributions(total=None):
    for colors in product(range(5), repeat=5):
        for gold in range(6):
            gems = colors + (gold,)
            if total is None:
                if sum(gems) <= 10:
                    yield gems
            elif sum(gems) == total:
                yield gems


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
        if action.type == action_type:
            groups[_base_action_key(action)].append(action)

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


def verify_all_return_groups(game, action_types=None):
    player_gems = list(game.board.players[game.board.current_player].gems)
    groups = defaultdict(list)
    for action in game.legal_actions:
        if action_types is not None and action.type not in action_types:
            continue
        groups[_base_action_key(action)].append(action)

    checked_groups = 0
    checked_actions = 0
    for key, actions in groups.items():
        next_gems = _next_gems_after_base_action(player_gems, actions[0], game.board)
        excess = max(0, sum(next_gems) - 10)
        expected = _expected_return_combos(next_gems, excess)
        actual = {_return_tuple(action) for action in actions}

        assert actual == expected, (
            key,
            sorted(expected - actual)[:8],
            sorted(actual - expected)[:8],
            player_gems,
            next_gems,
        )
        assert len(actions) == len(actual), key
        for action in actions:
            assert sum(action.return_gems) == excess
            assert sum(next_gems) - sum(action.return_gems) <= 10

        checked_groups += 1
        checked_actions += len(actions)

    return checked_groups, checked_actions


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
def test_exchange_generation_has_no_missing_or_duplicate_returns(
    gems, action_type, expected_excess
):
    verify_exchange_actions(setup_game_with_gems(gems), action_type, expected_excess)


def test_full_bank_all_player_token_distributions():
    checked_states = 0
    checked_groups = 0
    checked_actions = 0

    for gems in _gem_distributions():
        game = setup_game_with_gems(gems)
        groups, actions = verify_all_return_groups(game, EXCHANGE_TYPES)
        checked_states += 1
        checked_groups += groups
        checked_actions += actions

    assert checked_states == 5498
    assert checked_groups > 0
    assert checked_actions > 0


def test_depleted_bank_take_different_one_or_two_colors():
    checked_states = 0
    checked_groups = 0
    checked_actions = 0

    for available_count in (1, 2):
        for mask in range(1, 1 << 5):
            if mask.bit_count() != available_count:
                continue
            bank = tuple(1 if mask & (1 << i) else 0 for i in range(5)) + (5,)
            for gems in _gem_distributions(total=10):
                game = setup_game_with_gems(gems, bank=bank)
                groups, actions = verify_all_return_groups(
                    game, {ActionType.TAKE_DIFFERENT}
                )
                checked_states += 1
                checked_groups += groups
                checked_actions += actions

    assert checked_states == 15 * 1627
    assert checked_groups > 0
    assert checked_actions > 0


def test_depleted_bank_no_gold_reserve_has_no_return():
    checked_groups = 0
    checked_actions = 0
    for gems in _gem_distributions(total=10):
        game = setup_game_with_gems(gems, bank=(4, 4, 4, 4, 4, 0))
        groups, actions = verify_all_return_groups(
            game, {ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK}
        )
        checked_groups += groups
        checked_actions += actions

    assert checked_groups > 0
    assert checked_actions > 0


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
