import numpy as np
import pytest

import csplendor as cs
from csplendor import _csplendor as core


@pytest.mark.parametrize('observer', [-128, -2, -1, 0, 1, 2, 127])
def test_owning_numpy_matches_legacy_bits(observer):
    game = cs.Game(seed=42)
    for ply in range(32):
        legacy = cs.StateEncoder.encode(game, observer)
        assert isinstance(legacy, list)
        expected = np.asarray(legacy, dtype=np.float32)
        actual = cs.StateEncoder.encode_numpy(game, observer)
        assert actual.shape == (cs.StateEncoder.feature_size(),)
        assert actual.dtype == np.float32
        assert actual.flags.c_contiguous and actual.flags.owndata and actual.flags.writeable
        assert actual.base is None
        assert actual.tobytes() == expected.tobytes()
        assert cs.StateFeaturizer().featurize(game, observer).tobytes() == expected.tobytes()
        assert game.apply_random_action(7919 + ply * 97, False)


def test_hidden_rows_and_invalid_input_do_not_change_retained_arrays():
    game = cs.Game(seed=42)
    player = game.board.get_player(1)
    player.reserved = [0, 1, 2]
    player.reserved_is_hidden = [True, True, True]
    game.board.set_player(1, player)
    saved = cs.StateEncoder.encode_numpy(game, 0)
    original = saved.copy()
    for card in range(90):
        player.reserved = [card, card, card]
        game.board.set_player(1, player)
        row = cs.StateEncoder.encode_numpy(game, 0)
        assert row.tobytes() == np.asarray(cs.StateEncoder.encode(game, 0), dtype=np.float32).tobytes()
        assert not np.shares_memory(saved, row)
        # Opponent starts at 42; reserved cards start 12 fields into that block.
        for slot in range(3):
            assert (row[54 + slot * 8:61 + slot * 8] == 0).all()
        row[:] = -100
        assert saved.tobytes() == original.tobytes()
    # Public editor validation rejects bad card IDs before either encoder.
    with pytest.raises(ValueError):
        game.board.visible = [[127] * 4 for _ in range(3)]
    for method in (cs.StateEncoder.encode, cs.StateEncoder.encode_numpy):
        with pytest.raises(TypeError):
            method(game, 128)
    assert saved.tobytes() == original.tobytes()


def test_retained_featurizer_array_survives_search_and_game_destruction():
    game = cs.Game(seed=42)
    featurizer = cs.StateFeaturizer()
    held = featurizer.featurize(game)
    expected = held.tobytes()
    config = cs.MCTSConfig()
    config.use_dirichlet_noise = False
    config.use_determinization = False
    mcts = cs.MCTS(config)
    def inference(requests):
        return [{'policy': np.ones(48, dtype=np.float32),
                 'value': np.zeros(2, dtype=np.float32)} for _ in requests]
    result = core.mcts_search(mcts, featurizer, cs.ActionEncoder(), game, 8, inference, 1.0)
    assert len(result) == 48
    assert game.apply_random_action(42, False)
    del game, mcts, featurizer
    assert held.tobytes() == expected and held.flags.owndata
