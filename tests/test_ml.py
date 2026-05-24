import numpy as np

from csplendor import ActionEncoder, ActionEncoderCpp, Game, StateEncoder, StateFeaturizer


def test_python_and_cpp_state_encoders_return_stable_feature_vectors():
    game = Game(seed=42)
    featurizer = StateFeaturizer()

    py_features = featurizer.featurize(game)
    cpp_features = np.asarray(StateEncoder.encode(game), dtype=np.float32)
    cpp_canonical = np.asarray(StateEncoder.encode_canonical(game, 1), dtype=np.float32)

    assert py_features.shape == (196,)
    assert cpp_features.shape == (196,)
    assert cpp_canonical.shape == (196,)
    assert np.isfinite(py_features).all()
    assert np.isfinite(cpp_features).all()
    assert np.isfinite(cpp_canonical).all()
    np.testing.assert_allclose(py_features, cpp_features, rtol=1e-6, atol=1e-6)


def test_legacy_python_action_encoder_mask_covers_legal_base_actions():
    game = Game(seed=42)
    encoder = ActionEncoder()

    legal_actions = game.legal_actions
    mask = encoder.get_action_mask(game)

    assert mask.shape == (encoder.BASE_ACTION_COUNT,)
    assert mask.dtype == np.bool_
    assert mask.any()

    encoded_ids = {encoder.encode(action, game) for action in legal_actions}
    encoded_ids.discard(-1)
    assert encoded_ids
    assert encoded_ids == set(np.flatnonzero(mask))

    for action_id in encoded_ids:
        decoded = encoder.decode(action_id, game)
        assert decoded is not None
        assert encoder.encode(decoded, game) == action_id


def test_cpp_legacy_action_encoder_mask_matches_encoded_legals():
    game = Game(seed=123)
    mask = np.asarray(ActionEncoderCpp.get_action_mask(game), dtype=np.uint8)

    assert mask.shape == (ActionEncoderCpp.BASE_ACTION_COUNT,)
    assert mask.sum() > 0

    encoded_ids = {ActionEncoderCpp.encode(action, game) for action in game.legal_actions}
    encoded_ids.discard(-1)
    assert encoded_ids == set(np.flatnonzero(mask))
