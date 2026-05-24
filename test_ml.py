import numpy as np
from csplendor import Game, StateFeaturizer, ActionEncoder

def test_ml_components():
    game = Game(seed=42)
    featurizer = StateFeaturizer()
    encoder = ActionEncoder()
    
    print("Testing ML Components...")
    
    # 1. Test Featurization
    features = featurizer.featurize(game)
    print(f"Feature shape: {features.shape}")
    assert features.shape[0] > 0
    assert not np.isnan(features).any()
    
    # 2. Test Action Encoding
    legal_actions = game.legal_actions
    print(f"Number of legal actions: {len(legal_actions)}")
    
    mask = encoder.get_action_mask(game)
    print(f"Action mask shape: {mask.shape}")
    print(f"Number of masked legal actions: {np.sum(mask)}")
    
    assert mask.shape[0] == encoder.BASE_ACTION_COUNT
    
    # 3. Test Roundtrip (if any legal)
    if len(legal_actions) > 0:
        action = legal_actions[0]
        idx = encoder.encode(action, game)
        print(f"Encoded action index: {idx}")
        if idx != -1:
            decoded = encoder.decode(idx, game)
            assert decoded is not None
            # We can't easily compare Action objects because they are C++ objects
            # but we can check if the type matches
            assert decoded.type == action.type
            print("Action roundtrip (type check) passed.")

    # 4. Run a few steps in the game
    for i in range(5):
        legals = game.legal_actions
        if not legals:
            break
        game.apply(legals[0])
        features = featurizer.featurize(game)
        assert features.shape == (196,) # 6(bank) + 2*36(players) + 96(visible) + 3(decks) + 18(nobles) + 1(current) = 196
        # Bank (6)
        # Player 0 (36): gems(6), bonuses(5), points(1), reserved(3*8=24)
        # Player 1 (36)
        # Visible (12*8=96)
        # Decks (3)
        # Nobles (3*NOBLE_FEATURE_SIZE(6)=18)
        # Current (1)
        # Total: 6 + 36 + 36 + 96 + 3 + 18 + 1 = 196
        # Wait, my CARD_FEATURE_SIZE is 8.
        # 6 + 2*(6+5+1+3*8) + 12*8 + 3 + 3*6 + 1 = 196.
        # Let's see what the actual shape is.
        print(f"Step {i} feature shape: {features.shape}")

    print("ML components test completed successfully!")

if __name__ == "__main__":
    test_ml_components()
