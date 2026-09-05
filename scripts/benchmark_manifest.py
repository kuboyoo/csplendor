#!/usr/bin/env python3
"""Create a reproducibility manifest for csplendor native benchmarks.

Only operating-system metadata, executable/package paths, and an exact
allowlist of non-secret CMake build fields are inspected.  Repository secrets
(in particular ``.env`` and credentials) are never opened by this script.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Sequence


SCHEMA = "csplendor.benchmark_manifest.v1"
ROOT = Path(__file__).resolve().parents[1]

# Do not serialize arbitrary CMake cache entries.  Cache files can contain
# credentials supplied by an unrelated toolchain/package, so this list is
# intentionally exact rather than prefix based.
_SAFE_CMAKE_CACHE_KEYS = frozenset(
    {
        "CMAKE_BUILD_TYPE",
        "CMAKE_CXX_COMPILER",
        "CMAKE_CXX_FLAGS",
        "CMAKE_CXX_FLAGS_DEBUG",
        "CMAKE_CXX_FLAGS_MINSIZEREL",
        "CMAKE_CXX_FLAGS_RELEASE",
        "CMAKE_CXX_FLAGS_RELWITHDEBINFO",
        "CMAKE_EXE_LINKER_FLAGS",
        "CMAKE_EXE_LINKER_FLAGS_DEBUG",
        "CMAKE_EXE_LINKER_FLAGS_MINSIZEREL",
        "CMAKE_EXE_LINKER_FLAGS_RELEASE",
        "CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO",
        "CMAKE_GENERATOR",
        "CMAKE_GENERATOR_INSTANCE",
        "CMAKE_GENERATOR_PLATFORM",
        "CMAKE_GENERATOR_TOOLSET",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE",
        "CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO",
        "CMAKE_POSITION_INDEPENDENT_CODE",
        "CSPLENDOR_BUILD_ENGINE_BENCHMARK",
        "CSPLENDOR_BUILD_NATIVE_TESTS",
        "CSPLENDOR_BUILD_PARALLEL_BENCHMARK",
        "CSPLENDOR_BUILD_PYTHON_MODULE",
        "CSPLENDOR_CARD_EQUIVALENCE_CLASSES",
        "CSPLENDOR_CLOSED_FORM_RETURN_COUNT",
        "CSPLENDOR_COMPACT_FORCED_ACTIONS",
        "CSPLENDOR_COMPACT_SOLVER_REASONS",
        "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES",
        "CSPLENDOR_CACHE_REVEAL_SCORES",
        "CSPLENDOR_REUSE_SEARCH_SCRATCH",
        "CSPLENDOR_SOLVER_NORMAL_ROLLBACK",
        "CSPLENDOR_VERIFY_SOLVER_ROLLBACK",
        "CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER",
        "CSPLENDOR_CPU_TARGET",
        "CSPLENDOR_INCREMENTAL_EXACT_HASH",
        "CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE",
        "CSPLENDOR_NOBLE_ELIGIBILITY_TABLE",
        "CSPLENDOR_PACKED_CODE_SINK",
        "CSPLENDOR_PERF_INSTRUMENTATION",
        "CSPLENDOR_RETURN_PATTERN_TABLE",
        "CSPLENDOR_SANITIZER",
        "CSPLENDOR_SINGLE_PASS_LEGAL_CODES",
        "CSPLENDOR_SOLVER_PATH_STACK",
        "CSPLENDOR_VERIFY_INCREMENTAL_HASH",
        "CSPLENDOR_VERIFY_REVEAL_SEARCH_STATE",
    }
)
_FLAG_CACHE_KEYS = frozenset(
    key
    for key in _SAFE_CMAKE_CACHE_KEYS
    if key.startswith(("CMAKE_CXX_FLAGS", "CMAKE_EXE_LINKER_FLAGS"))
)
_BENCHMARK_BUILD_KEYS = _SAFE_CMAKE_CACHE_KEYS - {
    # These only decide which sibling targets are generated; they do not alter
    # benchmark_engine_hotpaths or csplendor_core compile options.
    "CSPLENDOR_BUILD_NATIVE_TESTS",
    "CSPLENDOR_BUILD_PARALLEL_BENCHMARK",
    "CSPLENDOR_BUILD_PYTHON_MODULE",
    # This is the explicit Phase-1A A/B experiment axis. Preserve its value in
    # each manifest, but do not reject the intended OFF/ON comparison solely
    # because this one option differs. Verification remains part of the build
    # fingerprint and must match across throughput binaries.
    "CSPLENDOR_INCREMENTAL_EXACT_HASH",
    # Phase-2A experiment axis. Keep the OFF/ON value in each manifest while
    # allowing the paired runner to compare the two otherwise-identical builds.
    "CSPLENDOR_NOBLE_ELIGIBILITY_TABLE",
    # Phase-2B code-materialization experiment axis.
    "CSPLENDOR_SINGLE_PASS_LEGAL_CODES",
    # Phase-2B token-return count experiment axis.
    "CSPLENDOR_CLOSED_FORM_RETURN_COUNT",
    # Phase-2B token-return emit experiment axis.
    "CSPLENDOR_RETURN_PATTERN_TABLE",
    # Phase-2B packed legal-code sink experiment axis.
    "CSPLENDOR_PACKED_CODE_SINK",
    # Phase-3A recursive path-container experiment axis.
    "CSPLENDOR_SOLVER_PATH_STACK",
    # Phase-3A reveal-card equivalence experiment axis.
    "CSPLENDOR_CARD_EQUIVALENCE_CLASSES",
    # Phase-3A attacker-action scratch experiment axis.
    "CSPLENDOR_COMPACT_FORCED_ACTIONS",
    # Phase-3A visible-solver TT metadata experiment axis.
    "CSPLENDOR_COMPACT_SOLVER_REASONS",
    # Phase-3B reveal-search sidecar experiment axis.
    "CSPLENDOR_INCREMENTAL_REVEAL_SEARCH_STATE",
    # Phase-3C solver transposition-table representation experiment axis.
    "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES",
    # Phase-3D-P1 scoring experiment axis (verification is NOT an axis).
    "CSPLENDOR_CACHE_REVEAL_SCORES",
    # Phase-3D-P2 temporary storage experiment axis.
    "CSPLENDOR_REUSE_SEARCH_SCRATCH",
    # Phase-3D-1 ordinary transition rollback experiment axis.
    "CSPLENDOR_SOLVER_NORMAL_ROLLBACK",
}
_SAFE_FLAG_PREFIXES = (
    "-march=",
    "-mtune=",
    "-mcpu=",
    "-mno-",
    "-fsanitize=",
    "-fno-sanitize=",
    "-std=",
)
_SAFE_FLAG_LITERALS = frozenset(
    {
        "-DNDEBUG",
        "-fno-lto",
        "-fno-omit-frame-pointer",
        "-fomit-frame-pointer",
        "-pg",
        "-pthread",
    }
)


def _command_output(command: Sequence[str], *, cwd: Path | None = None) -> str | None:
    try:
        result = subprocess.run(
            list(command),
            cwd=cwd,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return result.stdout.strip()


def sha256_file(path: Path | str) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path | str, payload: Any) -> None:
    """Durably replace *path* with one complete JSON document."""
    destination = Path(path)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=destination.parent,
            prefix=f".{destination.name}.",
            suffix=".tmp",
            delete=False,
        ) as stream:
            temporary_path = Path(stream.name)
            json.dump(payload, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary_path, destination)
        temporary_path = None
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def _tool_version(command: str, *arguments: str) -> dict[str, Any]:
    resolved = shutil.which(command)
    result = {
        "command": command,
        "path": resolved,
        "version": _command_output([command, *arguments]) if resolved else None,
    }
    if resolved:
        resolved_path = Path(resolved).resolve()
        if resolved_path.is_file():
            result["resolved_path"] = str(resolved_path)
            result["sha256"] = sha256_file(resolved_path)
    return result


def _safe_flag_summary(value: str) -> dict[str, Any]:
    """Describe flags without exposing arbitrary ``-DNAME=value`` values."""
    try:
        tokens = shlex.split(value)
    except ValueError:
        tokens = value.split()
    safe_tokens = []
    for token in tokens:
        if (
            token in _SAFE_FLAG_LITERALS
            or re.fullmatch(r"-O(?:0|1|2|3|s|g|fast)", token)
            or re.fullmatch(r"-g(?:0|1|2|3|gdb(?:0|1|2|3)?)?", token)
            or re.fullmatch(r"-flto(?:=(?:auto|jobserver|[0-9]+))?", token)
            or any(token.startswith(prefix) for prefix in _SAFE_FLAG_PREFIXES)
        ):
            safe_tokens.append(token)
        elif token.startswith(("-fprofile-generate", "-fprofile-use")):
            # A profile directory can contain private filesystem information.
            safe_tokens.append(
                token.split("=", 1)[0] + ("=<redacted>" if "=" in token else "")
            )
    return {
        "sha256": hashlib.sha256(value.encode("utf-8")).hexdigest(),
        "token_count": len(tokens),
        "safe_tokens": safe_tokens,
        "redacted_token_count": len(tokens) - len(safe_tokens),
    }


def _resolve_cmake_cache(
    cmake_cache: Path | str | None, resolved_binary: Path | None
) -> Path | None:
    if cmake_cache is not None:
        candidate = Path(cmake_cache)
        if candidate.is_dir():
            candidate /= "CMakeCache.txt"
        if not candidate.is_file():
            raise FileNotFoundError(f"CMake cache not found: {candidate}")
        return candidate.resolve()
    if resolved_binary is None:
        return None
    for directory in (resolved_binary.parent, resolved_binary.parent.parent):
        candidate = directory / "CMakeCache.txt"
        if candidate.is_file():
            return candidate.resolve()
    return None


def _cmake_build_metadata(cache_path: Path | None) -> tuple[dict[str, Any], str | None]:
    if cache_path is None:
        return {"available": False}, None

    raw_entries: dict[str, str] = {}
    with cache_path.open("r", encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if not line or line.startswith(("#", "//")) or "=" not in line:
                continue
            declaration, value = line.rstrip("\n").split("=", 1)
            key = declaration.split(":", 1)[0]
            if key in _SAFE_CMAKE_CACHE_KEYS:
                raw_entries[key] = value

    # The fingerprint uses only the exact allowlist above.  Raw flag values are
    # deliberately not emitted; only their hash and harmless optimization
    # switches are shown in the artifact.
    canonical = json.dumps(raw_entries, sort_keys=True, separators=(",", ":"))
    benchmark_entries = {
        key: value for key, value in raw_entries.items() if key in _BENCHMARK_BUILD_KEYS
    }
    # Pre-3D-P1 reference trees cannot enable this verifier. Missing means OFF,
    # while ON must still prevent comparison with a deployment timing build.
    benchmark_entries.setdefault("CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER", "OFF")
    benchmark_entries.setdefault("CSPLENDOR_VERIFY_SOLVER_ROLLBACK", "OFF")
    benchmark_canonical = json.dumps(
        benchmark_entries, sort_keys=True, separators=(",", ":")
    )
    public_entries: dict[str, Any] = {}
    for key, value in sorted(raw_entries.items()):
        public_entries[key] = (
            _safe_flag_summary(value) if key in _FLAG_CACHE_KEYS else value
        )
    return (
        {
            "available": True,
            "cache_path": str(cache_path),
            "allowlisted_entries": public_entries,
            "allowlisted_fingerprint_sha256": hashlib.sha256(
                canonical.encode("utf-8")
            ).hexdigest(),
            "benchmark_build_fingerprint_sha256": hashlib.sha256(
                benchmark_canonical.encode("utf-8")
            ).hexdigest(),
        },
        raw_entries.get("CMAKE_CXX_COMPILER"),
    )


def _git_metadata(repo_root: Path) -> dict[str, Any]:
    revision = _command_output(["git", "rev-parse", "HEAD"], cwd=repo_root)
    branch = _command_output(["git", "branch", "--show-current"], cwd=repo_root)
    status = _command_output(
        ["git", "status", "--short", "--untracked-files=all"], cwd=repo_root
    )
    status_lines = status.splitlines() if status else []
    return {
        "root": str(repo_root.resolve()),
        "revision": revision,
        "branch": branch,
        "dirty": bool(status_lines),
        "status_short": status_lines,
    }


def _lscpu_metadata() -> dict[str, Any]:
    raw = _command_output(["lscpu", "--json"])
    if raw:
        try:
            document = json.loads(raw)
            fields: dict[str, str] = {}
            for entry in document.get("lscpu", []):
                field = str(entry.get("field", "")).rstrip(":")
                if field:
                    fields[field] = str(entry.get("data", ""))
            return {"source": "lscpu --json", "fields": fields}
        except (TypeError, ValueError):
            pass
    return {
        "source": "python",
        "fields": {"CPU(s)": str(os.cpu_count() or 0), "Machine": platform.machine()},
    }


def _selected_cpu_topology(cpus: Sequence[int]) -> dict[str, Any]:
    selected: dict[str, dict[str, str | None]] = {}
    selected_set = {int(cpu) for cpu in cpus}
    selected_sibling_pair = False
    for cpu in sorted(selected_set):
        root = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")

        def read(name: str) -> str | None:
            try:
                return (root / name).read_text(encoding="ascii").strip()
            except (OSError, UnicodeError):
                return None

        siblings = read("thread_siblings_list")
        entry = {
            "core_id": read("core_id"),
            "physical_package_id": read("physical_package_id"),
            "thread_siblings_list": siblings,
        }
        selected[str(cpu)] = entry
        if siblings:
            sibling_cpus: set[int] = set()
            for component in siblings.split(","):
                if "-" in component:
                    first, last = (int(part) for part in component.split("-", 1))
                    sibling_cpus.update(range(first, last + 1))
                else:
                    sibling_cpus.add(int(component))
            if len(sibling_cpus & selected_set) > 1:
                selected_sibling_pair = True
    return {
        "selected_cpu_details": selected,
        "selected_set_contains_smt_siblings": selected_sibling_pair,
    }


def _current_affinity() -> list[int]:
    if hasattr(os, "sched_getaffinity"):
        try:
            return sorted(os.sched_getaffinity(0))
        except OSError:
            pass
    return list(range(os.cpu_count() or 1))


def _governors(cpus: Sequence[int]) -> dict[str, str | None]:
    result: dict[str, str | None] = {}
    for cpu in cpus:
        path = Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")
        try:
            # This sysfs value is machine metadata, not repository/user data.
            result[str(cpu)] = path.read_text(encoding="ascii").strip()
        except (OSError, UnicodeError):
            result[str(cpu)] = None
    return result


def _python_package_metadata() -> dict[str, Any]:
    try:
        package = importlib.import_module("csplendor")
    except (ImportError, OSError) as error:
        return {"importable": False, "error_type": type(error).__name__}

    package_path = getattr(package, "__file__", None)
    core_path = None
    try:
        core = importlib.import_module("csplendor._csplendor")
        core_path = getattr(core, "__file__", None)
    except (ImportError, OSError):
        pass
    return {
        "importable": True,
        "version": getattr(package, "__version__", None),
        "package_path": str(Path(package_path).resolve()) if package_path else None,
        "core_path": str(Path(core_path).resolve()) if core_path else None,
    }


def collect_manifest(
    command: Sequence[str],
    *,
    binary: Path | str | None = None,
    repo_root: Path | str = ROOT,
    compiler: str | None = None,
    benchmark_affinity: Sequence[int] | None = None,
    cmake_cache: Path | str | None = None,
) -> dict[str, Any]:
    """Return a JSON-safe reproducibility manifest.

    ``benchmark_affinity`` is the CPU set that the runner will apply.  The
    process' current allowed set is recorded separately so accidental cpuset
    changes remain visible.
    """
    normalized_command = [str(part) for part in command]
    resolved_binary: Path | None = None
    binary_argument = str(binary) if binary is not None else None
    if binary_argument is None and normalized_command:
        binary_argument = normalized_command[0]
    if binary_argument:
        found = shutil.which(binary_argument)
        candidate = Path(found) if found else Path(binary_argument)
        if candidate.exists() and candidate.is_file():
            resolved_binary = candidate.resolve()
        elif binary is not None:
            raise FileNotFoundError(f"benchmark binary not found: {binary_argument}")

    allowed_affinity = _current_affinity()
    selected_affinity = (
        sorted({int(cpu) for cpu in benchmark_affinity})
        if benchmark_affinity is not None
        else allowed_affinity
    )
    uname = platform.uname()
    binary_metadata: dict[str, Any] | None = None
    if resolved_binary is not None:
        stat = resolved_binary.stat()
        binary_metadata = {
            "path": str(resolved_binary),
            "sha256": sha256_file(resolved_binary),
            "size_bytes": stat.st_size,
        }
    cache_path = _resolve_cmake_cache(cmake_cache, resolved_binary)
    build_metadata, configured_compiler = _cmake_build_metadata(cache_path)
    compiler_command = compiler or configured_compiler or os.environ.get("CXX", "c++")
    cpu_topology = _selected_cpu_topology(selected_affinity)

    return {
        "schema": SCHEMA,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "command": normalized_command,
        "binary": binary_metadata,
        "host": {
            "uname_a": _command_output(["uname", "-a"]),
            "uname": {
                "system": uname.system,
                "node": uname.node,
                "release": uname.release,
                "version": uname.version,
                "machine": uname.machine,
            },
            "cpu": {
                "logical_count": os.cpu_count(),
                "allowed_affinity": allowed_affinity,
                "benchmark_affinity": selected_affinity,
                "topology": _lscpu_metadata(),
                **cpu_topology,
                "governors": _governors(selected_affinity),
            },
        },
        "tools": {
            "compiler": _tool_version(compiler_command, "--version"),
            "cmake": _tool_version("cmake", "--version"),
            "python": {
                "executable": str(Path(sys.executable).resolve()),
                "version": sys.version,
                "implementation": platform.python_implementation(),
            },
        },
        "build": build_metadata,
        "git": _git_metadata(Path(repo_root)),
        "python_package": _python_package_metadata(),
    }


def _strip_remainder_marker(command: Sequence[str]) -> list[str]:
    result = list(command)
    if result and result[0] == "--":
        result.pop(0)
    return result


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--repo-root", type=Path, default=ROOT)
    parser.add_argument("--compiler")
    parser.add_argument("--cmake-cache", type=Path)
    parser.add_argument("--cpu", type=int, action="append", dest="cpus")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = _strip_remainder_marker(args.command)
    payload = collect_manifest(
        command,
        binary=args.binary,
        repo_root=args.repo_root,
        compiler=args.compiler,
        benchmark_affinity=args.cpus,
        cmake_cache=args.cmake_cache,
    )
    if args.output:
        atomic_write_json(args.output, payload)
    print(json.dumps(payload, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
