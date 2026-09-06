# csplendor Technical Documentation

Welcome to the technical documentation for the `csplendor` engine.

## Contents
- [Project Overview](overview.md) - High-level architecture and philosophies.
- [Engine Specifications](engine_specs.md) - Internal logic and data structures.
- [AI Engine Specification](ai_engine_spec.md) - Detailed guide for AI/ML developers covering action encoding and state machine phases.
- [Python API Reference](api_ref.md) - Manual for using the engine in Python scripts.
- [Machine Learning Integration Guide](ml_integration.md) - Guide for training AI models using featurization and action space encoding.
- [Versioned Game Snapshot](game_snapshot.md) - Portable current-state snapshots for replay and Reanalyze.
- [Information-State Identity](information_state.md) - Versioned observer-safe keys for opening analysis and persistent books.
- [Web API Reference](web_api.md) - Usage of the FastAPI backend.
- [Release Validation Record](release_validation.md) - Local release checks and remaining platform/publication gates.
- [MCTS Hot-path Optimizations](mcts_hotpath_optimizations.md) - Internal bitsets, compact edges, equivalence tests, and benchmark results.
- [Second Refactoring Plan](refactoring_plan_v2.md) - Completed R0--R8 architecture and compatibility refactoring record.
- [Refactoring Compatibility Contracts](refactoring_contracts.md) - Versioned API classification, state invariants, mutation, and ownership contracts.
- [R0 Refactoring Baseline](refactoring_plan/r0_baseline.md) - Reproducible build/include/runtime measurements and shared test support.
- [R1-A Binding Split](refactoring_plan/r1a_bindings.md) - Responsibility-specific pybind11 translation units and before/after validation.
- [R1-B1 Snapshot Compiled Core](refactoring_plan/r1b_snapshot.md) - Compiled snapshot serialization boundary and compatibility/performance validation.
- [R1-B2 Trace Compiled Core](refactoring_plan/r1b_trace.md) - Compiled deterministic trace serialization and replay validation.
- [R2-A State Invariants](refactoring_plan/r2a_invariants.md) - Profile-specific invariant diagnostics and stale-cache detection.
- [R2-B Mutation Gateway](refactoring_plan/r2b_mutation.md) - Fixed-capacity policies, validated editor updates, and trusted mutation boundaries.
- [R2-C State Copy Ownership](refactoring_plan/r2c_copy_ownership.md) - Field roles and full/search/snapshot/undo copy boundaries.
- [R3 Rule Boundaries](refactoring_plan/r3_rules.md) - Shared rule queries, transitions, and differential compatibility.
- [R4 Encoding Schema](refactoring_plan/r4_encoding_schema.md) - Versioned action/state descriptors and golden equivalence.
- [R5 MCTS Ownership](refactoring_plan/r5_mcts_ownership.md) - Facade, tree, scheduler, and session ownership boundaries.
- [R6 Solver Tooling](refactoring_plan/r6_solver_tooling.md) - C++ solver facades and Python solver/puzzle module split.
- [R7 API Services](refactoring_plan/r7_api_services.md) - Application services, stores, USI/replay, and optional AI boundary.
- [R8 Quality and Documentation](refactoring_plan/r8_quality_docs.md) - Test taxonomy, native suites, coverage, and final documentation alignment.

---

## Getting Started

### For GUI Development
Focus on the **[Web API Reference](web_api.md)**. The JSON state provided by the API contains everything needed to render the board, player hands, and available moves.

### For AI Training
Focus on the **[ML Integration Guide](ml_integration.md)**. Use the `StateFeaturizer` to feed your neural network and the `ActionEncoder` to handle the discrete action space.

### For Custom Game Logic
Focus on the **[Python API Reference](api_ref.md)** and **[Engine Specifications](engine_specs.md)** to understand how to interact with the core simulation directly.
