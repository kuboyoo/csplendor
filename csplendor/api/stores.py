"""In-memory application stores and their replaceable interfaces."""

from __future__ import annotations

from typing import (
    Dict,
    Generic,
    Iterator,
    MutableMapping,
    Optional,
    Protocol,
    TypeVar,
    runtime_checkable,
)

T = TypeVar("T")


@runtime_checkable
class SessionStore(Protocol, Generic[T]):
    """Minimal session persistence contract used by application services."""

    def __contains__(self, key: object) -> bool: ...

    def __getitem__(self, key: str) -> T: ...

    def __setitem__(self, key: str, value: T) -> None: ...

    def get(self, key: str, default: Optional[T] = None) -> Optional[T]: ...

    def clear(self) -> None: ...


@runtime_checkable
class KifuStore(SessionStore[T], Protocol, Generic[T]):
    """Marker interface for KIFU replay and recording persistence."""


class InMemoryStore(MutableMapping[str, T], Generic[T]):
    """Dictionary-compatible store used by the current single-process API.

    Implementing ``MutableMapping`` keeps the historical test and embedding
    surface (indexing, ``get`` and ``clear``) while allowing a persistent store
    to be injected into the application services later.
    """

    def __init__(self) -> None:
        self._items: Dict[str, T] = {}

    def __getitem__(self, key: str) -> T:
        return self._items[key]

    def __setitem__(self, key: str, value: T) -> None:
        self._items[key] = value

    def __delitem__(self, key: str) -> None:
        del self._items[key]

    def __iter__(self) -> Iterator[str]:
        return iter(self._items)

    def __len__(self) -> int:
        return len(self._items)
