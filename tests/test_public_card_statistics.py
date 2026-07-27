import math

import numpy as np
import pytest

import csplendor as cs


def _hidden_reserve(game: cs.Game, level: int) -> None:
    action = next(
        action
        for action in game.legal_actions
        if action.type == cs.ActionType.RESERVE_DECK
        and int(action.deck_level) == level - 1
    )
    assert game.apply_trusted(action, False)


def test_observable_pool_contains_deck_and_hidden_opponent_reserve() -> None:
    game = cs.Game(seed=20260726)
    before = game.board.observable_card_pool(1, 1)

    _hidden_reserve(game, 1)
    observer_pool = game.board.observable_card_pool(1, 1)
    owner_pool = game.board.observable_card_pool(0, 1)
    hidden_card = next(
        int(card_id)
        for card_id, hidden in zip(
            game.board.players[0].reserved,
            game.board.players[0].reserved_is_hidden,
        )
        if hidden
    )

    assert observer_pool == before
    assert hidden_card in observer_pool
    assert hidden_card not in owner_pool
    assert len(owner_pool) + 1 == len(observer_pool)


def test_observable_pool_is_invariant_under_determinization() -> None:
    game = cs.Game(seed=51)
    _hidden_reserve(game, 2)
    expected = game.board.observable_card_pool(1, 2)

    for seed in range(16):
        shuffled = game.shuffled_clone(1, seed)
        assert shuffled.board.observable_card_pool(1, 2) == expected


def test_public_card_statistics_are_finite_normalized_and_observer_safe() -> None:
    game = cs.Game(seed=77)
    _hidden_reserve(game, 3)
    expected = np.asarray(
        cs.StateEncoder.encode_public_card_statistics(game, 1, 1),
        dtype=np.float32,
    )

    assert expected.shape == (cs.StateEncoder.public_card_feature_size(),)
    assert expected.shape == (117,)
    assert np.isfinite(expected).all()
    assert (expected >= 0.0).all()
    assert (expected <= 1.0).all()

    for seed in range(16):
        shuffled = game.shuffled_clone(1, seed)
        actual = np.asarray(
            cs.StateEncoder.encode_public_card_statistics(shuffled, 1, 1),
            dtype=np.float32,
        )
        np.testing.assert_array_equal(actual, expected)


def test_next_three_probability_matches_hypergeometric_reference() -> None:
    game = cs.Game(seed=9)
    features = np.asarray(
        cs.StateEncoder.encode_public_card_statistics(game, 0, 0),
        dtype=np.float64,
    )
    pool = game.board.observable_card_pool(0, 1)
    player = game.board.players[0]
    reachable = 0
    for card_id in pool:
        card = cs.get_card(card_id)
        shortfall = sum(
            max(
                0,
                int(card.cost[color])
                - int(player.bonuses[color])
                - int(player.gems[color]),
            )
            for color in range(5)
        )
        if max(0, shortfall - int(player.gems[5])) <= 3:
            reachable += 1

    population = len(pool)
    draws = min(3, len(game.board.decks[0]))
    expected = 1.0 - (
        math.comb(population - reachable, draws) / math.comb(population, draws)
    )
    # Per tier: sizes(3), bonuses(5), points(6), costs(5), then player
    # next-one[0:4] and next-three[0:4]. Threshold <=3 is the fourth value.
    actual = features[3 + 5 + 6 + 5 + 4 + 3]
    assert actual == pytest.approx(expected, abs=1e-6)


@pytest.mark.parametrize("level", [0, 4])
def test_card_pool_rejects_invalid_level(level: int) -> None:
    game = cs.Game(seed=0)
    with pytest.raises(ValueError):
        game.board.observable_card_pool(0, level)
