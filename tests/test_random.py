import random

import csplendor


def _card_ids_in_state(board):
    ids = []
    for row in board.visible:
        ids.extend(int(card_id) for card_id in row if int(card_id) >= 0)
    for deck in board.decks:
        ids.extend(int(card_id) for card_id in deck)
    for player in board.players:
        ids.extend(int(card_id) for card_id in player.reserved if int(card_id) >= 0)
        ids.extend(int(card_id) for card_id in player.purchased_cards if int(card_id) >= 0)
    return ids


def _assert_state_invariants(game):
    board = game.board
    assert board.current_player in (0, 1)
    assert board.turn >= 0
    assert all(int(gem) >= 0 for gem in board.bank)
    assert all(sum(player.gems) <= 10 for player in board.players)

    card_ids = _card_ids_in_state(board)
    assert len(card_ids) == 90
    assert len(set(card_ids)) == 90


def test_seeded_random_playout_preserves_core_invariants():
    rng = random.Random(42)
    game = csplendor.Game(seed=42)

    _assert_state_invariants(game)
    applied = 0
    for _ in range(120):
        if game.is_game_over():
            break
        legal_actions = game.legal_actions
        assert legal_actions, "non-terminal state must expose at least one legal action"
        action = rng.choice(legal_actions)
        assert game.is_legal(action)
        assert game.apply(action) is True
        applied += 1
        _assert_state_invariants(game)

    assert applied > 0
    assert game.turn <= applied
