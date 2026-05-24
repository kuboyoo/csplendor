# csplendor: High-Performance Splendor Engine

`csplendor` is a fast C++ based engine for the board game Splendor, optimized for 2-player competitive play and machine-learning workflows.

## Features
- **Fast logic**: C++17 implementation capable of about 20,000 moves/sec.
- **Python bindings**: Seamless integration via `pybind11`.
- **ML ready**: Built-in state featurization and action-space encoders.
- **Web API**: FastAPI integration for GUI development.

## Installation & Build

### Prerequisites
- C++17 compatible compiler, such as GCC 9+
- CMake 3.12+
- Python 3.8+
- `pybind11`, `numpy`, `fastapi`, `uvicorn`

### Building from Source
If you modify the C++ source files, rebuild the extension.

**Option 1: Using pip (recommended for development)**
```bash
pip install -e .
```

**Option 2: Manual CMake build**
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

# 1. Initialize a game
game = csplendor.Game(seed=42)

# 2. Get legal actions
legals = game.legal_actions
print(f"Legal moves: {len(legals)}")

# 3. Apply an action
action = legals[0]
game.apply(action)

# 4. Access state
board = game.board
print(f"Current Turn: {board.turn}")
print(f"Scores: {game.scores}")

# 5. Featurize for ML
featurizer = csplendor.StateFeaturizer()
features = featurizer.featurize(game) # numpy array (196,)
```

## Running the Web API
Start the FastAPI server for GUI integration with:
```bash
uvicorn csplendor.api:app --reload
```

## Documentation
For detailed specifications, see the `doc/` directory:
- [Technical Overview](doc/overview.md)
- [Engine Specs](doc/engine_specs.md)
- [Python API Reference](doc/api_ref.md)
- [ML Integration Guide](doc/ml_integration.md)
- [Web API Reference](doc/web_api.md)

## Testing
Run the normal test suite with:
```bash
pip install -e ".[dev,web]"
python -m pytest
python -m compileall -q csplendor
```

Run performance checks explicitly with:
```bash
python -m pytest -m performance
```

---

## Action Space Reference

The current recommended encoder is `ActionEncoderV3`. It indexes purchase actions by card ID, reducing slot-position-dependent redundancy.

### ActionEncoderV3 (3133 actions)

| Category | Offset | Size | Description |
|----------|--------|------|-------------|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE | 1085 | 2035 | 90 cards x card-specific payment patterns |
| VISIT_NOBLE | 3120 | 12 | noble ID 0-11 |
| PASS | 3132 | 1 | none |
| **Total** | none | **3133** | none |

### ActionEncoderV2 (4869 actions)

`ActionEncoderV2` is the compatibility full action-space encoder. It indexes purchase actions by visible/reserved slot.

| Category | Offset | Size | Description |
|----------|--------|------|-------------|
| TAKE_DIFFERENT | 0 | 840 | 10 combos x 84 return patterns |
| TAKE_SAME | 840 | 140 | 5 colors x 28 return patterns |
| RESERVE_VISIBLE | 980 | 84 | 12 slots x 7 return patterns |
| RESERVE_DECK | 1064 | 21 | 3 levels x 7 return patterns |
| PURCHASE_VISIBLE | 1085 | 3024 | 12 slots x 252 payment patterns |
| PURCHASE_RESERVED | 4109 | 756 | 3 slots x 252 payment patterns |
| VISIT_NOBLE | 4865 | 3 | visible noble slots |
| PASS | 4868 | 1 | none |
| **Total** | none | **4869** | none |

### Compatibility Notes

- **ActionEncoderCpp**: 48 actions, compressed representation without return/payment variants.
- **ActionEncoderV2**: 4869 actions, slot-based full representation including return/payment variants.
- **ActionEncoderV3**: 3133 actions, current recommended card-ID-based representation.
