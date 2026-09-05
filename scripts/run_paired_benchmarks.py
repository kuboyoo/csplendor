#!/usr/bin/env python3
"""Collect and compare native csplendor benchmark JSONL with paired A/B runs.

Examples::

    python scripts/run_paired_benchmarks.py collect \
      --command './build/benchmark_engine_hotpaths --suite rules' \
      --cpu-set 4 --output /tmp/rules.json

    python scripts/run_paired_benchmarks.py paired \
      --baseline-command '/tmp/a/benchmark_engine_hotpaths --suite all' \
      --candidate-command '/tmp/b/benchmark_engine_hotpaths --suite all' \
      --cpu-set 4 --output /tmp/paired.json

    python scripts/run_paired_benchmarks.py compare /tmp/a.json /tmp/b.json

Commands are split with :mod:`shlex` and executed without a shell.  On Linux,
``taskset`` is preferred for affinity and GNU ``time`` supplies per-process
maximum RSS.  The native command must print one JSON object per non-empty line.
"""

from __future__ import annotations

import argparse
import errno
import hashlib
import json
import math
import os
import random
import shlex
import shutil
import signal
import stat
import statistics
import subprocess
import sys
import tempfile
import time
from copy import deepcopy
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

try:
    from benchmark_manifest import (
        ROOT,
        SCHEMA as MANIFEST_SCHEMA,
        atomic_write_json,
        collect_manifest,
        sha256_file,
    )
except ModuleNotFoundError:  # Importing as scripts.run_paired_benchmarks.
    from scripts.benchmark_manifest import (  # type: ignore[no-redef]
        ROOT,
        SCHEMA as MANIFEST_SCHEMA,
        atomic_write_json,
        collect_manifest,
        sha256_file,
    )


ENGINE_SCHEMA = "csplendor.engine_hotpath.v1"
RESULT_SCHEMA = "csplendor.paired_benchmark.v1"
DEFAULT_WARMUPS = 3
DEFAULT_PAIRS = 21
DEFAULT_BOOTSTRAP_ITERATIONS = 10_000
_DYNAMIC_RECORD_FIELDS = {
    "elapsed_ns",
    "rate_per_second",
    "rss_kib",
    "runner_rss_kib",
    "counters",
}

# Counter values have different contracts.  Configuration and correctness
# values must be bit-for-bit identical for Phase-0/non-algorithmic A/B runs.
# Instrumentation values may change, but the measurement schema (its key set)
# must not silently gain or lose fields.
_CONFIG_COUNTERS = frozenset({"instrumentation_enabled", "requested_node_limit"})
_CORRECTNESS_COUNTERS = frozenset(
    {
        "actions_per_call",
        "cancelled",
        "completed",
        "completed_evaluated",
        "completed_max_depth",
        "completed_terminal",
        "deck_cards",
        "deck_reserve_branches",
        "deck_reserve_candidates",
        "evaluated_boards",
        "duplicate_result",
        "failed",
        "game_resets",
        "inference_requests",
        "integrity_errors",
        "invalid_replay",
        "issued",
        "leaf_requests",
        "legal_actions",
        "legal_moves",
        "mask_popcount",
        "measured_root_visits",
        "memo_hits",
        "memoized_states",
        "nodes",
        "persistent_cache_states_after",
        "persistent_cache_states_before",
        "persistent_memo_hits",
        "purchase_transitions",
        "purchased_cards",
        "reservations_aborted",
        "reservations_committed",
        "reveal_branches",
        "root_q_digest",
        "root_visit_digest",
        "root_visits",
        "root_visits_before",
        "selected",
        "stale_result",
        "terminal_nodes",
        "terminal_paths",
        "tree_size",
        "tt_entries",
        "tt_hits",
        "tt_probes",
        "tt_stores",
        "virtual_loss_added",
        "virtual_loss_released",
        "hash_oracle_failures",
        "solver_reveal_state_oracle_failures",
    }
)
_MEASUREMENT_COUNTERS = frozenset(
    {
        "acquired_noble_vector_reallocations",
        "action_vector_reallocations",
        "board_restores",
        "board_snapshot_copies",
        "callback_batches",
        "callback_items",
        "clone_light_calls",
        "determinization_clone_calls",
        "evaluation_owners",
        "expansion_claimed",
        "expansion_published",
        "expansion_waited",
        "exact_deck_card_salts_visited",
        "exact_hash_cache_hits",
        "exact_hash_cache_misses",
        "exact_hash_calls",
        "exact_hash_fields_visited",
        "global_aligned_new_array_bytes",
        "global_aligned_new_array_calls",
        "global_aligned_new_bytes",
        "global_aligned_new_calls",
        "global_allocation_bytes",
        "global_allocation_calls",
        "global_new_array_bytes",
        "global_new_array_calls",
        "global_new_bytes",
        "global_new_calls",
        "inference_waiters",
        "max_inflight",
        "observable_hash_calls",
        "observable_hash_fields_visited",
        "parallel_access_epoch_updates",
        "parallel_batch_count",
        "parallel_batch_items",
        "parallel_coordinator_idle_nanoseconds",
        "parallel_edge_comparisons",
        "parallel_edge_lookups",
        "parallel_ledger_atomic_increments",
        "parallel_ledger_completion_atomic_increments",
        "parallel_ledger_error_atomic_increments",
        "parallel_ledger_evaluation_atomic_increments",
        "parallel_ledger_issuance_atomic_increments",
        "parallel_ledger_reservation_atomic_increments",
        "parallel_ledger_selection_atomic_increments",
        "parallel_live_reservation_allocations",
        "parallel_live_reservation_inserts",
        "parallel_live_reservation_rehashes",
        "parallel_node_lock_acquisitions",
        "parallel_node_lock_hold_nanoseconds",
        "parallel_node_lock_wait_nanoseconds",
        "parallel_queue_empty_waits",
        "parallel_queue_full_waits",
        "parallel_queue_wait_nanoseconds",
        "parallel_reservation_occupancy_0",
        "parallel_reservation_occupancy_1",
        "parallel_reservation_occupancy_2",
        "parallel_reservation_occupancy_3",
        "parallel_reservation_occupancy_4_plus",
        "parallel_reservation_occupancy_max",
        "parallel_reservation_occupancy_samples",
        "parallel_reservation_occupancy_sum",
        "parallel_shard_lock_acquisitions",
        "parallel_shard_lock_hold_nanoseconds",
        "parallel_shard_lock_wait_nanoseconds",
        "parallel_tree_lookups",
        "parallel_worker_idle_nanoseconds",
        "purchased_card_vector_reallocations",
        "solver_board_rollbacks",
        "solver_card_equivalence_lookups",
        "solver_visible_refill_score_calls",
        "solver_defender_reserve_score_calls",
        "solver_visible_refill_sort_candidates",
        "solver_defender_reserve_sort_candidates",
        "solver_is_claimed_calls",
        "solver_is_claimed_comparisons",
        "solver_path_erases",
        "solver_path_depth_0",
        "solver_path_depth_1_to_2",
        "solver_path_depth_3_to_4",
        "solver_path_depth_5_to_8",
        "solver_path_depth_9_plus",
        "solver_path_depth_max",
        "solver_path_depth_samples",
        "solver_path_depth_sum",
        "solver_path_finds",
        "solver_path_inserts",
        "solver_path_linear_comparisons",
        "solver_reveal_candidates",
        "solver_reveal_state_fallback_initializations",
        "solver_reveal_state_fast_initializations",
        "solver_reveal_state_fast_key_reads",
        "solver_reveal_state_oracle_checks",
        "solver_reveal_state_runtime_fallbacks",
        "solver_reveal_state_transitions",
        "solver_scanned_deck_cards",
        "solver_scanned_purchased_ids",
        "solver_set_deck_hash_calls",
        "solver_state_key_calls",
        "solver_state_key_fields_visited",
        "solver_temporary_set_allocations",
        "solver_temporary_vector_allocations",
        "solver_tt_hits",
        "solver_tt_key_comparisons",
        "solver_tt_probe_length_0",
        "solver_tt_probe_length_1",
        "solver_tt_probe_length_2",
        "solver_tt_probe_length_3_to_4",
        "solver_tt_probe_length_5_plus",
        "solver_tt_probes",
        "solver_tt_stores",
    }
)
_PARALLEL_SCHEDULING_COUNTERS = frozenset(
    {
        "evaluated_boards",
        "inference_requests",
        "reservations_committed",
        "root_q_digest",
        "root_visit_digest",
        "selected",
        "tree_size",
        "virtual_loss_added",
        "virtual_loss_released",
    }
)


