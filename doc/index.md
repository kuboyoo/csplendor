# csplendor Technical Documentation

Welcome to the technical documentation for the `csplendor` engine.

## Contents
- [Project Overview](overview.md) - High-level architecture and philosophies.
- [Engine Specifications](engine_specs.md) - Internal logic and data structures.
- [AI Engine Specification](ai_engine_spec.md) - Detailed guide for AI/ML developers covering action encoding and state machine phases.
- [Python API Reference](api_ref.md) - Manual for using the engine in Python scripts.
- [Machine Learning Integration Guide](ml_integration.md) - Guide for training AI models using featurization and action space encoding.
- [Versioned Game Snapshot](game_snapshot.md) - Portable current-state snapshots for replay and Reanalyze.
- [Web API Reference](web_api.md) - Usage of the FastAPI backend.
- [Release Validation Record](release_validation.md) - Local release checks and remaining platform/publication gates.
- [MCTS Hot-path Optimizations](mcts_hotpath_optimizations.md) - Internal bitsets, compact edges, equivalence tests, and benchmark results.
- [Second Refactoring Plan](refactoring_plan_v2.md) - Current architecture, compatibility contracts, priorities, and phased refactoring roadmap.
- [Refactoring Compatibility Contracts](refactoring_contracts.md) - Versioned API classification, state invariants, mutation, and ownership contracts.
- [R0 Refactoring Baseline](refactoring_plan/r0_baseline.md) - Reproducible build/include/runtime measurements and shared test support.

---

## Getting Started

### For GUI Development
Focus on the **[Web API Reference](web_api.md)**. The JSON state provided by the API contains everything needed to render the board, player hands, and available moves.

### For AI Training
Focus on the **[ML Integration Guide](ml_integration.md)**. Use the `StateFeaturizer` to feed your neural network and the `ActionEncoder` to handle the discrete action space.

### For Custom Game Logic
Focus on the **[Python API Reference](api_ref.md)** and **[Engine Specifications](engine_specs.md)** to understand how to interact with the core simulation directly.
