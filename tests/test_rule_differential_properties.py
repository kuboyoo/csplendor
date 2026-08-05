"""Shared-corpus differential properties for rule-facing engine surfaces."""

from tests.support import (
    assert_differential_corpus,
    random_reachable_state_corpus,
    reachable_state_corpus,
)


def _materialized_action_codes(game):
    return tuple(int(action.pack()) for action in game.legal_actions)


def _native_action_codes(game):
    return tuple(int(code) for code in game.legal_action_codes)


def test_materialized_and_native_action_codes_match_on_shared_fixture():
    assert_differential_corpus(
        reachable_state_corpus(plies=12),
        _materialized_action_codes,
        _native_action_codes,
        label="reachable state",
    )


def test_materialized_and_native_action_codes_match_on_property_corpus():
    assert_differential_corpus(
        random_reachable_state_corpus(
            master_seed=20260805, games=16, max_plies=48
        ),
        _materialized_action_codes,
        _native_action_codes,
        label="seeded property state",
    )