class BenchmarkContractError(ValueError):
    """Raised when two runs are unsafe to compare."""


def counter_contract_manifest() -> dict[str, Any]:
    return {
        "schema": "csplendor.counter_contract.v1",
        "configuration_exact": sorted(_CONFIG_COUNTERS),
        "correctness_exact": sorted(_CORRECTNESS_COUNTERS),
        "measurement_keyset_exact": sorted(_MEASUREMENT_COUNTERS),
        "parallel_scheduler_threads_gt_1_measurement_overrides": sorted(
            _PARALLEL_SCHEDULING_COUNTERS
        ),
        "parallel_scheduler_threads_1_strict": True,
    }


def parse_cpu_set(value: str) -> list[int]:
    """Parse taskset notation such as ``4`` or ``4,6-8``."""
    cpus: set[int] = set()
    for component in value.split(","):
        component = component.strip()
        if not component:
            raise ValueError(f"invalid empty CPU component in {value!r}")
        if "-" in component:
            start_text, end_text = component.split("-", 1)
            start, end = int(start_text), int(end_text)
            if start < 0 or end < start:
                raise ValueError(f"invalid CPU range: {component!r}")
            cpus.update(range(start, end + 1))
        else:
            cpu = int(component)
            if cpu < 0:
                raise ValueError("CPU identifiers must be non-negative")
            cpus.add(cpu)
    if not cpus:
        raise ValueError("CPU set must not be empty")
    return sorted(cpus)


def format_cpu_set(cpus: Sequence[int]) -> str:
    return ",".join(str(cpu) for cpu in sorted({int(cpu) for cpu in cpus}))


def abba_order(pair_index: int) -> tuple[str, str]:
    """Return AB, BA, AB, BA ...; flattened, this is ABBA repeated."""
    if pair_index < 0:
        raise ValueError("pair index must be non-negative")
    return ("A", "B") if pair_index % 2 == 0 else ("B", "A")


def percentile(values: Sequence[float], probability: float) -> float:
    """Linear-interpolated percentile (inclusive endpoints)."""
    if not values:
        raise ValueError("cannot summarize an empty sample")
    if not 0.0 <= probability <= 1.0:
        raise ValueError("probability must be in [0, 1]")
    ordered = sorted(float(value) for value in values)
    position = (len(ordered) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def absolute_statistics(values: Sequence[float]) -> dict[str, Any]:
    raw = [float(value) for value in values]
    if not raw or not all(math.isfinite(value) for value in raw):
        raise ValueError("samples must be a non-empty finite sequence")
    return {
        "raw": raw,
        "median": float(statistics.median(raw)),
        "p50": percentile(raw, 0.50),
        "p95": percentile(raw, 0.95),
    }


def _counter_value_statistics(values: Sequence[Any]) -> dict[str, Any]:
    if all(
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(float(value))
        for value in values
    ):
        return absolute_statistics(values)
    raw = [deepcopy(value) for value in values]
    return {
        "raw": raw,
        "identical_in_all_samples": all(value == raw[0] for value in raw[1:]),
    }


def paired_bootstrap_ratio_ci(
    baseline: Sequence[float],
    candidate: Sequence[float],
    *,
    iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
    seed: int = 0,
) -> tuple[float, float, float]:
    """Bootstrap a 95% CI for the median paired candidate/baseline ratio."""
    if len(baseline) != len(candidate) or not baseline:
        raise ValueError("paired samples must have the same non-zero length")
    if iterations < 1:
        raise ValueError("bootstrap iterations must be positive")
    ratios = []
    for baseline_value, candidate_value in zip(baseline, candidate):
        a, b = float(baseline_value), float(candidate_value)
        if not math.isfinite(a) or not math.isfinite(b) or a <= 0.0 or b <= 0.0:
            raise ValueError("paired rate samples must be finite and positive")
        ratios.append(b / a)
    rng = random.Random(seed)
    draws = [
        float(statistics.median(ratios[rng.randrange(len(ratios))] for _ in ratios))
        for _ in range(iterations)
    ]
    return (
        percentile(draws, 0.025),
        float(statistics.median(ratios)),
        percentile(draws, 0.975),
    )


def crossover_block_bootstrap_ratio_ci(
    baseline: Sequence[float],
    candidate: Sequence[float],
    *,
    iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
    seed: int = 0,
) -> tuple[float, float, float, list[float]]:
    """Bootstrap two-pair crossover blocks after cancelling fixed-slot bias.

    Consecutive pairs must use complementary binary-slot layouts.  The
    statistical unit is the geometric mean of their two candidate/baseline
    ratios, not either placement-dependent pair by itself.
    """
    if len(baseline) != len(candidate) or not baseline:
        raise ValueError("crossover samples must have the same non-zero length")
    if len(baseline) % 2:
        raise ValueError("crossover samples require an even number of pairs")
    if iterations < 1:
        raise ValueError("bootstrap iterations must be positive")
    pair_ratios: list[float] = []
    for baseline_value, candidate_value in zip(baseline, candidate):
        left, right = float(baseline_value), float(candidate_value)
        if (
            not math.isfinite(left)
            or not math.isfinite(right)
            or left <= 0.0
            or right <= 0.0
        ):
            raise ValueError("crossover rate samples must be finite and positive")
        pair_ratios.append(right / left)
    block_ratios = [
        math.sqrt(pair_ratios[index] * pair_ratios[index + 1])
        for index in range(0, len(pair_ratios), 2)
    ]
    rng = random.Random(seed)
    draws = [
        float(
            statistics.median(
                block_ratios[rng.randrange(len(block_ratios))] for _ in block_ratios
            )
        )
        for _ in range(iterations)
    ]
    return (
        percentile(draws, 0.025),
        float(statistics.median(block_ratios)),
        percentile(draws, 0.975),
        block_ratios,
    )


def unpaired_bootstrap_ratio_ci(
    baseline: Sequence[float],
    candidate: Sequence[float],
    *,
    iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
    seed: int = 0,
) -> tuple[float, float, float]:
    """Bootstrap an unpaired 95% CI for the ratio of sample medians."""
    if not baseline or not candidate:
        raise ValueError("unpaired samples must both be non-empty")
    if iterations < 1:
        raise ValueError("bootstrap iterations must be positive")
    left = [float(value) for value in baseline]
    right = [float(value) for value in candidate]
    if not all(math.isfinite(value) and value > 0.0 for value in left + right):
        raise ValueError("unpaired rate samples must be finite and positive")
    rng = random.Random(seed)
    draws = []
    for _ in range(iterations):
        sampled_left = [left[rng.randrange(len(left))] for _ in left]
        sampled_right = [right[rng.randrange(len(right))] for _ in right]
        draws.append(statistics.median(sampled_right) / statistics.median(sampled_left))
    point = statistics.median(right) / statistics.median(left)
    return percentile(draws, 0.025), float(point), percentile(draws, 0.975)


def _json_load(path: Path | str) -> dict[str, Any]:
    document = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise BenchmarkContractError(f"{path}: top-level JSON must be an object")
    return document


def _manifest_command_arguments(manifest: Mapping[str, Any]) -> list[Any]:
    command = manifest.get("command")
    if not isinstance(command, list):
        return []
    # Baseline and candidate executable paths are intentionally different.
    return command[1:]


def _manifest_compatibility_view(manifest: Mapping[str, Any]) -> dict[str, Any]:
    try:
        host = manifest["host"]
        cpu = host["cpu"]
        uname = host["uname"]
        tools = manifest["tools"]
        topology = cpu["topology"]
    except (KeyError, TypeError) as error:
        raise BenchmarkContractError(
            f"incomplete benchmark manifest: {error}"
        ) from error
    topology_fields = (
        topology.get("fields", {}) if isinstance(topology, Mapping) else {}
    )
    stable_topology_names = (
        "Architecture",
        "CPU op-mode(s)",
        "CPU(s)",
        "On-line CPU(s) list",
        "Vendor ID",
        "Model name",
        "Thread(s) per core",
        "Core(s) per socket",
        "Socket(s)",
        "NUMA node(s)",
    )
    compiler = tools.get("compiler", {})
    if not isinstance(compiler, Mapping):
        compiler = {}
    build = manifest.get("build", {})
    if not isinstance(build, Mapping):
        build = {}
    return {
        "uname": {
            name: uname.get(name) for name in ("system", "node", "release", "machine")
        },
        "cpu": {
            "logical_count": cpu.get("logical_count"),
            "benchmark_affinity": cpu.get("benchmark_affinity"),
            "topology": {
                name: topology_fields.get(name) for name in stable_topology_names
            },
            "governors": cpu.get("governors"),
            "selected_cpu_details": cpu.get("selected_cpu_details"),
            "selected_set_contains_smt_siblings": cpu.get(
                "selected_set_contains_smt_siblings"
            ),
        },
        "tools": {
            "compiler_path": compiler.get("resolved_path", compiler.get("path")),
            "compiler_sha256": compiler.get("sha256"),
            "compiler_version": compiler.get("version"),
            "cmake_version": tools.get("cmake", {}).get("version"),
            "python_version": tools.get("python", {}).get("version"),
            "python_implementation": tools.get("python", {}).get("implementation"),
        },
        "build": {
            "available": build.get("available"),
            "benchmark_build_fingerprint_sha256": build.get(
                "benchmark_build_fingerprint_sha256"
            ),
        },
        "command_arguments": _manifest_command_arguments(manifest),
    }


def manifest_mismatches(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any]
) -> list[str]:
    mismatches: list[str] = []
    if baseline.get("schema") != MANIFEST_SCHEMA:
        mismatches.append("baseline.schema")
    if candidate.get("schema") != MANIFEST_SCHEMA:
        mismatches.append("candidate.schema")
    if mismatches:
        return mismatches
    left = _manifest_compatibility_view(baseline)
    right = _manifest_compatibility_view(candidate)
    for side, view in (("baseline", left), ("candidate", right)):
        build = view["build"]
        if build.get("available") is not True or not build.get(
            "benchmark_build_fingerprint_sha256"
        ):
            mismatches.append(f"{side}.build.benchmark_build_fingerprint_sha256")
        compiler = view["tools"]
        if not compiler.get("compiler_sha256"):
            mismatches.append(f"{side}.tools.compiler_sha256")
    if mismatches:
        return mismatches

    def compare(prefix: str, first: Any, second: Any) -> None:
        if isinstance(first, Mapping) and isinstance(second, Mapping):
            for key in sorted(set(first) | set(second)):
                compare(
                    f"{prefix}.{key}" if prefix else str(key),
                    first.get(key),
                    second.get(key),
                )
        elif first != second:
            mismatches.append(prefix)

    compare("", left, right)
    return mismatches


