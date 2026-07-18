# csplendor: High-Performance Splendor Engine

`csplendor` is a fast C++ based engine for the board game Splendor, optimized for 2-player competitive play and machine-learning workflows.

## Features
- **Fast logic**: On the 250-legal-action midgame benchmark below, the C++17 implementation reaches approximately 26,000 Python `legal_actions` calls/sec, 980,000 C++ internal legal-action counts/sec, and 740,000 C++ internal self-play moves/sec.
- **Python bindings**: Seamless integration via `pybind11`.
- **ML ready**: Built-in state featurization and action-space encoders.
- **Web API**: FastAPI integration for GUI development.

### Performance reference

Measured on 2026-07-13 with a Ryzen 9 7900X, GCC 13.3, a Release build,
Python 3.12.1, and one pinned logical CPU. The representative legal-action
workload is the same seed-42, 12-ply, 250-action position used by
`tests/test_perf.py`; the table reports the median of seven best-of-five runs.
The self-play row uses a separate 30-sample workload of ten games (seeds 0--9).

| Operation | Before refactoring | After phases 0--7 | Speedup |
|---|---:|---:|---:|
| Python `legal_actions` | 21,473 calls/sec | 26,586 calls/sec | 1.24x |
| C++ `legal_action_codes` | 61,313 calls/sec | 118,594 calls/sec | 1.93x |
| C++ `legal_action_count` | 316,991 calls/sec | 981,149 calls/sec | 3.10x |
| C++ internal self-play | 160,545 moves/sec | 740,538 moves/sec | 4.61x |

On the same 250-action position, a 30-pair sustained A/B measured about 1.19x
for `legal_actions` (95% CI: 1.11--1.19x), 1.97x for codes, and 3.11x for
count. On a fixed position with only five legal actions, eliminating the old
fixed-buffer setup has a larger effect: 5.07x, 9.17x, and 9.62x respectively.
The speedup therefore depends on branching factor and on how much time is
spent constructing Python Action objects.

Compared numerically with the old README claims, the new values are 1.33x for
`legal_actions` (20,000 to 26,586 calls/sec), 2.97x for count (330,000 to
981,149 calls/sec), and 4.63x for self-play (160,000 to 740,538 moves/sec).
Use the same-condition table or paired A/B ratios to assess the refactoring.

With NN inference excluded, a 256-simulation native synthetic MCTS search
improved from 70,690 to 94,427 simulations/sec without determinization (1.34x)
and from 65,001 to 108,441 simulations/sec with determinization (1.67x). A
copy-focused microbenchmark with history length 200 and determinization is
14.7x faster, while history-free searches are roughly unchanged. End-to-end
speedup with a real model depends on the fraction of time spent in inference.

### Experimental parallel MCTS

Shared-tree parallel search is available as a Stage B experimental opt-in.
`num_threads=1` uses a low-overhead serial path; `num_threads>=2` uses native
traversal workers and one inference coordinator. Python evaluator callbacks are
synchronous and never overlap within one search. `DETERMINISTIC_EPOCH` is a
serial trace/replay oracle, not a parallel speed mode, and `timeout_ms` is a
soft deadline that does not preempt a callback already in progress.

`ParallelSearchOptions.max_tree_nodes` defaults to 50,000. It limits the one
shared tree, or the aggregate of all active worker trees in root-parallel mode.
If capacity is reached after the root is expanded, the API returns a partial
result and falls back to the normalized legal root prior/noise when root visits
are still zero; capacity before root expansion raises
`TreeCapacityReachedError`. Root-parallel Python callbacks recheck timeout and
cancellation after acquiring their serialization mutex, so stale callback
waiters are not drained after an overrun.

The default remains one thread. Scheduled soak, variable scheduler seeds, a
real-NN fixed-time quality canary, and a secondary feature signature for
already-expanded nodes remain stable-release gates. See the
[parallel-search implementation status](https://github.com/kuboyoo/csplendor/blob/main/doc/parallel_search_plan/implementation_status.md)
for the current contract and limitations.

## Installation & Build

### Prerequisites
- C++17 compatible compiler, such as GCC 9+
- CMake 3.13+
- Python 3.8+

Build dependencies and NumPy are installed from package metadata. FastAPI is
optional: use `pip install "csplendor[web]"` when running the web service.

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
pip install "csplendor[web]"
uvicorn csplendor.api:app --reload
```

Game/session/replay endpoints are standalone. The legacy `/ai_move` bridge is
an optional compatibility integration and lazily requires both torch and the
external `dlsplendor` package; models and NN search code are intentionally not
bundled in csplendor. If that stack is absent, the endpoint returns HTTP 503
without affecting the rule engine or other web endpoints.

The legacy `.pkl` replay viewer invokes pickle and must only read trusted local
files placed in the configured replay data directory by the server
administrator. `/replay/load` accepts only `.pkl` files inside that directory;
arbitrary paths resolving outside it, path traversal, and symlinks escaping the
directory are rejected. `/replay/files` neither exposes absolute paths nor
unpickles files while listing them. Never place uploads or other untrusted input
in this directory.

## Documentation
For detailed specifications, see the `doc/` directory:
- [Technical Overview](https://github.com/kuboyoo/csplendor/blob/main/doc/overview.md)
- [Engine Specs](https://github.com/kuboyoo/csplendor/blob/main/doc/engine_specs.md)
- [Python API Reference](https://github.com/kuboyoo/csplendor/blob/main/doc/api_ref.md)
- [ML Integration Guide](https://github.com/kuboyoo/csplendor/blob/main/doc/ml_integration.md)
- [Web API Reference](https://github.com/kuboyoo/csplendor/blob/main/doc/web_api.md)
- [Release Validation Record](https://github.com/kuboyoo/csplendor/blob/main/doc/release_validation.md)

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
- **Forced pass**: `Game.legal_actions` returns one `ActionType.PASS` only when
  no ordinary move exists. The 48-slot MCTS policy omits this forced choice;
  call `Game.apply_forced_pass()` before searching such a root.
- **Seed portability**: a fixed `Game(seed)` is reproducible with the same C++
  standard-library implementation, but initial/deck shuffles are not promised
  to match across libstdc++, libc++, and MSVC. Native parallel MCTS uses its
  separately versioned portable RNG contract.
