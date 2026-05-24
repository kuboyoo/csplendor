# csplendor: High-Performance Splendor Engine

`csplendor` is a fast, C++ based engine for the board game Splendor, optimized for 2-player competitive play and machine learning training.

## Features
- **Fast Logic**: C++17 implementation capable of ~20,000 moves/sec.
- **Python Bindings**: Seamless integration via `pybind11`.
- **ML Ready**: Built-in state featurization and action space encoding.
- **Web API**: FastAPI integration for GUI developments.

## Installation & Build

### Prerequisites
- C++17 compatible compiler (e.g., GCC 9+)
- CMake 3.12+
- Python 3.8+
- `pybind11`, `numpy`, `fastapi`, `uvicorn`

### Building from Source
If you modify the C++ source files, you need to rebuild the extension.

**Option 1: Using pip (Recommended for development)**
```bash
pip install -e .
```

**Option 2: Manual CMake Build**
```bash
mkdir -p build
cd build
cmake ..
make -j
# Copy the compiled library to the package directory
cp _csplendor.*.so ../csplendor/
```

## Basic Usage (Python)

```python
import csplendor

# 1. Initialize Game
game = csplendor.Game(seed=42)

# 2. Get Legal Actions
legals = game.legal_actions
print(f"Legal moves: {len(legals)}")

# 3. Apply an Action
action = legals[0]
game.apply(action)

# 4. Access State
board = game.board
print(f"Current Turn: {board.turn}")
print(f"Scores: {game.scores}")

# 5. Featurize for ML
featurizer = csplendor.StateFeaturizer()
features = featurizer.featurize(game) # numpy array (196,)
```

## Running the Web API
To start the FastAPI server for GUI interaction:
```bash
uvicorn csplendor.api:app --reload
```

## Documentation
For detailed specifications, please refer to the `doc/` directory:
- [Technical Overview](doc/overview.md)
- [Engine Specs](doc/engine_specs.md)
- [Python API Reference](doc/api_ref.md)
- [ML Integration Guide](doc/ml_integration.md)
- [Web API Reference](doc/web_api.md)

## Testing
Run the verification scripts to ensure everything is working:
```bash
PYTHONPATH=. python test_random.py
PYTHONPATH=. python test_ml.py
PYTHONPATH=. python test_api.py
```

---

## Action Space Reference (ActionEncoderV2)

> **Version**: V2 (749 actions, redundancy-free)  
> **Header**: `src/action_encoder_v2.h`  
> **Python**: `csplendor.ActionEncoderV2`

### Overview

| Category | Offset | Size | Formula |
|----------|--------|------|---------|
| TAKE_DIFFERENT | 0 | 100 | 10 combos × 10 return patterns |
| TAKE_SAME | 100 | 105 | 5 colors × 21 return patterns |
| RESERVE_VISIBLE | 205 | 336 | 12 slots × 28 return patterns |
| RESERVE_DECK | 541 | 84 | 3 levels × 28 return patterns |
| PURCHASE_VISIBLE | 625 | 96 | 12 slots × 8 payment patterns |
| PURCHASE_RESERVED | 721 | 24 | 3 slots × 8 payment patterns |
| VISIT_NOBLE | 745 | 3 | 3 nobles |
| PASS | 748 | 1 | — |
| **Total** | — | **749** | — |

### Action ID Calculation

```
TAKE_DIFFERENT: ID = combo_idx * 10 + return_pattern
TAKE_SAME:      ID = 100 + color * 21 + return_pattern
RESERVE_VISIBLE: ID = 205 + (level * 4 + slot) * 28 + return_pattern
RESERVE_DECK:   ID = 541 + level * 28 + return_pattern
PURCHASE_VISIBLE: ID = 625 + (level * 4 + slot) * 8 + payment_pattern
PURCHASE_RESERVED: ID = 721 + slot * 8 + payment_pattern
VISIT_NOBLE:    ID = 745 + noble_idx
PASS:           ID = 748
```

### TAKE_DIFFERENT (10 combos × 10 return patterns = 100)

**Combo Index → Colors Taken**:
| Combo | Colors |
|-------|--------|
| 0 | W(0), B(1), G(2) |
| 1 | W(0), B(1), R(3) |
| 2 | W(0), B(1), K(4) |
| 3 | W(0), G(2), R(3) |
| 4 | W(0), G(2), K(4) |
| 5 | W(0), R(3), K(4) |
| 6 | B(1), G(2), R(3) |
| 7 | B(1), G(2), K(4) |
| 8 | B(1), R(3), K(4) |
| 9 | G(2), R(3), K(4) |

**Returnable Colors** (per combo): The 2 non-taken colors + Gold.

| Combo | Returnable |
|-------|------------|
| 0 (WBG) | R(3), K(4), Gold(5) |
| 1 (WBR) | G(2), K(4), Gold(5) |
| ... | ... |

**Return Pattern Index** (10 patterns):
| Pattern | Description |
|---------|-------------|
| 0 | No return |
| 1-3 | Return 1 of [r0, r1, gold] |
| 4-9 | Return 2 (combinations with repetition) |

> **Constraint**: Cannot return a color that was just taken.

### TAKE_SAME (5 colors × 21 return patterns = 105)

**Returnable Colors**: The 4 non-taken colors + Gold.

**Return Pattern Index** (21 patterns):
| Pattern | Description |
|---------|-------------|
| 0 | No return |
| 1-5 | Return 1 of 5 returnable colors |
| 6-20 | Return 2 (H(5,2) = 15 combinations) |

> **Constraint**: Cannot return the color that was just taken.

### RESERVE (12/3 slots × 28 return patterns = 336/84)

**Slot Index**:
- VISIBLE: `level * 4 + slot` (0-11)
- DECK: `level` (0-2)

**Return Pattern Index** (28 patterns):
| Pattern | Description |
|---------|-------------|
| 0 | No return |
| 1-6 | Return 1 of 6 colors (including gold) |
| 7-27 | Return 2 (H(6,2) = 21 combinations) |

> Gold can be returned because gold receipt is mandatory during reserve.

### PURCHASE (12/3 slots × 8 payment patterns = 96/24)

**Slot Index**: Same as RESERVE.

**Payment Pattern Index**: Total gold used as wildcard (0-7).

> The exact gold_as breakdown is determined by matching to legal actions.

### Color Indices

| Index | Color | Symbol |
|-------|-------|--------|
| 0 | White (Diamond) | W |
| 1 | Blue (Sapphire) | B |
| 2 | Green (Emerald) | G |
| 3 | Red (Ruby) | R |
| 4 | Black (Onyx) | K |
| 5 | Gold | $ |

### Compatibility Notes

- **ActionEncoderCpp (V1)**: 48 actions, compressed (no return/payment variants)
- **ori (genbu.pt)**: 406 actions, different encoding scheme
- **ActionEncoderV2**: 749 actions, full detail with redundancy elimination

When mapping between encoders, use the mapping functions in `OriAdapter.py`.

### Verification Snippet

```python
from csplendor._csplendor import ActionEncoderV2, Game, ActionType

game = Game(42)
for action in game.legal_actions:
    if action.type == ActionType.TAKE_DIFFERENT:
        taken = {i for i in range(5) if action.take[i] > 0}
        returned = {i for i in range(5) if action.return_gems[i] > 0}
        assert not (taken & returned), "Redundant action detected!"
        
    encoded = ActionEncoderV2.encode(action, game)
    assert 0 <= encoded < 749, f"Invalid action ID: {encoded}"
```

