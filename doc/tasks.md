# Engine Refinement Tasks

This document outlines planned improvements to the `csplendor` engine to reach the performance and efficiency levels of world-class game engines (e.g., Shogi/Chess engines).

## 1. Zero Allocation & Memory Efficiency
Aim to eliminate all heap allocations during the MCTS search loop.

- [x] **Static Decks & Arrays**: Replace `std::vector` in `Board::decks` and `Board::nobles` with fixed-size `FixedStack` (stack-allocated).
- [x] **Fixed-Size Move List**: Implement a `MoveList` container (stack-allocated) to avoid `std::vector<Action>` allocations in `MoveGenerator`.
- [ ] **History Pre-allocation**: Pre-allocate `board_history` and `action_history` or use a fixed circular buffer for Undo support.

## 2. Incremental State Updates
Transition from "from scratch" calculations to incremental "delta" updates.

- [x] **Incremental Zobrist Hashing**: Implemented hash caching with invalidation on state changes. Hash is computed once and cached until invalidated.
- [ ] **Resource/Score Caching**: Maintain `packed_gems`, `packed_bonuses`, and `points` strictly via incremental updates to avoid periodic re-syncing.
- [x] **Noble Eligibility Mask**: Maintain a bitmask of eligible nobles for each player, updated only when a player's bonuses change via `sync_packed()`.

## 3. High-Performance Bit Manipulation
Leverage advanced bitwise operations and SIMD for low-level logic.

- [ ] **SIMD-based Affordability Check**: Explore 128-bit SIMD (SSE/AVX) to check card affordability across all 5 colors in a single clock cycle.
- [ ] **Bank & Inventory Masks**: Use bitmasking to quickly check if the bank has enough gems for a "Take 3" or "Take 2 same" action without looping.
- [ ] **Action Compression**: Compress the `Action` struct to the smallest possible bit-representation for cache efficiency.

## 4. Architectural Enhancements
Optimize the boundary between simulation and neural network inference.

- [~] **C++ MCTS Core Integration**: Migrate the MCTS search logic from Python to C++. Use Python only for neural network batch inference.
    - [x] Phase 1: Core data structures (`MCTSNode`, `MCTSConfig`, `MCTS` class) - DONE
    - [x] Phase 1: PUCT selection, backpropagation, node expansion - DONE
    - [x] Phase 1: Python bindings for MCTS classes - DONE
    - [ ] Phase 2: Batch inference interface and Python callback integration
    - [ ] Phase 3: Replace Python MCTS with C++ MCTS in training loop
- [ ] **Lightweight Undo (Copy-on-Write)**: Optimize `Board::clone_light()` or implement a robust `undo()` that is faster than cloning for node expansion.
- [ ] **Transposition Table Optimization**: Use a fixed-size, lock-less (if multithreaded) transposition table for storing search results across the MCTS tree.

## 5. Benchmarking & Profiling
Set up rigorous performance tracking.

- [ ] **Instruction Level Profiling**: Use `perf` or `valgrind --tool=callgrind` to identify remaining bottlenecks.
- [ ] **NPS (Nodes Per Second) Tracking**: Add a benchmark script to measure absolute search speed after each major optimization.

## 6. Hidden Information & Determinization (Combatting "Clairvoyance")
Address the problem where MCTS "knows" the future deck order during search, leading to unrealistic play.

- [x] **Observer-Aware Randomization**: Enhanced `randomize_hidden_information` to preserve only observer's knowledge while shuffling hidden info (deck order, opponent's hidden reserved cards).
- [x] **MCTS Search Determinization**:
    - **Implementation**: Added `--useDeterminization` flag to enable shuffled clones during MCTS search.
    - **Observable Hash**: Added `observable_hash(observer)` to ensure same observable states hash identically across determinizations.
- [ ] **Pool-based Deck Management**: Maintain a "seen cards" list to ensure randomization only recruits cards from the pool of truly unknown cards (e.g., cards already burned, on board, or in player hands must be excluded from shuffling).
- [x] **MCTS-Friendly Interface**:
    - [x] Added `Game.shuffled_clone(observer_player, seed)` to the C++ bindings. This allows `MCTS.py` to easily create a "fair" mental model of the game for simulation.
    - [x] `clone_light()` remains fast; `shuffled_clone()` adds randomization for MCTS expansion.
