"""Optional web API exports.

Keep importing ``csplendor.api`` lightweight: notation codecs are also used by
core tooling that must not require FastAPI/Pydantic to be installed.
"""

from typing import Any

__all__ = ["app"]


def __getattr__(name: str) -> Any:
    if name == "app":
        from .app import app

        return app
    raise AttributeError(name)