def validate_manifest_compatibility(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any]
) -> None:
    mismatches = manifest_mismatches(baseline, candidate)
    if mismatches:
        raise BenchmarkContractError(
            "benchmark manifests are incompatible: " + ", ".join(mismatches)
        )


def validate_manifest_matches_command(
    manifest: Mapping[str, Any], command: Sequence[str]
) -> None:
    """Reject a stale externally supplied manifest before running a binary."""
    normalized_command = [str(part) for part in command]
    if manifest.get("command") != normalized_command:
        raise BenchmarkContractError(
            "supplied benchmark manifest command does not match the executed command"
        )
    if not normalized_command:
        raise BenchmarkContractError("supplied benchmark command is empty")
    argument = normalized_command[0]
    found = shutil.which(argument)
    binary = (Path(found) if found else Path(argument)).resolve()
    if not binary.is_file():
        raise BenchmarkContractError(f"benchmark binary not found: {argument}")
    binary_metadata = manifest.get("binary")
    if not isinstance(binary_metadata, Mapping):
        raise BenchmarkContractError(
            "supplied benchmark manifest has no binary metadata"
        )
    expected_path = binary_metadata.get("path")
    expected_sha256 = binary_metadata.get("sha256")
    actual_sha256 = sha256_file(binary)
    if expected_path != str(binary) or expected_sha256 != actual_sha256:
        raise BenchmarkContractError(
            "supplied benchmark manifest binary path/SHA-256 does not match the executed binary"
        )


