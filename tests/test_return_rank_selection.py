"""Binding-level checks against the unchanged full enumeration API."""

import pytest

from csplendor import Game
from tests.support import set_current_player


@pytest.mark.parametrize('simple', [False, True])
@pytest.mark.parametrize('gems', [[2, 2, 2, 2, 2, 0], [3, 3, 3, 3, 3, 3]])
def test_return_rank_all_indices_and_modulo(simple, gems):
    game = Game(seed=42)
    game.simple_payment_mode = simple
    set_current_player(game, gems=gems)
    codes = list(game.legal_action_codes)
    actions = game.legal_actions
    parent = game.serialize_snapshot()
    assert len(codes) == len(actions) == game.legal_action_count
    for index, (code, action) in enumerate(zip(codes, actions)):
        assert game.legal_action_code_at(index) == code == action.pack()
        expected = game.clone_light()
        assert expected.apply_trusted(action, False)
        actual = game.clone_light()
        assert actual.apply_legal_action_index(index, True)
        assert actual.serialize_snapshot() == expected.serialize_snapshot()
        assert actual.undo()
        assert actual.serialize_snapshot() == parent
    for random_value in (0, len(codes) - 1, len(codes), 2**64 - 1):
        expected = game.clone_light()
        assert expected.apply_trusted(actions[random_value % len(codes)], False)
        actual = game.clone_light()
        assert actual.apply_random_action(random_value, False)
        assert actual.serialize_snapshot() == expected.serialize_snapshot()
    for index in (len(codes), 2048, 65535):
        assert game.legal_action_code_at(index) == 0
        assert game.apply_legal_action_index(index, True) is False
        assert game.serialize_snapshot() == parent
