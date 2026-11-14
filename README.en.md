[日本語・詳細ガイド](README.md)

# csplendor

A C++17 rules engine for **two-player Splendor**, with Python bindings, legal move generation, state transitions, MCTS and mate-search helpers. Use it as the rules layer for AI training, game servers and replay tools. Models, training experiments, browser interfaces and hosted-service infrastructure belong to separate projects.

## Requirements and setup

Python 3.8+, a C++17 compiler, CMake 3.13+, setuptools 68+, wheel and pybind11 2.10+. Pip installs Python build dependencies and NumPy 1.20+. GPU/PyTorch is optional.

```bash
git clone https://github.com/kuboyoo/csplendor.git
cd csplendor
python -m venv .venv
source .venv/bin/activate
# Windows PowerShell: .venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e '.[dev,web]'
```

Re-run the editable install after C++ changes. Builds default to Release and `portable`. See [macOS build settings](doc/building.md) for Apple Silicon local builds and distribution wheels.

## Example

```python
import csplendor as cs

game = cs.Game(seed=42)
game.simple_payment_mode = False
assert game.apply(game.legal_actions[0])
features = cs.StateFeaturizer().featurize(game, observer=game.current_player)
assert features.shape == (196,)
snapshot = game.serialize_snapshot()
restored = cs.Game.deserialize_snapshot(snapshot)
assert restored.serialize_snapshot() == snapshot
```

A position occurring for the fourth time in a game (not necessarily consecutively) is a draw by repetition (`GameEndReason.REPETITION`).

`simple_payment_mode=True` chooses one minimum-gold payment per purchase; `False` enumerates every valid payment. Token-return choices remain available in both modes. **Game defaults to False, while HTTP POST /game defaults to True.** Set the mode explicitly for each match. Seed 0 is deterministic too.

## Benchmarks and specifications

The [Japanese README](README.md) contains the complete guide; focused documents provide the reference details:

| Topic | Reference |
|---|---|
| Performance | [Benchmark results and measurement conditions](doc/performance_benchmarks.md) |
| Rules | [Engine specification](doc/engine_specs.md) |
| Cards and nobles | [All 90 cards and 12 nobles: IDs, colors, costs, points](doc/card_catalog.md) |
| Python | [API reference](doc/api_ref.md) |
| AI inputs | [196-feature schema and action masks](doc/ml_integration.md) |
| Full action space | [V2: 4869 IDs](doc/action_space_v2.md), [V3: 3133 IDs](doc/action_space_v3.md), [V5: 3049 IDs, recommended](doc/action_space_v5.md) |
| Search | [Mate search](doc/mate_usage.md), [experimental parallel MCTS](doc/parallel_mcts_usage.md) |
| Persistence | [Full snapshot](doc/game_snapshot.md), [observer-safe identity](doc/information_state.md), [KIFU](doc/KIFU.md) |
| External engines | [USI (Universal Splendor Protocol) specification](doc/USI.md) |
| HTTP integration | [Web API](doc/web_api.md) |
| All documents | [Documentation index](doc/index.md) |

Measured on November 14, 2026 (main `e361b6e`, Apple M4 Pro, macOS 27.2, Apple clang 21, Python 3.13, portable Release; no neural-network inference): native legal-action counting 6.49M calls/s and packed generation 0.47M calls/s on a 190-action position, native random self-play 3.16M moves/s, `Game.legal_actions` 12k calls/s from Python on a 182-action position, and `V3SearchSession` 0.14M / 0.35M / 0.49M simulations/s with `num_threads` 1 / 4 / 10 (64 games × 400 simulations). The latest Ryzen 9 7900X / Linux measurements of earlier versions (August–October 2026, 250-action position under the earlier rules) are 3.07M legal counts/s, 3.09M random self-play moves/s and 0.16M / 0.46M / 0.72M simulations/s with 1 / 4 / 8 threads. The Japanese README table and the benchmark reference list every workload, its conditions and how to reproduce it.

Gem arrays use White, Blue, Green, Red, Black, Gold order; five-element arrays omit Gold. USI symbols are W, U, G, R, K, D. Card IDs, action-list indices, packed action codes and policy IDs are different identifiers. Built-in C++ MCTS uses a fixed **48-action** policy; V3/V5 cannot be substituted directly.

## Web integration

```bash
python -m uvicorn csplendor.api:app --host 127.0.0.1 --port 8000
```

OpenAPI is available at `http://127.0.0.1:8000/docs`. The included API stores sessions in process memory and returns hidden reservation IDs. A hosted competition service must add observer-specific responses, authentication, match locking/version checks, persistence and isolated AI workers. Full snapshots are server-only data. USI (**Universal Splendor Protocol**) is the text protocol over stdin/stdout between a Splendor game server and AI engines. USI process management belongs to the external AI/service; this package provides move and position conversion helpers.

## Validation

```bash
python -m pytest
python -m py_compile csplendor/*.py
# Opt-in performance checks
python -m pytest -m performance
```

License: [GPL-3.0](LICENSE).
