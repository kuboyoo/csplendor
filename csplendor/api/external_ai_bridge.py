"""Compatibility-only discovery for the external dlsplendor package."""

from __future__ import annotations

import importlib.util
import logging
import os
import sys
from typing import Optional

logger = logging.getLogger(__name__)


def activate_external_path(
    legacy_sibling: str,
    *,
    environment_variable: str,
    integration: str,
    prepend: bool = True,
) -> Optional[str]:
    """Activate one explicitly requested legacy source-tree integration."""

    configured = os.environ.get(environment_variable)
    candidate = configured or legacy_sibling
    if not os.path.isdir(candidate):
        return None
    resolved = os.path.abspath(candidate)
    if resolved not in sys.path:
        if prepend:
            sys.path.insert(0, resolved)
        else:
            sys.path.append(resolved)
        logger.warning(
            "Activated external AI compatibility path",
            extra={
                "event": "external_ai_path_activated",
                "integration": integration,
                "source": "environment" if configured else "legacy_sibling",
                "path": resolved,
            },
        )
    return resolved


def activate_dlsplendor_path(legacy_sibling: str) -> Optional[str]:
    """Make an explicitly configured or legacy sibling checkout importable.

    Normal csplendor imports never call this function. Installed packages take
    precedence; ``CSPLENDOR_DLSPLENDOR_PATH`` is the migration bridge, and the
    historical sibling path is the final rollback-compatible fallback.
    """

    if importlib.util.find_spec("dlsplendor") is not None:
        return None
    return activate_external_path(
        legacy_sibling,
        environment_variable="CSPLENDOR_DLSPLENDOR_PATH",
        integration="dlsplendor",
        prepend=False,
    )
