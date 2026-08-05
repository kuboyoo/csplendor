#!/usr/bin/env python3
"""Measure and compare refactoring build/include baselines.

Create artifacts outside the repository so compiler- and host-specific values
do not become source-controlled golden data:

    python scripts/refactor_build_baseline.py \
      --label before --build-dir /tmp/csplendor-build-before \
      --output /tmp/csplendor-build-before.json
    python scripts/refactor_build_baseline.py \
      --compare /tmp/csplendor-build-before.json /tmp/candidate.json
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

try:
    import resource
except ImportError:  # pragma: no cover - unavailable on Windows
    resource = None


ROOT = Path(__file__).resolve().parents[1]
SCHEMA_VERSION = 1
REGRESSION_LIMIT = 1.05
LOCAL_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def _command_version(command):
    try:
        output = subprocess.check_output(
            [command, "--version"], text=True, stderr=subprocess.STDOUT
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return output.splitlines()[0]


def _git_revision():
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=ROOT,
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def _child_peak_rss_kib():
    if resource is None:
        return None
    value = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
    # Darwin reports bytes; Linux and the BSDs used by CI report KiB.
    if sys.platform == "darwin":
        value //= 1024
    return int(value)


def _run_timed(command):
    started = time.perf_counter()
    completed = subprocess.run(
        command,
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    elapsed = time.perf_counter() - started
    if completed.returncode:
        tail = "\n".join(completed.stdout.splitlines()[-80:])
        raise RuntimeError(
            "command failed ({}): {}\n{}".format(
                completed.returncode, " ".join(map(str, command)), tail
            )
        )
    return {
        "seconds": elapsed,
        # This is an upper bound after all children completed so far. It is
        # portable enough for paired runs, but not a cross-host absolute RSS.
        "max_child_rss_kib_so_far": _child_peak_rss_kib(),
    }


def collect_include_graph(source_dir):
    """Return direct and transitive local include graph statistics."""
    source_dir = Path(source_dir)
    files = sorted(
        path
        for suffix in ("*.h", "*.cpp")
        for path in source_dir.rglob(suffix)
        if path.is_file()
    )
    known = {path.resolve(): path.relative_to(source_dir).as_posix() for path in files}
    edges = set()
    unresolved = set()
    for source in files:
        for include in LOCAL_INCLUDE.findall(source.read_text(encoding="utf-8")):
            candidates = (
                (source.parent / include).resolve(),
                (source_dir / include).resolve(),
            )
            target = next((path for path in candidates if path in known), None)
            if target is not None:
                edges.add((known[source.resolve()], known[target]))
            else:
                unresolved.add((known[source.resolve()], include))

    adjacency = {name: set() for name in known.values()}
    for source, target in edges:
        adjacency[source].add(target)

    def closure(start):
        pending = list(adjacency[start])
        seen = set()
        while pending:
            item = pending.pop()
            if item in seen:
                continue
            seen.add(item)
            pending.extend(adjacency.get(item, ()))
        seen.discard(start)
        return seen

    transitive = {name: len(closure(name)) for name in sorted(adjacency)}
    direct = {name: len(adjacency[name]) for name in sorted(adjacency)}
    translation_units = [name for name in adjacency if name.endswith(".cpp")]
    return {
        "node_count": len(files),
        "translation_unit_count": len(translation_units),
        "direct_edge_count": len(edges),
        "unresolved_local_includes": [list(item) for item in sorted(unresolved)],
        "direct_dependency_count": direct,
        "transitive_dependency_count": transitive,
        "max_transitive_dependency_count": max(transitive.values(), default=0),
        "max_translation_unit_transitive_dependency_count": max(
            (transitive[name] for name in translation_units), default=0
        ),
    }


def _find_extension(build_dir):
    candidates = []
    for pattern in ("_csplendor*.so", "_csplendor*.pyd", "_csplendor*.dylib"):
        candidates.extend(Path(build_dir).rglob(pattern))
    files = sorted(path for path in candidates if path.is_file())
    if len(files) != 1:
        raise RuntimeError(f"expected one built extension, found: {files}")
    return files[0]


def collect_build(label, build_dir, parallel):
    build_dir = Path(build_dir).resolve()
    if build_dir == ROOT or ROOT in build_dir.parents:
        raise ValueError("build directory must be outside the repository")
    if build_dir.exists() and any(build_dir.iterdir()):
        raise ValueError("build directory must not exist or must be empty")
    build_dir.mkdir(parents=True, exist_ok=True)

    try:
        import pybind11
    except ImportError as error:
        raise RuntimeError("pybind11 is required to measure the binding build") from error

    cmake = shutil.which("cmake")
    if cmake is None:
        raise RuntimeError("cmake is required")
    compiler = os.environ.get("CXX", "c++")
    configure = [
        cmake,
        "-S",
        str(ROOT),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        "-DCSPLENDOR_BUILD_PYTHON_MODULE=ON",
        "-DCSPLENDOR_BUILD_NATIVE_TESTS=OFF",
        "-DCSPLENDOR_CPU_TARGET=portable",
        "-Dpybind11_DIR=" + pybind11.get_cmake_dir(),
    ]
    build = [cmake, "--build", str(build_dir), "--parallel", str(parallel)]
    configure_result = _run_timed(configure)
    clean_build_result = _run_timed(build)
    incremental_result = _run_timed(build)
    extension = _find_extension(build_dir)

    return {
        "schema_version": SCHEMA_VERSION,
        "label": label,
        "revision": _git_revision(),
        "environment": {
            "platform": platform.platform(),
            "machine": platform.machine(),
            "python": platform.python_version(),
            "cmake": _command_version(cmake),
            "compiler": _command_version(compiler),
            "cpu_target": "portable",
            "parallel": parallel,
        },
        "include_graph": collect_include_graph(ROOT / "src"),
        "build": {
            "configure": configure_result,
            "clean": clean_build_result,
            "noop_incremental": incremental_result,
            "extension_size_bytes": extension.stat().st_size,
            "extension_name": extension.name,
        },
    }


def _load(path):
    payload = json.loads(Path(path).read_text(encoding="utf-8"))
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported build baseline schema")
    return payload


def compare_payloads(baseline, candidate):
    keys = ("platform", "machine", "python", "cmake", "compiler", "cpu_target", "parallel")
    mismatches = {
        key: [baseline["environment"].get(key), candidate["environment"].get(key)]
        for key in keys
        if baseline["environment"].get(key) != candidate["environment"].get(key)
    }
    metric_paths = {
        "clean_build_seconds": ("clean", "seconds"),
        "clean_peak_rss_kib": ("clean", "max_child_rss_kib_so_far"),
        "noop_incremental_seconds": ("noop_incremental", "seconds"),
        "extension_size_bytes": ("extension_size_bytes",),
    }
    metrics = {}
    for name, path in metric_paths.items():
        before = baseline["build"]
        after = candidate["build"]
        for part in path:
            before = before[part]
            after = after[part]
        ratio = after / before if before is not None and after is not None and before else None
        metrics[name] = {
            "baseline": before,
            "candidate": after,
            "candidate_over_baseline": ratio,
            "over_five_percent": ratio is not None and ratio > REGRESSION_LIMIT,
        }
    return {
        "schema_version": SCHEMA_VERSION,
        "environment_mismatches": mismatches,
        "metrics": metrics,
        "include_graph": {
            "baseline_edges": baseline["include_graph"]["direct_edge_count"],
            "candidate_edges": candidate["include_graph"]["direct_edge_count"],
            "baseline_max_translation_unit_dependencies": baseline[
                "include_graph"
            ]["max_translation_unit_transitive_dependency_count"],
            "candidate_max_translation_unit_dependencies": candidate[
                "include_graph"
            ]["max_translation_unit_transitive_dependency_count"],
        },
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--label", default="local")
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--parallel", type=int, default=2)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--compare", nargs=2, metavar=("BASELINE", "CANDIDATE"))
    parser.add_argument("--graph-only", action="store_true")
    args = parser.parse_args()
    if args.parallel < 1:
        parser.error("--parallel must be positive")

    if args.compare:
        payload = compare_payloads(_load(args.compare[0]), _load(args.compare[1]))
    elif args.graph_only:
        payload = {
            "schema_version": SCHEMA_VERSION,
            "label": args.label,
            "revision": _git_revision(),
            "include_graph": collect_include_graph(ROOT / "src"),
        }
    elif args.build_dir:
        payload = collect_build(args.label, args.build_dir, args.parallel)
    else:
        parser.error("use --build-dir, --graph-only, or --compare")

    rendered = json.dumps(payload, indent=2, sort_keys=True)
    if args.output:
        args.output.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)


if __name__ == "__main__":
    main()