def _native_binary_identity(
    manifest: Mapping[str, Any], command: Sequence[str]
) -> tuple[Path, str, bytes]:
    """Resolve and validate an original ELF executable before slot staging."""
    validate_manifest_matches_command(manifest, command)
    binary_metadata = manifest.get("binary")
    if not isinstance(binary_metadata, Mapping):
        raise BenchmarkContractError("benchmark manifest has no binary metadata")
    path_value = binary_metadata.get("path")
    digest = binary_metadata.get("sha256")
    if not isinstance(path_value, str) or not isinstance(digest, str):
        raise BenchmarkContractError("benchmark manifest binary identity is incomplete")
    binary = Path(path_value)
    try:
        metadata = binary.lstat()
    except OSError as error:
        raise BenchmarkContractError(
            f"cannot stat benchmark binary: {binary}"
        ) from error
    if not stat.S_ISREG(metadata.st_mode):
        raise BenchmarkContractError(
            f"binary-slot rotation requires a regular executable: {binary}"
        )
    if not metadata.st_mode & 0o111:
        raise BenchmarkContractError(
            f"binary-slot rotation requires an executable file: {binary}"
        )
    if metadata.st_mode & (stat.S_ISUID | stat.S_ISGID):
        raise BenchmarkContractError(
            "binary-slot rotation rejects setuid/setgid executables"
        )
    if hasattr(os, "getxattr"):
        try:
            capabilities = os.getxattr(
                binary, "security.capability", follow_symlinks=False
            )
        except OSError as error:
            unsupported = {
                errno.ENODATA,
                getattr(errno, "ENOATTR", errno.ENODATA),
                errno.ENOTSUP,
                getattr(errno, "EOPNOTSUPP", errno.ENOTSUP),
            }
            if error.errno not in unsupported:
                raise BenchmarkContractError(
                    "cannot verify benchmark executable capabilities"
                ) from error
        else:
            if capabilities:
                raise BenchmarkContractError(
                    "binary-slot rotation rejects file-capability executables"
                )
    open_flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    open_flags |= getattr(os, "O_NOFOLLOW", 0)
    try:
        descriptor = os.open(binary, open_flags)
    except OSError as error:
        raise BenchmarkContractError(
            f"cannot safely open benchmark binary: {binary}"
        ) from error
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise BenchmarkContractError(
                f"binary-slot rotation requires a regular executable: {binary}"
            )
        chunks = []
        while True:
            chunk = os.read(descriptor, 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
    finally:
        os.close(descriptor)
    snapshot = b"".join(chunks)
    magic = snapshot[:4]
    if magic != b"\x7fELF":
        raise BenchmarkContractError(
            f"binary-slot rotation requires a native ELF executable: {binary}"
        )
    if b"$ORIGIN" in snapshot or b"${ORIGIN}" in snapshot:
        raise BenchmarkContractError(
            "binary-slot rotation does not support ELF binaries with "
            "$ORIGIN-dependent RPATH/RUNPATH"
        )
    snapshot_sha256 = hashlib.sha256(snapshot).hexdigest()
    if snapshot_sha256 != digest:
        raise BenchmarkContractError(
            "benchmark binary changed while creating its manifest snapshot"
        )
    return binary, digest, snapshot


class _FixedBinarySlotRotator:
    """Stage A/B into two private fixed-inode executable slots."""

    _SLOT_IDS = ("slot0", "slot1")

    def __init__(
        self,
        commands: Mapping[str, Sequence[str]],
        manifests: Mapping[str, Mapping[str, Any]],
    ) -> None:
        self._commands = {side: list(commands[side]) for side in ("A", "B")}
        self._sources = {
            side: _native_binary_identity(manifests[side], self._commands[side])
            for side in ("A", "B")
        }
        self._temporary: tempfile.TemporaryDirectory[str] | None = None
        self._slot_paths: dict[str, Path] = {}
        self._slot_identities: dict[str, tuple[int, int]] = {}

    def __enter__(self) -> _FixedBinarySlotRotator:
        self._temporary = tempfile.TemporaryDirectory(
            prefix="csplendor-benchmark-slots-"
        )
        try:
            root = Path(self._temporary.name)
            root.chmod(0o700)
            create_flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
            create_flags |= getattr(os, "O_CLOEXEC", 0)
            create_flags |= getattr(os, "O_NOFOLLOW", 0)
            for slot_id in self._SLOT_IDS:
                path = root / f"{slot_id}.elf"
                descriptor = os.open(path, create_flags, 0o700)
                try:
                    os.fchmod(descriptor, 0o700)
                    metadata = os.fstat(descriptor)
                    if not stat.S_ISREG(metadata.st_mode):
                        raise BenchmarkContractError(
                            f"binary slot is not a regular file: {slot_id}"
                        )
                    self._slot_paths[slot_id] = path
                    self._slot_identities[slot_id] = (
                        metadata.st_dev,
                        metadata.st_ino,
                    )
                finally:
                    os.close(descriptor)
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def __exit__(self, _type: Any, _value: Any, _traceback: Any) -> None:
        try:
            if self._temporary is not None:
                self._temporary.cleanup()
        finally:
            self._temporary = None
            self._slot_paths.clear()
            self._slot_identities.clear()

    def _assert_live_slot(self, slot_id: str) -> os.stat_result:
        try:
            metadata = self._slot_paths[slot_id].lstat()
        except (KeyError, OSError) as error:
            raise BenchmarkContractError(
                f"binary slot disappeared: {slot_id}"
            ) from error
        if not stat.S_ISREG(metadata.st_mode):
            raise BenchmarkContractError(f"binary slot is not regular: {slot_id}")
        if (metadata.st_dev, metadata.st_ino) != self._slot_identities[slot_id]:
            raise BenchmarkContractError(f"binary slot inode changed: {slot_id}")
        return metadata

    @staticmethod
    def _write_all(descriptor: int, chunk: bytes) -> None:
        remaining = memoryview(chunk)
        while remaining:
            written = os.write(descriptor, remaining)
            if written <= 0:
                raise OSError("short write while staging benchmark binary")
            remaining = remaining[written:]

    def _copy_to_slot(self, side: str, slot_id: str) -> dict[str, Any]:
        _source, expected_sha256, snapshot = self._sources[side]
        self._assert_live_slot(slot_id)
        target_flags = os.O_WRONLY | os.O_TRUNC
        target_flags |= getattr(os, "O_CLOEXEC", 0)
        target_flags |= getattr(os, "O_NOFOLLOW", 0)
        try:
            target_fd = os.open(self._slot_paths[slot_id], target_flags)
        except OSError as error:
            raise BenchmarkContractError(
                f"cannot stage benchmark binary into {slot_id}"
            ) from error
        digest = hashlib.sha256()
        try:
            target_metadata = os.fstat(target_fd)
            if (
                target_metadata.st_dev,
                target_metadata.st_ino,
            ) != self._slot_identities[slot_id]:
                raise BenchmarkContractError(f"binary slot inode changed: {slot_id}")
            digest.update(snapshot)
            self._write_all(target_fd, snapshot)
            os.fchmod(target_fd, 0o700)
            os.fsync(target_fd)
        finally:
            os.close(target_fd)
        staged_sha256 = digest.hexdigest()
        if staged_sha256 != expected_sha256:
            raise BenchmarkContractError(
                f"staged {side} SHA-256 differs from its original manifest"
            )
        verified_sha256 = self.verify(slot_id, expected_sha256)
        metadata = self._assert_live_slot(slot_id)
        return {
            "slot_id": slot_id,
            "slot_path": str(self._slot_paths[slot_id]),
            "slot_device": metadata.st_dev,
            "slot_inode": metadata.st_ino,
            "source_binary_path": str(_source),
            "source_binary_sha256": expected_sha256,
            "source_binary_size_bytes": len(snapshot),
            "staged_binary_sha256": verified_sha256,
        }

    def verify(self, slot_id: str, expected_sha256: str) -> str:
        self._assert_live_slot(slot_id)
        actual_sha256 = sha256_file(self._slot_paths[slot_id])
        self._assert_live_slot(slot_id)
        if actual_sha256 != expected_sha256:
            raise BenchmarkContractError(
                f"binary slot content changed before/after execution: {slot_id}"
            )
        return actual_sha256

    def prepare(self, pair_index: int) -> dict[str, dict[str, Any]]:
        order = abba_order(pair_index)
        side_by_slot = {"slot0": order[0], "slot1": order[1]}
        layout: dict[str, dict[str, Any]] = {}
        for slot_id in self._SLOT_IDS:
            side = side_by_slot[slot_id]
            layout[side] = self._copy_to_slot(side, slot_id)
        layout_id = f"A-{layout['A']['slot_id']}_B-{layout['B']['slot_id']}"
        for side in ("A", "B"):
            layout[side]["layout_id"] = layout_id
            layout[side]["layout_index"] = pair_index % 2
        return layout

    def execution_command(
        self, side: str, slot_metadata: Mapping[str, Any]
    ) -> tuple[list[str], str]:
        slot_id = str(slot_metadata["slot_id"])
        expected_sha256 = str(slot_metadata["staged_binary_sha256"])
        verified_sha256 = self.verify(slot_id, expected_sha256)
        return [
            str(self._slot_paths[slot_id]),
            *self._commands[side][1:],
        ], verified_sha256


def _finite_number(value: Any, field: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise BenchmarkContractError(f"{field} must be numeric")
    converted = float(value)
    if not math.isfinite(converted) or (positive and converted <= 0.0):
        qualifier = "positive " if positive else "finite "
        raise BenchmarkContractError(f"{field} must be {qualifier}numeric")
    return converted


def _integer(value: Any, field: str, *, positive: bool = False) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise BenchmarkContractError(f"{field} must be an integer")
    if (positive and value <= 0) or (not positive and value < 0):
        qualifier = "positive" if positive else "non-negative"
        raise BenchmarkContractError(f"{field} must be {qualifier}")
    return value


def counter_classification(
    name: str,
    *,
    workload: str | None = None,
    semantics: Mapping[str, Any] | None = None,
) -> str:
    # A multi-worker scheduler is intentionally race-scheduled.  Its completed
    # simulation/error invariants stay strict, while search shape and ownership
    # counts are measurements.  The 1-worker deterministic gate remains strict.
    if (
        workload == "parallel_scheduler"
        and isinstance(semantics, Mapping)
        and semantics.get("threads", 1) > 1
        and name in _PARALLEL_SCHEDULING_COUNTERS
    ):
        return "measurement"
    if name in _CONFIG_COUNTERS:
        return "configuration"
    if name in _CORRECTNESS_COUNTERS:
        return "correctness"
    if name in _MEASUREMENT_COUNTERS:
        return "measurement"
    raise BenchmarkContractError(f"unclassified native counter: {name!r}")


def _validate_counter_object(counters: Mapping[str, Any]) -> None:
    for name, value in counters.items():
        if not isinstance(name, str) or not name:
            raise BenchmarkContractError("counter names must be non-empty strings")
        counter_classification(name)
        if isinstance(value, (dict, list)) or value is None:
            raise BenchmarkContractError(f"counter {name!r} must be a JSON scalar")


def normalize_record(record: Mapping[str, Any]) -> dict[str, Any]:
    """Validate a native row while preserving all unknown fields."""
    normalized = deepcopy(dict(record))
    if normalized.get("schema") != ENGINE_SCHEMA:
        raise BenchmarkContractError(
            f"unsupported native schema: {normalized.get('schema')!r}"
        )
    for field in ("workload", "fixture", "digest"):
        if not isinstance(normalized.get(field), str) or not normalized[field]:
            raise BenchmarkContractError(f"native row requires non-empty {field!r}")
    _integer(normalized.get("seed"), "seed")
    operations = _integer(normalized.get("operations"), "operations", positive=True)
    elapsed_ns = _integer(normalized.get("elapsed_ns"), "elapsed_ns", positive=True)
    rate = _finite_number(
        normalized.get("rate_per_second"), "rate_per_second", positive=True
    )
    expected_rate = float(operations) * 1.0e9 / float(elapsed_ns)
    if not math.isclose(rate, expected_rate, rel_tol=1.0e-9, abs_tol=1.0e-9):
        raise BenchmarkContractError(
            "rate_per_second is inconsistent with operations and elapsed_ns"
        )
    if "rss_kib" not in normalized:
        raise BenchmarkContractError("native row requires 'rss_kib'")
    if normalized["rss_kib"] is not None:
        _integer(normalized["rss_kib"], "rss_kib")
    if not isinstance(normalized.get("counters"), dict):
        raise BenchmarkContractError("counters must be an object")
    if not isinstance(normalized.get("semantics"), dict):
        raise BenchmarkContractError("semantics must be an object")
    if normalized["semantics"].get("correct") is not True:
        raise BenchmarkContractError("semantics.correct must be true")
    _validate_counter_object(normalized["counters"])
    return normalized


def parse_native_json_lines(output: str) -> list[dict[str, Any]]:
    records: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    for line_number, line in enumerate(output.splitlines(), 1):
        if not line.strip():
            continue
        try:
            parsed = json.loads(line)
        except json.JSONDecodeError as error:
            raise BenchmarkContractError(
                f"native output line {line_number} is not JSON: {error.msg}"
            ) from error
        if not isinstance(parsed, dict):
            raise BenchmarkContractError(
                f"native output line {line_number} must be an object"
            )
        record = normalize_record(parsed)
        key = record_key(record)
        if key in seen:
            raise BenchmarkContractError(f"duplicate native benchmark row: {key!r}")
        seen.add(key)
        records.append(record)
    if not records:
        raise BenchmarkContractError("native benchmark produced no JSON rows")
    return records


def record_key(record: Mapping[str, Any]) -> tuple[str, str]:
    return str(record["workload"]), str(record["fixture"])


def _record_metadata(record: Mapping[str, Any]) -> dict[str, Any]:
    return {
        key: value for key, value in record.items() if key not in _DYNAMIC_RECORD_FIELDS
    }


def _validate_counter_pair(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any]
) -> None:
    left = baseline["counters"]
    right = candidate["counters"]
    if set(left) != set(right):
        raise BenchmarkContractError(
            "native metadata mismatch: counter key set changed "
            f"(missing_candidate={sorted(set(left) - set(right))}, "
            f"missing_baseline={sorted(set(right) - set(left))})"
        )
    for name in sorted(left):
        classification = counter_classification(
            name,
            workload=str(baseline["workload"]),
            semantics=baseline["semantics"],
        )
        if (
            classification in {"configuration", "correctness"}
            and left[name] != right[name]
        ):
            raise BenchmarkContractError(
                "native metadata mismatch: "
                f"{classification} counter {name!r} changed "
                f"({left[name]!r} != {right[name]!r})"
            )


def validate_record_pair(
    baseline: Mapping[str, Any], candidate: Mapping[str, Any]
) -> None:
    left = normalize_record(baseline)
    right = normalize_record(candidate)
    if _record_metadata(left) != _record_metadata(right):
        raise BenchmarkContractError(
            f"native metadata mismatch for {record_key(left)!r}"
        )
    _validate_counter_pair(left, right)


def _records_by_key(
    records: Iterable[Mapping[str, Any]],
) -> dict[tuple[str, str], Mapping[str, Any]]:
    result: dict[tuple[str, str], Mapping[str, Any]] = {}
    for record in records:
        key = record_key(record)
        if key in result:
            raise BenchmarkContractError(f"duplicate benchmark row: {key!r}")
        result[key] = record
    return result


def _validate_record_sets(
    baseline: Sequence[Mapping[str, Any]], candidate: Sequence[Mapping[str, Any]]
) -> tuple[
    dict[tuple[str, str], Mapping[str, Any]], dict[tuple[str, str], Mapping[str, Any]]
]:
    left, right = _records_by_key(baseline), _records_by_key(candidate)
    if set(left) != set(right):
        missing_candidate = sorted(set(left) - set(right))
        missing_baseline = sorted(set(right) - set(left))
        raise BenchmarkContractError(
            "native workload set mismatch: "
            f"missing_candidate={missing_candidate}, missing_baseline={missing_baseline}"
        )
    for key in sorted(left):
        validate_record_pair(left[key], right[key])
    return left, right


def _process_group_exists(process_group: int) -> bool:
    try:
        os.killpg(process_group, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def _terminate_process_group(
    process: subprocess.Popen[str], *, grace_seconds: float = 0.2
) -> None:
    """Terminate the complete benchmark wrapper/process group and reap it."""
    if hasattr(os, "killpg"):
        process_group = process.pid
        try:
            os.killpg(process_group, signal.SIGTERM)
        except ProcessLookupError:
            pass
        deadline = time.monotonic() + grace_seconds
        while time.monotonic() < deadline and _process_group_exists(process_group):
            time.sleep(0.01)
        if _process_group_exists(process_group):
            try:
                os.killpg(process_group, signal.SIGKILL)
            except ProcessLookupError:
                pass
    elif process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=grace_seconds)
        except subprocess.TimeoutExpired:
            process.kill()
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def _run_with_rss(
    command: Sequence[str], cpu_set: Sequence[int], timeout: float | None
) -> tuple[str, int | None, list[str]]:
    if not command:
        raise ValueError("benchmark command must not be empty")
    cpus = sorted({int(cpu) for cpu in cpu_set})
    if not cpus:
        raise ValueError("CPU set must not be empty")
    if timeout is not None and (not math.isfinite(timeout) or timeout <= 0.0):
        raise ValueError("timeout must be finite and positive")
    if hasattr(os, "sched_getaffinity"):
        allowed = set(os.sched_getaffinity(0))
        unavailable = sorted(set(cpus) - allowed)
        if unavailable:
            raise ValueError(f"CPU set is outside the current affinity: {unavailable}")

    benchmark_command = [str(part) for part in command]
    taskset = shutil.which("taskset")
    execution_command = (
        [taskset, "-c", format_cpu_set(cpus), *benchmark_command]
        if taskset
        else benchmark_command
    )
    gnu_time = Path("/usr/bin/time")
    rss_path: Path | None = None
    preexec_fn = None
    if taskset is None and hasattr(os, "sched_setaffinity"):

        def set_affinity() -> None:
            os.sched_setaffinity(0, cpus)

        preexec_fn = set_affinity
    try:
        with tempfile.NamedTemporaryFile(
            prefix="csplendor-rss-", delete=False
        ) as stream:
            rss_path = Path(stream.name)
        if gnu_time.is_file():
            executed = [
                str(gnu_time),
                "-f",
                "CSPLENDOR_MAX_RSS_KIB=%M",
                "-o",
                str(rss_path),
                "--",
                *execution_command,
            ]
        else:
            executed = execution_command
        process = subprocess.Popen(
            executed,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            preexec_fn=preexec_fn,
            start_new_session=hasattr(os, "setsid"),
        )
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            _terminate_process_group(process)
            try:
                process.communicate(timeout=1.0)
            except (subprocess.TimeoutExpired, ValueError):
                pass
            raise
        if hasattr(os, "killpg") and _process_group_exists(process.pid):
            _terminate_process_group(process, grace_seconds=0.0)
            raise RuntimeError("benchmark command left a running descendant process")
        if process.returncode != 0:
            diagnostic = stderr[-4000:].strip()
            raise RuntimeError(
                f"benchmark command exited with {process.returncode}: {diagnostic}"
            )
        rss_kib = None
        if gnu_time.is_file() and rss_path is not None:
            for line in rss_path.read_text(encoding="ascii").splitlines():
                prefix = "CSPLENDOR_MAX_RSS_KIB="
                if line.startswith(prefix):
                    rss_kib = int(line[len(prefix) :])
        return stdout, rss_kib, execution_command
    finally:
        if rss_path is not None:
            try:
                rss_path.unlink()
            except FileNotFoundError:
                pass


def run_native_command(
    command: Sequence[str],
    *,
    cpu_set: Sequence[int],
    timeout: float | None = None,
) -> dict[str, Any]:
    output, rss_kib, execution_command = _run_with_rss(command, cpu_set, timeout)
    records = parse_native_json_lines(output)
    # GNU time reports the peak for the complete process.  It is attributable
    # to a workload only when the process emitted exactly one workload row.
    if rss_kib is not None and len(records) == 1:
        for record in records:
            record["runner_rss_kib"] = rss_kib
    return {
        "execution_command": execution_command,
        "runner_rss_kib": rss_kib,
        "runner_rss_scope": "single_workload" if len(records) == 1 else "process",
        "records": records,
    }


def _default_cpu_set() -> list[int]:
    if hasattr(os, "sched_getaffinity"):
        available = sorted(os.sched_getaffinity(0))
    else:
        available = list(range(os.cpu_count() or 1))
    if not available:
        raise RuntimeError("no CPUs are available")
    return [available[0]]


def _summary_for_runs(runs: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    if not runs:
        return []
    maps = [_records_by_key(run["records"]) for run in runs]
    keys = set(maps[0])
    if any(set(mapping) != keys for mapping in maps[1:]):
        raise BenchmarkContractError("workload set changed between repeated runs")
    summary = []
    for key in sorted(keys):
        first = maps[0][key]
        for mapping in maps[1:]:
            validate_record_pair(first, mapping[key])
        entry: dict[str, Any] = {
            "workload": key[0],
            "fixture": key[1],
            "digest": first["digest"],
            "rate_per_second": absolute_statistics(
                [mapping[key]["rate_per_second"] for mapping in maps]
            ),
            "elapsed_ns": absolute_statistics(
                [mapping[key]["elapsed_ns"] for mapping in maps]
            ),
        }
        runner_rss = [
            mapping[key].get("runner_rss_kib")
            for mapping in maps
            if mapping[key].get("runner_rss_kib") is not None
        ]
        native_rss = [
            mapping[key].get("rss_kib")
            for mapping in maps
            if mapping[key].get("rss_kib") is not None
        ]
        if runner_rss:
            entry["runner_rss_kib"] = absolute_statistics(runner_rss)
        if native_rss:
            entry["native_rss_kib"] = absolute_statistics(native_rss)
        counter_names = set(maps[0][key]["counters"])
        counter_summary = {}
        for name in sorted(counter_names):
            values = [mapping[key]["counters"][name] for mapping in maps]
            counter_summary[name] = {
                "classification": counter_classification(
                    name,
                    workload=str(first["workload"]),
                    semantics=first["semantics"],
                ),
                **_counter_value_statistics(values),
            }
        if counter_summary:
            entry["counters"] = counter_summary
        summary.append(entry)
    return summary


def collect_runs(
    command: Sequence[str],
    *,
    runs: int = DEFAULT_PAIRS,
    warmups: int = DEFAULT_WARMUPS,
    cpu_set: Sequence[int] | None = None,
    timeout: float | None = None,
    label: str = "local",
    repo_root: Path | str = ROOT,
    cmake_cache: Path | str | None = None,
) -> dict[str, Any]:
    if runs < 1 or warmups < 0:
        raise ValueError("runs must be positive and warmups non-negative")
    selected_cpus = list(cpu_set or _default_cpu_set())
    manifest = collect_manifest(
        command,
        repo_root=repo_root,
        benchmark_affinity=selected_cpus,
        cmake_cache=cmake_cache,
    )
    for _ in range(warmups):
        run_native_command(command, cpu_set=selected_cpus, timeout=timeout)
    samples = []
    for run_index in range(runs):
        sample = run_native_command(command, cpu_set=selected_cpus, timeout=timeout)
        sample["run_index"] = run_index
        samples.append(sample)
    return {
        "schema": RESULT_SCHEMA,
        "mode": "collect",
        "label": label,
        "manifest": manifest,
        "counter_contract": counter_contract_manifest(),
        "settings": {
            "warmups": warmups,
            "runs": runs,
            "cpu_set": sorted(selected_cpus),
            "timeout_seconds": timeout,
            "rss_scope": "process; per-workload only for single-row commands",
        },
        "samples": samples,
        "summary": _summary_for_runs(samples),
    }


def _validate_crossover_pair_layouts(pairs: Sequence[Mapping[str, Any]]) -> None:
    if len(pairs) % 2:
        raise BenchmarkContractError(
            "binary-slot crossover requires an even number of pairs"
        )
    for expected_index, pair in enumerate(pairs):
        if pair.get("pair_index") != expected_index:
            raise BenchmarkContractError(
                "binary-slot crossover pair indexes must be contiguous"
            )
        expected_order = list(abba_order(expected_index))
        if pair.get("order") != expected_order:
            raise BenchmarkContractError(
                f"binary-slot crossover order mismatch at pair {expected_index}"
            )
        expected_layout = {
            expected_order[0]: "slot0",
            expected_order[1]: "slot1",
        }
        layout = pair.get("binary_slot_layout")
        if (
            not isinstance(layout, Mapping)
            or {side: layout.get(side) for side in ("A", "B")} != expected_layout
        ):
            raise BenchmarkContractError(
                f"binary-slot crossover layout mismatch at pair {expected_index}"
            )
        for side in ("A", "B"):
            sample = pair.get(side)
            slot_metadata = (
                sample.get("binary_slot") if isinstance(sample, Mapping) else None
            )
            if (
                not isinstance(slot_metadata, Mapping)
                or slot_metadata.get("slot_id") != expected_layout[side]
                or slot_metadata.get("layout_index") != expected_index % 2
                or slot_metadata.get("source_binary_sha256")
                != slot_metadata.get("staged_binary_sha256")
                or slot_metadata.get("post_run_sha256")
                != slot_metadata.get("staged_binary_sha256")
            ):
                raise BenchmarkContractError(
                    f"binary-slot sample metadata mismatch at pair {expected_index} side {side}"
                )


def _comparison_from_paired_samples(
    pairs: Sequence[Mapping[str, Any]],
    *,
    bootstrap_iterations: int,
    rotate_binary_slots: bool = False,
) -> list[dict[str, Any]]:
    if not pairs:
        raise BenchmarkContractError("paired comparison requires at least one pair")
    if rotate_binary_slots:
        _validate_crossover_pair_layouts(pairs)
    pair_maps = []
    for pair in pairs:
        left, right = _validate_record_sets(pair["A"]["records"], pair["B"]["records"])
        pair_maps.append((left, right))
    keys = set(pair_maps[0][0])
    if any(set(left) != keys or set(right) != keys for left, right in pair_maps[1:]):
        raise BenchmarkContractError("workload set changed between pairs")
    if rotate_binary_slots:
        for key in sorted(keys):
            reference = pair_maps[0][0][key]
            for baseline, candidate in pair_maps:
                validate_record_pair(reference, baseline[key])
                validate_record_pair(reference, candidate[key])

    metrics: list[dict[str, Any]] = []
    for metric_index, key in enumerate(sorted(keys)):
        baseline_rates = [mapping[0][key]["rate_per_second"] for mapping in pair_maps]
        candidate_rates = [mapping[1][key]["rate_per_second"] for mapping in pair_maps]
        ratio_raw = [
            float(candidate) / float(baseline)
            for baseline, candidate in zip(baseline_rates, candidate_rates)
        ]
        if rotate_binary_slots:
            low, median_ratio, high, block_ratios = crossover_block_bootstrap_ratio_ci(
                baseline_rates,
                candidate_rates,
                iterations=bootstrap_iterations,
                seed=metric_index,
            )
            ratio_summary: dict[str, Any] = {
                "method": "two_pair_binary_slot_crossover",
                "raw": block_ratios,
                "pair_raw": ratio_raw,
                "median": median_ratio,
                "crossover_block_bootstrap_ci95": [low, high],
                "bootstrap_unit": "two_pair_crossover_block",
                "crossover_blocks": len(block_ratios),
                "bootstrap_iterations": bootstrap_iterations,
            }
        else:
            low, median_ratio, high = paired_bootstrap_ratio_ci(
                baseline_rates,
                candidate_rates,
                iterations=bootstrap_iterations,
                seed=metric_index,
            )
            ratio_summary = {
                "raw": ratio_raw,
                "median": median_ratio,
                "paired_bootstrap_ci95": [low, high],
                "bootstrap_iterations": bootstrap_iterations,
            }
        first = pair_maps[0][0][key]
        metric: dict[str, Any] = {
            "workload": key[0],
            "fixture": key[1],
            "digest": first["digest"],
            "absolute": {
                "A_rate_per_second": absolute_statistics(baseline_rates),
                "B_rate_per_second": absolute_statistics(candidate_rates),
                "A_elapsed_ns": absolute_statistics(
                    [mapping[0][key]["elapsed_ns"] for mapping in pair_maps]
                ),
                "B_elapsed_ns": absolute_statistics(
                    [mapping[1][key]["elapsed_ns"] for mapping in pair_maps]
                ),
            },
            "B_over_A": ratio_summary,
        }
        for rss_field in ("runner_rss_kib", "rss_kib"):
            baseline_rss = [mapping[0][key].get(rss_field) for mapping in pair_maps]
            candidate_rss = [mapping[1][key].get(rss_field) for mapping in pair_maps]
            if all(value is not None for value in baseline_rss + candidate_rss):
                label = (
                    "runner_rss_kib"
                    if rss_field == "runner_rss_kib"
                    else "native_rss_kib"
                )
                metric["absolute"][f"A_{label}"] = absolute_statistics(baseline_rss)
                metric["absolute"][f"B_{label}"] = absolute_statistics(candidate_rss)
        counter_names = set(pair_maps[0][0][key]["counters"])
        counter_comparison = {}
        for name in sorted(counter_names):
            baseline_values = [
                mapping[0][key]["counters"][name] for mapping in pair_maps
            ]
            candidate_values = [
                mapping[1][key]["counters"][name] for mapping in pair_maps
            ]
            counter_comparison[name] = {
                "classification": counter_classification(
                    name,
                    workload=str(first["workload"]),
                    semantics=first["semantics"],
                ),
                "A": _counter_value_statistics(baseline_values),
                "B": _counter_value_statistics(candidate_values),
                "identical_in_every_pair": baseline_values == candidate_values,
            }
        if counter_comparison:
            metric["counters"] = counter_comparison
        metrics.append(metric)
    return metrics


def run_paired(
    baseline_command: Sequence[str],
    candidate_command: Sequence[str],
    *,
    pairs: int = DEFAULT_PAIRS,
    warmups: int = DEFAULT_WARMUPS,
    cpu_set: Sequence[int] | None = None,
    timeout: float | None = None,
    bootstrap_iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
    baseline_repo_root: Path | str = ROOT,
    candidate_repo_root: Path | str = ROOT,
    baseline_manifest: Mapping[str, Any] | None = None,
    candidate_manifest: Mapping[str, Any] | None = None,
    baseline_cmake_cache: Path | str | None = None,
    candidate_cmake_cache: Path | str | None = None,
    rotate_binary_slots: bool = False,
) -> dict[str, Any]:
    if pairs < 1 or warmups < 0:
        raise ValueError("pairs must be positive and warmups non-negative")
    if rotate_binary_slots and pairs % 2:
        raise ValueError("binary-slot rotation requires an even number of pairs")
    selected_cpus = list(cpu_set or _default_cpu_set())
    if baseline_manifest is not None:
        manifest_a = dict(baseline_manifest)
        validate_manifest_matches_command(manifest_a, baseline_command)
    else:
        manifest_a = collect_manifest(
            baseline_command,
            repo_root=baseline_repo_root,
            benchmark_affinity=selected_cpus,
            cmake_cache=baseline_cmake_cache,
        )
    if candidate_manifest is not None:
        manifest_b = dict(candidate_manifest)
        validate_manifest_matches_command(manifest_b, candidate_command)
    else:
        manifest_b = collect_manifest(
            candidate_command,
            repo_root=candidate_repo_root,
            benchmark_affinity=selected_cpus,
            cmake_cache=candidate_cmake_cache,
        )
    validate_manifest_compatibility(manifest_a, manifest_b)

    commands = {"A": list(baseline_command), "B": list(candidate_command)}
    paired_samples = []
    if rotate_binary_slots:
        with _FixedBinarySlotRotator(
            commands, {"A": manifest_a, "B": manifest_b}
        ) as slots:
            for warmup_index in range(warmups):
                order = abba_order(warmup_index)
                layout = slots.prepare(warmup_index)
                for side in order:
                    staged_command, _ = slots.execution_command(side, layout[side])
                    run_native_command(
                        staged_command, cpu_set=selected_cpus, timeout=timeout
                    )
                    slots.verify(
                        str(layout[side]["slot_id"]),
                        str(layout[side]["staged_binary_sha256"]),
                    )
            for pair_index in range(pairs):
                order = abba_order(pair_index)
                layout = slots.prepare(pair_index)
                samples: dict[str, Any] = {}
                for side in order:
                    staged_command, pre_run_sha256 = slots.execution_command(
                        side, layout[side]
                    )
                    samples[side] = run_native_command(
                        staged_command, cpu_set=selected_cpus, timeout=timeout
                    )
                    post_run_sha256 = slots.verify(
                        str(layout[side]["slot_id"]), pre_run_sha256
                    )
                    samples[side]["binary_slot"] = {
                        **deepcopy(layout[side]),
                        "logical_side": side,
                        "logical_command": list(commands[side]),
                        "staged_binary_sha256": pre_run_sha256,
                        "post_run_sha256": post_run_sha256,
                    }
                _validate_record_sets(samples["A"]["records"], samples["B"]["records"])
                paired_samples.append(
                    {
                        "pair_index": pair_index,
                        "order": list(order),
                        "binary_slot_layout": {
                            "A": layout["A"]["slot_id"],
                            "B": layout["B"]["slot_id"],
                            "layout_id": layout["A"]["layout_id"],
                        },
                        "A": samples["A"],
                        "B": samples["B"],
                    }
                )
    else:
        for warmup_index in range(warmups):
            for side in abba_order(warmup_index):
                run_native_command(
                    commands[side], cpu_set=selected_cpus, timeout=timeout
                )
        for pair_index in range(pairs):
            order = abba_order(pair_index)
            samples = {}
            for side in order:
                samples[side] = run_native_command(
                    commands[side], cpu_set=selected_cpus, timeout=timeout
                )
            _validate_record_sets(samples["A"]["records"], samples["B"]["records"])
            paired_samples.append(
                {
                    "pair_index": pair_index,
                    "order": list(order),
                    "A": samples["A"],
                    "B": samples["B"],
                }
            )

    return {
        "schema": RESULT_SCHEMA,
        "mode": "paired",
        "manifests": {"A": manifest_a, "B": manifest_b},
        "counter_contract": counter_contract_manifest(),
        "settings": {
            "warmups_per_side": warmups,
            "pairs": pairs,
            "order": "ABBA",
            "cpu_set": sorted(selected_cpus),
            "timeout_seconds": timeout,
            "bootstrap_iterations": bootstrap_iterations,
            "rss_scope": "process; per-workload only for single-row commands",
            "rotate_binary_slots": rotate_binary_slots,
            "statistical_unit": (
                "two_pair_crossover_block" if rotate_binary_slots else "pair"
            ),
            "binary_slot_policy": (
                "two_private_fixed_inodes_crossed_every_pair"
                if rotate_binary_slots
                else "original_binary_paths"
            ),
        },
        "pairs": paired_samples,
        "comparison": _comparison_from_paired_samples(
            paired_samples,
            bootstrap_iterations=bootstrap_iterations,
            rotate_binary_slots=rotate_binary_slots,
        ),
    }


def _comparison_from_unpaired_samples(
    baseline_runs: Sequence[Mapping[str, Any]],
    candidate_runs: Sequence[Mapping[str, Any]],
    *,
    bootstrap_iterations: int,
) -> list[dict[str, Any]]:
    if not baseline_runs or not candidate_runs:
        raise BenchmarkContractError("unpaired comparison requires non-empty samples")
    left_maps = [_records_by_key(run["records"]) for run in baseline_runs]
    right_maps = [_records_by_key(run["records"]) for run in candidate_runs]
    keys = set(left_maps[0])
    if any(set(mapping) != keys for mapping in left_maps[1:] + right_maps):
        raise BenchmarkContractError("workload set changed between collections")

    for key in sorted(keys):
        for mapping in left_maps[1:]:
            validate_record_pair(left_maps[0][key], mapping[key])
        for mapping in right_maps:
            validate_record_pair(left_maps[0][key], mapping[key])

    metrics: list[dict[str, Any]] = []
    for metric_index, key in enumerate(sorted(keys)):
        baseline_rates = [mapping[key]["rate_per_second"] for mapping in left_maps]
        candidate_rates = [mapping[key]["rate_per_second"] for mapping in right_maps]
        low, ratio, high = unpaired_bootstrap_ratio_ci(
            baseline_rates,
            candidate_rates,
            iterations=bootstrap_iterations,
            seed=metric_index,
        )
        first = left_maps[0][key]
        metric: dict[str, Any] = {
            "workload": key[0],
            "fixture": key[1],
            "digest": first["digest"],
            "absolute": {
                "A_rate_per_second": absolute_statistics(baseline_rates),
                "B_rate_per_second": absolute_statistics(candidate_rates),
                "A_elapsed_ns": absolute_statistics(
                    [mapping[key]["elapsed_ns"] for mapping in left_maps]
                ),
                "B_elapsed_ns": absolute_statistics(
                    [mapping[key]["elapsed_ns"] for mapping in right_maps]
                ),
            },
            "B_over_A": {
                "method": "unpaired_ratio_of_medians",
                "median": ratio,
                "unpaired_bootstrap_ci95": [low, high],
                "bootstrap_iterations": bootstrap_iterations,
            },
        }
        for rss_field in ("runner_rss_kib", "rss_kib"):
            baseline_rss = [mapping[key].get(rss_field) for mapping in left_maps]
            candidate_rss = [mapping[key].get(rss_field) for mapping in right_maps]
            if all(value is not None for value in baseline_rss + candidate_rss):
                label = (
                    "runner_rss_kib"
                    if rss_field == "runner_rss_kib"
                    else "native_rss_kib"
                )
                metric["absolute"][f"A_{label}"] = absolute_statistics(baseline_rss)
                metric["absolute"][f"B_{label}"] = absolute_statistics(candidate_rss)
        counter_comparison = {}
        for name in sorted(first["counters"]):
            baseline_values = [mapping[key]["counters"][name] for mapping in left_maps]
            candidate_values = [
                mapping[key]["counters"][name] for mapping in right_maps
            ]
            counter_comparison[name] = {
                "classification": counter_classification(
                    name,
                    workload=str(first["workload"]),
                    semantics=first["semantics"],
                ),
                "A": _counter_value_statistics(baseline_values),
                "B": _counter_value_statistics(candidate_values),
                "identical_across_collections": (
                    baseline_values == candidate_values
                    if len(baseline_values) == len(candidate_values)
                    else False
                ),
            }
        if counter_comparison:
            metric["counters"] = counter_comparison
        metrics.append(metric)
    return metrics


def compare_collections(
    baseline: Mapping[str, Any],
    candidate: Mapping[str, Any],
    *,
    bootstrap_iterations: int = DEFAULT_BOOTSTRAP_ITERATIONS,
) -> dict[str, Any]:
    if (
        baseline.get("schema") != RESULT_SCHEMA
        or candidate.get("schema") != RESULT_SCHEMA
    ):
        raise BenchmarkContractError("unsupported collection schema")
    if baseline.get("mode") != "collect" or candidate.get("mode") != "collect":
        raise BenchmarkContractError("compare requires two collect artifacts")
    validate_manifest_compatibility(baseline["manifest"], candidate["manifest"])
    baseline_samples, candidate_samples = baseline["samples"], candidate["samples"]
    if len(baseline_samples) != len(candidate_samples) or not baseline_samples:
        raise BenchmarkContractError(
            "collections require the same non-zero sample count"
        )
    settings_fields = ("warmups", "runs", "cpu_set", "timeout_seconds")
    baseline_settings = baseline.get("settings", {})
    candidate_settings = candidate.get("settings", {})
    settings_mismatches = [
        name
        for name in settings_fields
        if baseline_settings.get(name) != candidate_settings.get(name)
    ]
    if settings_mismatches:
        raise BenchmarkContractError(
            "collection settings mismatch: " + ", ".join(settings_mismatches)
        )
    return {
        "schema": RESULT_SCHEMA,
        "mode": "compare",
        "manifests": {"A": baseline["manifest"], "B": candidate["manifest"]},
        "counter_contract": counter_contract_manifest(),
        "settings": {
            "baseline_samples": len(baseline_samples),
            "candidate_samples": len(candidate_samples),
            "source_order": "independent collections",
            "statistical_method": "unpaired bootstrap",
            "acceptance_grade_paired_ab": False,
            "bootstrap_iterations": bootstrap_iterations,
        },
        "collections": {"A": baseline_samples, "B": candidate_samples},
        "comparison": _comparison_from_unpaired_samples(
            baseline_samples,
            candidate_samples,
            bootstrap_iterations=bootstrap_iterations,
        ),
    }


def _command(value: str) -> list[str]:
    command = shlex.split(value)
    if not command:
        raise argparse.ArgumentTypeError("command must not be empty")
    return command


def _positive(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be positive")
    return parsed


def _non_negative(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be non-negative")
    return parsed


def _add_execution_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--cpu-set", type=parse_cpu_set, default=None)
    parser.add_argument("--warmups", type=_non_negative, default=DEFAULT_WARMUPS)
    parser.add_argument("--timeout", type=float)
    parser.add_argument("--output", type=Path)


def _emit(payload: Mapping[str, Any], output: Path | None) -> None:
    if output is not None:
        atomic_write_json(output, payload)
        print(f"saved {output}", file=sys.stderr)
    else:
        print(json.dumps(payload, indent=2, sort_keys=True))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="mode", required=True)

    collect_parser = subparsers.add_parser("collect", help="collect one implementation")
    collect_parser.add_argument("--command", required=True, type=_command)
    collect_parser.add_argument(
        "--runs", "--samples", type=_positive, default=DEFAULT_PAIRS
    )
    collect_parser.add_argument("--label", default="local")
    collect_parser.add_argument("--repo-root", type=Path, default=ROOT)
    collect_parser.add_argument("--cmake-cache", type=Path)
    _add_execution_options(collect_parser)

    paired_parser = subparsers.add_parser("paired", help="run A/B in ABBA order")
    paired_parser.add_argument("--baseline-command", required=True, type=_command)
    paired_parser.add_argument("--candidate-command", required=True, type=_command)
    paired_parser.add_argument("--pairs", type=_positive, default=DEFAULT_PAIRS)
    paired_parser.add_argument(
        "--bootstrap-iterations", type=_positive, default=DEFAULT_BOOTSTRAP_ITERATIONS
    )
    paired_parser.add_argument("--baseline-repo-root", type=Path, default=ROOT)
    paired_parser.add_argument("--candidate-repo-root", type=Path, default=ROOT)
    paired_parser.add_argument("--baseline-manifest", type=Path)
    paired_parser.add_argument("--candidate-manifest", type=Path)
    paired_parser.add_argument("--baseline-cmake-cache", type=Path)
    paired_parser.add_argument("--candidate-cmake-cache", type=Path)
    paired_parser.add_argument(
        "--rotate-binary-slots",
        action="store_true",
        help=(
            "stage native ELF binaries into two fixed-inode slots, cross their "
            "placement every pair, and bootstrap two-pair crossover blocks"
        ),
    )
    _add_execution_options(paired_parser)

    compare_parser = subparsers.add_parser(
        "compare", help="compare two collect JSON files"
    )
    compare_parser.add_argument("baseline", type=Path)
    compare_parser.add_argument("candidate", type=Path)
    compare_parser.add_argument(
        "--bootstrap-iterations", type=_positive, default=DEFAULT_BOOTSTRAP_ITERATIONS
    )
    compare_parser.add_argument("--output", type=Path)

    args = parser.parse_args(argv)
    if args.mode == "collect":
        payload = collect_runs(
            args.command,
            runs=args.runs,
            warmups=args.warmups,
            cpu_set=args.cpu_set,
            timeout=args.timeout,
            label=args.label,
            repo_root=args.repo_root,
            cmake_cache=args.cmake_cache,
        )
    elif args.mode == "paired":
        payload = run_paired(
            args.baseline_command,
            args.candidate_command,
            pairs=args.pairs,
            warmups=args.warmups,
            cpu_set=args.cpu_set,
            timeout=args.timeout,
            bootstrap_iterations=args.bootstrap_iterations,
            baseline_repo_root=args.baseline_repo_root,
            candidate_repo_root=args.candidate_repo_root,
            baseline_manifest=_json_load(args.baseline_manifest)
            if args.baseline_manifest
            else None,
            candidate_manifest=_json_load(args.candidate_manifest)
            if args.candidate_manifest
            else None,
            baseline_cmake_cache=args.baseline_cmake_cache,
            candidate_cmake_cache=args.candidate_cmake_cache,
            rotate_binary_slots=args.rotate_binary_slots,
        )
    else:
        payload = compare_collections(
            _json_load(args.baseline),
            _json_load(args.candidate),
            bootstrap_iterations=args.bootstrap_iterations,
        )
    _emit(payload, args.output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
