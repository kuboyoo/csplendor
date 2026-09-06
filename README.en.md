# csplendor: High-Performance Splendor Engine

`csplendor` is a fast C++ based engine for the board game Splendor, optimized for 2-player competitive play and machine-learning workflows.

## Features

- **Fast logic**: C++17 rule transitions, legal-action generation and search. The latest cumulative comparison and historical measurements are separated below.
- **Python bindings**: Seamless integration via `pybind11`.
- **ML ready**: Built-in state featurization and action-space encoders.
- **Web API**: FastAPI integration for GUI development.

### Performance reference

#### Final candidate versus main (2026-09-06)

Direct paired A/B: `main f5ec6c5` versus measured engine `b202e6a`, Ryzen 9 7900X,
GCC 15.2, portable Release, 22 pairs / 11 blocks. Native LTO is OFF; both Python
extensions retain their pre-existing pybind11 LTO. Later documentation commits
are not different measured engine implementations.

| Workload | main | Candidate | Speedup [95% CI] |
|---|---:|---:|---:|
| Native legal count, 200k calls | 1,185,093 calls/s | 3,112,934 calls/s | 2.629 [2.615–2.651] |
| Native legal codes, 200k calls | 155,366 calls/s | 378,400 calls/s | 2.438 [2.429–2.447] |
| Native legal actions, 200k calls | 172,136 calls/s | 346,226 calls/s | 2.008 [1.959–2.057] |
| Exact reveal, depth 7, 1M nodes, independent repeat | 2,505.49 ms | 1,051.16 ms | 2.373 [2.361–2.403] |
| Python StateFeaturizer, 50k calls, independent repeat | 372.14 ms | 29.04 ms | 12.808 [12.514–13.048] |
| Python features + environment step, 50k moves, repeat | 529.33 ms | 89.37 ms | 6.044 [5.732–6.164] |

The generation rows measure native calls, not Python `legal_actions` retrieval.
Speedups are medians of crossover-block ratios, not ratios of the displayed
time/rate medians. The depth-7 fixture ends UNKNOWN at its node limit: this is
not the time to complete a seven-ply mate proof. Current solver RSS fell about
30%. Do not extrapolate these numbers to whole-AI or puzzle-saving throughput,
or multiply historical phase ratios.

Cumulative parallel-MCTS gains and the incremental benefit of opt-in LTO remain
unconfirmed. `CSPLENDOR_ENABLE_LTO` stays OFF by default. See the
[F1 report, CSV and manifest](doc/performance_experiments/final_main_vs_candidate_20260906.md).

F2 exercised real selfplay12/selfplay17 checkpoints on CPU through the existing
consumer: **Python MCTS, V3/3133 actions, canonical/public 313-feature encoding**,
not native 48-action MCTS or the optimized StateFeaturizer path. Real-model A/B,
browser rendering and GPU acceptance were not performed. F3's seven lint violations
in five added tests are fixed with import-only changes: **READY_FOR_REVIEW** locally.
Related tests (73) and Python 3.12 tests (595, coverage 58.89%) passed; remote CI
has not run and shipment is not approved. See the
[correction report](doc/performance_experiments/f3_lint_correction_20260906.md).
See [acceptance/review](doc/performance_experiments/f2_f3_shipping_review_20260906.md)
and [integration/rollback preparation](doc/performance_experiments/f4_integration_runbook_20260906.md).
No main merge, push or production installation has been executed.

#### Historical generation measurements (not the current candidate)

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

#### Historical MCTS search performance

Measured on 2026-08-04 on the same Ryzen 9 7900X with GCC 13 and a portable
Release build. The table compares the pre-optimization `main` (`6ddb47c`) with
the MCTS hot-path optimizations on the same host, seeds, tree size, and batch
size. Each result is the median of five samples using a zero-latency native
evaluator.

