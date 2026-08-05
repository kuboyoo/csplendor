"""Isolated reader for administrator-controlled legacy pickle replays.

Python pickle is an executable, unsafe format. This reader accepts only regular
files contained by a configured server directory, enforces a byte limit, and is
never used while listing files. It must not be exposed to user-uploaded data.
"""

from __future__ import annotations

import glob
import os
import pickle
import stat
from typing import Any, Dict, List, Tuple

from .application_errors import InvalidRequest, PayloadTooLarge, ResourceNotFound

UNSAFE_FORMAT_WARNING = (
    "Legacy pickle replay: load only administrator-controlled trusted files"
)


class LegacyPickleReplayReader:
    def __init__(self, data_dir: str, *, max_bytes: int) -> None:
        self.data_dir = os.path.realpath(data_dir)
        self.max_bytes = int(max_bytes)

    def resolve(self, path: str) -> str:
        if not isinstance(path, str) or not path.strip():
            raise InvalidRequest("Replay filename is required")
        if "\0" in path:
            raise InvalidRequest("Invalid replay filename")
        try:
            candidate = (
                path if os.path.isabs(path) else os.path.join(self.data_dir, path)
            )
            candidate = os.path.realpath(candidate)
        except (OSError, ValueError) as error:
            raise InvalidRequest("Invalid replay filename") from error
        try:
            contained = (
                os.path.commonpath([self.data_dir, candidate]) == self.data_dir
            )
        except ValueError:
            contained = False
        if not contained:
            raise InvalidRequest(
                "Replay path must stay inside the configured data directory"
            )
        if not candidate.endswith(".pkl"):
            raise InvalidRequest("Only .pkl files are supported")
        try:
            if not os.path.isfile(candidate):
                raise ResourceNotFound("Replay file not found")
            if os.path.getsize(candidate) > self.max_bytes:
                raise PayloadTooLarge("Replay file is too large")
        except (InvalidRequest, ResourceNotFound, PayloadTooLarge):
            raise
        except OSError as error:
            raise ResourceNotFound("Replay file not found") from error
        return candidate

    def list_files(self) -> List[Dict[str, Any]]:
        files: List[Dict[str, Any]] = []
        if not os.path.isdir(self.data_dir):
            return files
        for pickle_path in sorted(
            glob.glob(os.path.join(self.data_dir, "*.pkl"))
        ):
            try:
                resolved = os.path.realpath(pickle_path)
                if os.path.commonpath([self.data_dir, resolved]) != self.data_dir:
                    continue
                if not os.path.isfile(resolved):
                    continue
                size_bytes = os.path.getsize(resolved)
            except (OSError, ValueError):
                continue
            files.append(
                {
                    "filename": os.path.basename(pickle_path),
                    "path": os.path.basename(pickle_path),
                    "num_examples": None,
                    "size_mb": round(size_bytes / 1024 / 1024, 2),
                }
            )
        return files

    def load(self, path: str) -> Tuple[str, Any]:
        resolved = self.resolve(path)
        file_descriptor = None
        try:
            flags = os.O_RDONLY
            flags |= getattr(os, "O_BINARY", 0)
            flags |= getattr(os, "O_CLOEXEC", 0)
            flags |= getattr(os, "O_NOFOLLOW", 0)
            file_descriptor = os.open(resolved, flags)
            with os.fdopen(file_descriptor, "rb") as stream:
                file_descriptor = None
                file_stat = os.fstat(stream.fileno())
                if not stat.S_ISREG(file_stat.st_mode):
                    raise InvalidRequest("Invalid replay file")
                if file_stat.st_size > self.max_bytes:
                    raise PayloadTooLarge("Replay file is too large")
                data = pickle.load(stream)  # noqa: S301
        except (InvalidRequest, PayloadTooLarge, ResourceNotFound):
            raise
        except FileNotFoundError as error:
            raise ResourceNotFound("Replay file not found") from error
        except Exception as error:
            raise InvalidRequest("Failed to load replay") from error
        finally:
            if file_descriptor is not None:
                os.close(file_descriptor)
        return resolved, data
