# csplendor Technical Documentation

Welcome to the technical documentation for the `csplendor` engine.

## Contents
- [Project Overview](overview.md) - High-level architecture and philosophies.
- [Engine Specifications](engine_specs.md) - Internal logic and data structures.
- [AI Engine Specification](ai_engine_spec.md) - Detailed guide for AI/ML developers covering action encoding and state machine phases.
- [Python API Reference](api_ref.md) - Manual for using the engine in Python scripts.
- [Machine Learning Integration Guide](ml_integration.md) - Guide for training AI models using featurization and action space encoding.
- [Web API Reference](web_api.md) - Usage of the FastAPI backend.

---

## Getting Started

### For GUI Development
Focus on the **[Web API Reference](web_api.md)**. The JSON state provided by the API contains everything needed to render the board, player hands, and available moves.

### For AI Training
Focus on the **[ML Integration Guide](ml_integration.md)**. Use the `StateFeaturizer` to feed your neural network and the `ActionEncoder` to handle the discrete action space.

### For Custom Game Logic
Focus on the **[Python API Reference](api_ref.md)** and **[Engine Specifications](engine_specs.md)** to understand how to interact with the core simulation directly.