| Mode/backend | Pre-optimization `main` | Optimized | Speedup |
|---|---:|---:|---:|
| Exact legacy, 1 thread | 37,487 sim/s | 387,132 sim/s | 10.33x |
| Exact sharded, 1 thread | 31,773 sim/s | 222,253 sim/s | 7.00x |
| Exact sharded, 4 threads | 94,819 sim/s | 217,910 sim/s | 2.30x |
| Exact sharded, 8 threads | 125,095 sim/s | 194,405 sim/s | 1.55x |
| Exact root-parallel, 8 workers | 286,487 sim/s | 1,418,195 sim/s | 4.95x |
| Determinized legacy, 1 thread | 56,969 sim/s | 358,261 sim/s | 6.29x |
| Determinized sharded, 4 threads | 156,161 sim/s | 294,279 sim/s | 1.88x |
| Determinized root-parallel, 8 workers | 440,313 sim/s | 1,584,560 sim/s | 3.60x |

Component microbenchmarks improved the 48-action mask from 624.6 ns to 32.7
ns (19.10x), action decoding from 3,240.5 ns to 15.4 ns (210.07x), and dense
mask iteration from 22.0 ns to 4.7 ns (4.69x). Peak RSS for a 40,000-simulation
sharded eight-thread run fell by about 72%, from 159,832 KiB to 44,644 KiB.

End-to-end speedup with a real model depends on the fraction of time spent in
NN inference. See [MCTS hot-path optimizations](https://github.com/kuboyoo/csplendor/blob/main/doc/mcts_hotpath_optimizations.md)
for the methodology and details of the O(1) audit and compact-edge layout.

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

### macOS Apple Silicon CPU targets

`CSPLENDOR_CPU_TARGET` selects a distributable or locally optimized build from
the same source tree.

- `portable` (default): adds no CPU-specific flags. Always use this for
  distributed artifacts such as generic arm64 wheels.
- `native`: uses `-mcpu=native` for the local Apple Silicon CPU. On an M4 Pro,
  this produces M4-targeted code.

Set `CSPLENDOR_OSX_ARCHITECTURES` to `arm64`, `x86_64`, or `universal2` to
select the Python build architecture. Conflicting requests from `ARCHFLAGS`
and related frontend settings are rejected. Normal wheel builds also require
the selected architecture to match the platform tag, so cross-builds need a
matching Python or `_PYTHON_HOST_PLATFORM`.

Python extension examples:

```bash
# Generic arm64 wheel for distribution
MACOSX_DEPLOYMENT_TARGET=11.0 \
  CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=portable \
  python -m pip wheel . --wheel-dir dist/arm64

# Locally optimized extension for this Mac
CSPLENDOR_OSX_ARCHITECTURES=arm64 \
  CSPLENDOR_CPU_TARGET=native \
  python -m pip install -e .
```

Use separate build directories when invoking CMake directly:

```bash
cmake -S . -B build/macos-arm64-portable \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
  -DCSPLENDOR_CPU_TARGET=portable
cmake --build build/macos-arm64-portable --parallel 2

cmake -S . -B build/macos-m4-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCSPLENDOR_CPU_TARGET=native
cmake --build build/macos-m4-native --parallel 2
```

A `native` build is limited to editable installs or direct CMake builds. A
normal wheel tag cannot express an M4-only requirement, so native wheel builds
fail. Wheel builds also reject `--skip-build` so a binary from an earlier CPU
profile cannot be included. The temporary wheel used internally by a PEP 660
editable install is not distributable, so the distribution wheel
architecture/tag check does not apply to it. The examples use macOS 11.0, the
minimum Apple Silicon OS, as the deployment target; raise it to match the
supported OS range when needed. When the environment variable is omitted,
Python builds pass that Python installation's deployment target through to
CMake. A universal2 Python may use the `native` target for an editable or
direct CMake build when its process is running as arm64 and the extension is
arm64-only. The resulting extension cannot be loaded if that same Python is
launched as x86_64 under Rosetta. Native builds under Rosetta, universal2
native extensions, and non-Apple platforms remain unsupported. The wheel
compatibility tag is also constrained by the build Python's own minimum target,
so release validation must inspect both the Mach-O minimum OS and the wheel
tag.

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
- **Seed portability**: `Game(seed)` uses a repository-owned portable shuffle,
  so initial layouts and deck order match across libstdc++, libc++, and MSVC.
  Native parallel MCTS uses its separately versioned portable RNG contract.
