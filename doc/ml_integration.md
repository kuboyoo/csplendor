# Machine Learning Integration Guide

`csplendor` provides specialized tools to bridge the gap between game logic and neural network training.

## 1. State Featurization (`StateFeaturizer`)
The `StateFeaturizer` class converts the current game state into a fixed-length NumPy vector of size **196**. All values are normalized to a range of [0, 1] to facilitate training.

### Feature Map (196 elements)
1. **Bank Gems (6)**: Normalized by 5.0 (for gold) or 4.0 (for colors).
2. **Player 0 Information (36)**:
    - Current Gems (6): Normalized by 10.0.
    - Current Bonuses (5): Normalized by 8.0.
    - Prestige Points (1): Normalized by 15.0.
    - Reserved Cards (3 cards * 8 features = 24): Each card has [id/90, level/3, points/5, bonus/5, cost[5]/7].
3. **Player 1 Information (36)**: Same structure as Player 0.
4. **Visible Cards (12 cards * 8 features = 96)**: Cards on the board (3 levels * 4 slots). Same 8 features as reserved cards.
5. **Deck Counts (3)**: Number of cards remaining in each deck. Normalized by 40.0.
6. **Nobles (3 nobles * 6 features = 18)**: Nobles available on board. [id/10, requirement[5]/4].
7. **Current Player (1)**: 0 or 1.

### Usage
```python
from csplendor import Game, StateFeaturizer
game = Game()
featurizer = StateFeaturizer()
feature_vector = featurizer.featurize(game) # numpy array of shape (196,)
```

## 2. Action Encoding (`ActionEncoder`)
The `ActionEncoder` maps complex Splendor actions to a fixed action space of size **45**.

### Action Index Mapping (0-44)
- **0-9**: `TAKE_DIFFERENT` (10 combinations of 3 colors from 5).
- **10-14**: `TAKE_SAME` (5 colors).
- **15-26**: `RESERVE_VISIBLE` (12 board slots).
- **27-29**: `RESERVE_DECK` (3 levels).
- **30-41**: `PURCHASE` from board (12 board slots).
- **42-44**: `PURCHASE` from reserved (3 slots).

### Action Masking
For RL, use `get_action_mask(game)` to obtain a boolean mask of size 45, indicating which of the 45 base actions are legal in the current state.

### Usage
```python
from csplendor import ActionEncoder
encoder = ActionEncoder()
mask = encoder.get_action_mask(game) # boolean numpy array of shape (45,)
action_idx = model.predict(feature_vector, mask) # Hypothetical model call
action = encoder.decode(action_idx, game)
game.apply(action)
```

> [!NOTE]
> `encoder.decode()` automatically handles the complexities of gem return and noble choice by picking the first valid combination for the chosen base action.
