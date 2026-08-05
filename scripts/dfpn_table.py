"""Transposition-table ownership for the Python DFPN search."""

from __future__ import annotations

from typing import Any, Dict, Generic, TypeVar

NodeT = TypeVar("NodeT")


class DFPNTranspositionTable(Dict[Any, NodeT], Generic[NodeT]):
    """Owned table type that retains native ``dict`` hot-path operations."""


__all__ = ["DFPNTranspositionTable"]
