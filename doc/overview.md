# csplendor Project Overview

`csplendor` is a high-performance engine for the board game Splendor, written in C++17 with Python bindings. It is designed specifically for 2-player competitive play and machine learning research.

## Core Philosophies
1. **Performance First**: The game logic is implemented in C++ to allow tens of thousands of simulations per second, essential for Reinforcement Learning (RL) and Monte Carlo Tree Search (MCTS).
2. **ML Ready**: Built-in support for converting game states into normalized vectors (features) and mapping actions to a fixed-size index space.
3. **Rule Compliance**: Strictly follows the standard 2-player Splendor rules, including the "no-noble-choice" simplification (when only one noble visits) and the gem return mechanics.
4. **Extensibility**: A modular architecture that separates data (cards/nobles) from logic (game rules) and interface (Python/FastAPI).

## Technology Stack
- **Core Engine**: C++17
- **Bindings**: pybind11
- **ML Utilities**: Python, NumPy
- **Web Interface**: FastAPI, Pydantic
- **Build System**: CMake, setuptools

## Repository Structure
- `src/`: C++ source files and headers.
- `csplendor/`: Python package wrapper.
    - `api/`: FastAPI web server and Pydantic schemas.
    - `features.py`: State featurization logic.
    - `action_space.py`: Action encoding/decoding.
- `doc/`: Technical documentation (this directory).
- `test_*.py`: Verification and performance test scripts.
