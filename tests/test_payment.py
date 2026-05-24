from collections import defaultdict
from itertools import product

from csplendor import ActionType, Game, get_card


def set_player_gems(game, gems):
    p0 = game.board.players[0]
    p0.gems = list(gems)
    game.board.set_player(0, p0)


def set_reserved_card(game, card_id):
    p0 = game.board.players[0]
    p0.reserved = [card_id]
    game.board.set_player(0, p0)


def expected_gold_as_patterns(card, player):
    effective_cost = [
        max(0, int(card.cost[i]) - int(player.bonuses[i]))
        for i in range(5)
    ]
    gold = int(player.gems[5])
    patterns = set()

    for gold_as in product(*(range(cost + 1) for cost in effective_cost)):
        if sum(gold_as) > gold:
            continue
        if all(effective_cost[i] - gold_as[i] <= player.gems[i]
               for i in range(5)):
            patterns.add(tuple(gold_as))

    return patterns


def purchase_groups(game):
    groups = defaultdict(list)
    for action in game.legal_actions:
        if action.type == ActionType.PURCHASE:
            groups[(action.card_id, action.from_reserved)].append(action)
            assert sum(action.return_gems) == 0
    return groups


def assert_purchase_payment_patterns(game, source_cards):
    player = game.board.players[game.board.current_player]
    groups = purchase_groups(game)
    checked = 0

    for card_id, from_reserved in source_cards:
        expected = expected_gold_as_patterns(get_card(card_id), player)
        actual = {
            tuple(action.gold_as)
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


def test_visible_purchase_payment_options_are_exhaustive():
    game = Game(seed=42)
    set_player_gems(game, [1, 1, 1, 1, 1, 3])

    source_cards = [
        (card_id, False)
        for level in game.board.visible
        for card_id in level
        if card_id != -1
    ]

    assert assert_purchase_payment_patterns(game, source_cards) == len(source_cards)


def test_reserved_purchase_payment_options_are_exhaustive():
    game = Game(seed=42)
    set_player_gems(game, [1, 1, 1, 1, 1, 3])
    reserved_card_id = game.board.visible[0][0]
    set_reserved_card(game, reserved_card_id)

    assert assert_purchase_payment_patterns(game, [(reserved_card_id, True)]) == 1
