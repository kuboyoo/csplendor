from collections import defaultdict

from csplendor import Game, ActionType, get_card


def _setup_player_with_payment_choices(seed=42):
    game = Game(seed=seed)
    player = game.board.players[0]
    player.gems = [2, 2, 2, 2, 1, 1]
    player.reserved = [5, -1, -1]
    game.board.set_player(0, player)
    return game


def _effective_cost(card, player):
    return [max(0, int(card.cost[i]) - int(player.bonuses[i])) for i in range(5)]


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
