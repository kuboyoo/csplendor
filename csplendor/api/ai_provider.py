"""Lazy provider boundary for optional external AI implementations."""

from __future__ import annotations

import glob
import os
import time
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Protocol

from .application_errors import OptionalIntegrationUnavailable


@dataclass(frozen=True)
class AIRequest:
    ai_type: str
    time_limit: float
    use_determinization: bool
    num_simulations: Optional[int]
    options: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class AIDecision:
    action_index: int
    used_mode: str
    used_simulations: Optional[int]
    elapsed_ms: int


class AIProvider(Protocol):
    """External AI capability required by the web application."""

    def choose_action(self, game, request: AIRequest) -> AIDecision: ...

    def list_models(self) -> List[Dict[str, str]]: ...


def _default_project_root() -> str:
    return os.path.abspath(os.path.join(os.path.dirname(__file__), "../../.."))


class LegacyAIProvider:
    """Compatibility adapter around the historical in-repository manager.

    Importing this module does not import torch, inspect sibling repositories,
    or scan model directories. Those operations happen only after an explicit
    endpoint call.
    """

    _SEARCH_DIRS = (
        "alphazero-general/models3",
        "alphazero-general/temp",
        "alphazero-general/models",
        "models",
    )

    def __init__(self, project_root: Optional[str] = None) -> None:
        self._project_root = project_root

    def choose_action(self, game, request: AIRequest) -> AIDecision:
        try:
            from .ai_manager import AIIntegrationUnavailable, AIManager
        except (ImportError, OSError) as error:
            raise OptionalIntegrationUnavailable(
                "Optional AI integration is unavailable"
            ) from error
        try:
            manager = AIManager.get_instance()
            started = time.perf_counter()
            action_index = manager.get_best_action(
                game,
                ai_type=request.ai_type,
                time_limit=request.time_limit,
                use_determinization=request.use_determinization,
                num_simulations=request.num_simulations,
                az_options=request.options,
            )
            measured_ms = (time.perf_counter() - started) * 1000.0
        except AIIntegrationUnavailable as error:
            raise OptionalIntegrationUnavailable(str(error)) from error

        debug = getattr(manager, "_last_action_debug", {}) or {}
        used_mode = self._default_used_mode(request)
        if isinstance(debug.get("used_mode"), str):
            used_mode = debug["used_mode"]
        used_simulations = request.num_simulations
        if debug.get("actual_simulations") is not None:
            used_simulations = int(debug["actual_simulations"])
        elapsed_ms = measured_ms
        if debug.get("elapsed_ms") is not None:
            elapsed_ms = float(debug["elapsed_ms"])
        return AIDecision(
            action_index=int(action_index),
            used_mode=used_mode,
            used_simulations=used_simulations,
            elapsed_ms=int(elapsed_ms),
        )

    @staticmethod
    def _default_used_mode(request: AIRequest) -> str:
        ai_type = request.ai_type
        if ai_type == "deepsets":
            return "deepsets_mcts"
        if ai_type == "set_transformer":
            return "set_transformer_mcts"
        if ai_type == "nnue":
            return "nnue_ab"
        if ai_type == "alphazero":
            return "alphazero_mcts" if request.num_simulations else "alphazero_time"
        if ai_type == "genbu":
            return "genbu_mcts" if request.num_simulations else "genbu_time"
        return ai_type

    def list_models(self) -> List[Dict[str, str]]:
        root = self._project_root or _default_project_root()
        models: List[Dict[str, str]] = []
        seen_paths = set()
        for search_dir in self._SEARCH_DIRS:
            full_dir = os.path.join(root, search_dir)
            if not os.path.exists(full_dir):
                continue
            for pattern in ("*.pt", "*.pth.tar"):
                for filepath in glob.glob(os.path.join(full_dir, pattern)):
                    if filepath in seen_paths:
                        continue
                    seen_paths.add(filepath)
                    models.append(
                        {
                            "path": filepath,
                            "name": os.path.basename(filepath),
                            "display": os.path.relpath(filepath, root),
                            "dir": search_dir,
                        }
                    )
        models.sort(
            key=lambda model: (
                self._SEARCH_DIRS.index(model["dir"])
                if model["dir"] in self._SEARCH_DIRS
                else 999,
                model["name"],
            )
        )
        return models


_provider: AIProvider = LegacyAIProvider()


def get_ai_provider() -> AIProvider:
    return _provider


def set_ai_provider(provider: AIProvider) -> None:
    """Inject a provider for embedding applications and contract tests."""

    global _provider
    _provider = provider


def reset_ai_provider() -> None:
    global _provider
    _provider = LegacyAIProvider()
