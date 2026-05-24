from collections import defaultdict
from itertools import product

from csplendor import ActionType, Game, get_card


def _set_player_state(game, gems=None, reserved=None):
    player = game.board.players[0]
    if gems is not None:
        player.gems = list(gems)
    if reserved is not None:
        player.reserved = list(reserved)
    game.board.set_player(0, player)


def _setup_player_with_payment_choices(seed=42):
    game = Game(seed=seed)
    _set_player_state(game, gems=[2, 2, 2, 2, 1, 1], reserved=[5, -1, -1])
    return game


def _effective_cost(card, player):
    return [max(0, int(card.cost[i]) - int(player.bonuses[i])) for i in range(5)]


def _expected_gold_as_patterns(card, player):
    effective = _effective_cost(card, player)
    patterns = set()

    for gold_as in product(*(range(cost + 1) for cost in effective)):
        if sum(gold_as) > player.gems[5]:
            continue
        if all(effective[i] - gold_as[i] <= player.gems[i] for i in range(5)):
            patterns.add(tuple(gold_as))

    return patterns


def _purchase_groups(game):
    groups = defaultdict(list)
    for action in game.legal_actions:
        if action.type == ActionType.PURCHASE:
            groups[(int(action.card_id), bool(action.from_reserved))].append(action)
            assert sum(action.return_gems) == 0
    return groups


def _assert_purchase_payment_patterns(game, source_cards):
    player = game.board.players[game.board.current_player]
    groups = _purchase_groups(game)
    checked = 0

    for card_id, from_reserved in source_cards:
        expected = _expected_gold_as_patterns(get_card(card_id), player)
        actual = {
            tuple(int(v) for v in action.gold_as)
            for action in groups.get((card_id, from_reserved), [])
        }

        assert actual == expected, (
            f"payment patterns mismatch for card={card_id} "
            f"from_reserved={from_reserved}: "
            f"missing={sorted(expected - actual)[:8]} "
            f"extra={sorted(actual - expected)[:8]}"
        )
        checked += 1

    return checked


def test_purchase_actions_have_valid_gold_assignments_and_apply():
    game = _setup_player_with_payment_choices()
    player = game.board.players[0]
    purchase_actions = [a for a in game.legal_actions if a.type == ActionType.PURCHASE]

    assert purchase_actions
    by_card = defaultdict(list)
    for action in purchase_actions:
        by_card[(int(action.card_id), bool(action.from_reserved))].append(action)
        card = get_card(int(action.card_id))
        effective = _effective_cost(card, player)
        gold_as = [int(v) for v in action.gold_as]

        assert sum(action.return_gems) == 0
        assert sum(gold_as) <= player.gems[5]
        for color in range(5):
            assert 0 <= gold_as[color] <= effective[color]
            assert effective[color] - gold_as[color] <= player.gems[color]

        clone = game.clone()
        assert clone.apply(action) is True
        after = clone.board.players[0]
        assert int(action.card_id) in [int(card_id) for card_id in after.purchased_cards]

    assert len(by_card) > 0
    assert any(len(options) > 1 for options in by_card.values())


def test_visible_purchase_payment_options_are_exhaustive():
    game = Game(seed=42)
    _set_player_state(game, gems=[1, 1, 1, 1, 1, 3])

    source_cards = [
        (int(card_id), False)
        for level in game.board.visible
        for card_id in level
        if card_id != -1
    ]

    assert _assert_purchase_payment_patterns(game, source_cards) == len(source_cards)


def test_reserved_purchase_payment_options_are_exhaustive():
    game = Game(seed=42)
    _set_player_state(game, gems=[1, 1, 1, 1, 1, 3])
    reserved_card_id = int(game.board.visible[0][0])
    _set_player_state(game, reserved=[reserved_card_id, -1, -1])

    assert _assert_purchase_payment_patterns(game, [(reserved_card_id, True)]) == 1


def test_simple_payment_mode_keeps_minimal_gold_options_only():
    full_game = _setup_player_with_payment_choices(seed=7)
    simple_game = full_game.clone()
    simple_game.simple_payment_mode = True

    full_purchase = [a for a in full_game.legal_actions if a.type == ActionType.PURCHASE]
    simple_purchase = [a for a in simple_game.legal_actions if a.type == ActionType.PURCHASE]

    assert full_purchase
    assert simple_purchase
    assert len(simple_purchase) <= len(full_purchase)

    simple_by_card = defaultdict(list)
    for action in simple_purchase:
        simple_by_card[(int(action.card_id), bool(action.from_reserved))].append(action)

    assert all(len(actions) == 1 for actions in simple_by_card.values())
