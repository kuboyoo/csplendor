#!/usr/bin/env python3
"""Compact raw Phase-0 baseline payloads into reviewable evidence.

The raw output of :mod:`scripts.run_phase0_baseline` intentionally contains
complete host manifests and every native record.  That is useful while an
experiment is running, but unnecessarily large for a committed audit trail.
This tool keeps the identities, summaries, every paired sample, and fixed-slot
chain of custody needed to reproduce the statistical result::

    python scripts/compact_phase0_evidence.py \
      --input formal=/tmp/phase0-formal.json \
      --input shared=/tmp/phase0-shared.json \
      --output doc/performance_experiments/phase0_evidence.json

The output has no timestamp or input pathname, so identical named inputs
produce byte-for-byte identical JSON.  The output is replaced atomically.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import re
from copy import deepcopy
from pathlib import Path
from typing import Any, Mapping, Sequence

try:
    from benchmark_manifest import atomic_write_json
    from run_paired_benchmarks import counter_contract_manifest
except ModuleNotFoundError:  # Importing as scripts.compact_phase0_evidence.
    from scripts.benchmark_manifest import atomic_write_json
    from scripts.run_paired_benchmarks import counter_contract_manifest


INPUT_SCHEMA = "csplendor.phase0_baseline.v1"
CASE_SCHEMA = "csplendor.paired_benchmark.v1"
OUTPUT_SCHEMA = "csplendor.phase0_compact_evidence.v1"
_LABEL_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
_SHA256_PATTERN = re.compile(r"[0-9a-f]{64}\Z")


class EvidenceContractError(ValueError):
    """Raised when raw evidence is incomplete or internally inconsistent."""


def _mapping(value: Any, field: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise EvidenceContractError(f"{field} must be an object")
    return value


def _list(value: Any, field: str) -> list[Any]:
    if not isinstance(value, list):
        raise EvidenceContractError(f"{field} must be an array")
    return value


def _string(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise EvidenceContractError(f"{field} must be a non-empty string")
    return value


def _number(value: Any, field: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise EvidenceContractError(f"{field} must be numeric")
    result = float(value)
    if not math.isfinite(result) or (positive and result <= 0.0):
        qualifier = "positive " if positive else "finite "
        raise EvidenceContractError(f"{field} must be {qualifier}numeric")
    return result


def _integer(value: Any, field: str, *, nonnegative: bool = True) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise EvidenceContractError(f"{field} must be an integer")
    if nonnegative and value < 0:
        raise EvidenceContractError(f"{field} must be non-negative")
    return value


def _sha256(value: Any, field: str) -> str:
    result = _string(value, field)
    if _SHA256_PATTERN.fullmatch(result) is None:
        raise EvidenceContractError(f"{field} must be a lowercase SHA-256")
    return result


def parse_named_input(value: str) -> tuple[str, Path]:
    """Parse one ``LABEL=PATH`` command-line value."""
    if "=" not in value:
        raise argparse.ArgumentTypeError("input must be LABEL=PATH")
    label, path_text = value.split("=", 1)
    if _LABEL_PATTERN.fullmatch(label) is None:
        raise argparse.ArgumentTypeError(
            "input label must contain only letters, digits, '.', '_' or '-'"
        )
    if not path_text:
        raise argparse.ArgumentTypeError("input path must not be empty")
    return label, Path(path_text)


def _manifest_binary_identity(
    case: Mapping[str, Any], side: str, field: str
) -> dict[str, Any]:
    manifests = _mapping(case.get("manifests"), f"{field}.manifests")
    manifest = _mapping(manifests.get(side), f"{field}.manifests.{side}")
    binary = _mapping(manifest.get("binary"), f"{field}.manifests.{side}.binary")
    identity = {
        "path": _string(binary.get("path"), f"{field}.binary.{side}.path"),
        "sha256": _sha256(binary.get("sha256"), f"{field}.binary.{side}.sha256"),
        "size_bytes": _integer(
            binary.get("size_bytes"), f"{field}.binary.{side}.size_bytes"
        ),
    }
    return identity


def _compact_absolute(value: Any, field: str) -> dict[str, Any]:
    absolute = _mapping(value, field)
    result: dict[str, Any] = {}
    for metric_name, raw_summary in absolute.items():
        summary = _mapping(raw_summary, f"{field}.{metric_name}")
        compact = {key: deepcopy(item) for key, item in summary.items() if key != "raw"}
        if not compact:
            raise EvidenceContractError(f"{field}.{metric_name} has no non-raw summary")
        result[str(metric_name)] = compact
    if not result:
        raise EvidenceContractError(f"{field} must not be empty")
    return result


def _find_record(
    sample: Mapping[str, Any], workload: str, fixture: str, field: str
) -> Mapping[str, Any]:
    records = _list(sample.get("records"), f"{field}.records")
    matches = [
        record
        for record in records
        if isinstance(record, Mapping)
        and record.get("workload") == workload
        and record.get("fixture") == fixture
    ]
    if len(matches) != 1:
        raise EvidenceContractError(
            f"{field} must contain exactly one {workload}/{fixture} record"
        )
    return matches[0]


def _strict_json_equal(left: Any, right: Any) -> bool:
    """Compare JSON values without treating ``True`` and ``1`` as equal."""
    if type(left) is not type(right):
        return False
    if isinstance(left, dict):
        return left.keys() == right.keys() and all(
            _strict_json_equal(left[key], right[key]) for key in left
        )
    if isinstance(left, list):
        return len(left) == len(right) and all(
            _strict_json_equal(left_item, right_item)
            for left_item, right_item in zip(left, right)
        )
    return bool(left == right)


def _canonical_record_metadata(record: Mapping[str, Any], field: str) -> dict[str, Any]:
    semantics = _mapping(record.get("semantics"), f"{field}.semantics")
    counters = _mapping(record.get("counters"), f"{field}.counters")
    contract = counter_contract_manifest()
    configuration_names = set(
        _list(contract.get("configuration_exact"), "counter contract.configuration")
    )
    correctness_names = set(
        _list(contract.get("correctness_exact"), "counter contract.correctness")
    )
    if configuration_names & correctness_names:
        raise EvidenceContractError("counter contract classifications overlap")
    return {
        "semantics": deepcopy(dict(semantics)),
        "counters": {
            "contract_schema": _string(
                contract.get("schema"), "counter contract.schema"
            ),
            "configuration": {
                name: deepcopy(counters[name])
                for name in sorted(counters.keys() & configuration_names)
            },
            "correctness": {
                name: deepcopy(counters[name])
                for name in sorted(counters.keys() & correctness_names)
            },
        },
    }


def _compact_slot(
    value: Any,
    *,
    side: str,
    manifest_sha256: str,
    required: bool,
    field: str,
) -> dict[str, Any] | None:
    if value is None:
        if required:
            raise EvidenceContractError(f"{field} is required for slot crossover")
        return None
    slot = _mapping(value, field)
    logical_side = _string(slot.get("logical_side"), f"{field}.logical_side")
    if logical_side != side:
        raise EvidenceContractError(
            f"{field}.logical_side is {logical_side!r}, expected {side!r}"
        )
    source_sha256 = _sha256(
        slot.get("source_binary_sha256"), f"{field}.source_binary_sha256"
    )
    staged_sha256 = _sha256(
        slot.get("staged_binary_sha256"), f"{field}.staged_binary_sha256"
    )
    post_sha256 = _sha256(slot.get("post_run_sha256"), f"{field}.post_run_sha256")
    if not (source_sha256 == staged_sha256 == post_sha256 == manifest_sha256):
        raise EvidenceContractError(
            f"{field} source/staged/post SHA does not match manifest side {side}"
        )
    return {
        "slot_id": _string(slot.get("slot_id"), f"{field}.slot_id"),
        "slot_device": _integer(slot.get("slot_device"), f"{field}.slot_device"),
        "slot_inode": _integer(slot.get("slot_inode"), f"{field}.slot_inode"),
        "layout_id": _string(slot.get("layout_id"), f"{field}.layout_id"),
        "layout_index": _integer(slot.get("layout_index"), f"{field}.layout_index"),
        "logical_side": logical_side,
        "source_binary_sha256": source_sha256,
        "staged_binary_sha256": staged_sha256,
        "post_run_sha256": post_sha256,
    }


def _compact_sample(
    sample_value: Any,
    *,
    workload: str,
    fixture: str,
    digest: str,
    side: str,
    manifest_sha256: str,
    slot_required: bool,
    field: str,
) -> dict[str, Any]:
    sample = _mapping(sample_value, field)
    record = _find_record(sample, workload, fixture, field)
    if record.get("digest") != digest:
        raise EvidenceContractError(f"{field}.record digest differs from comparison")
    runner_rss = _integer(sample.get("runner_rss_kib"), f"{field}.runner_rss_kib")
    record_runner_rss = record.get("runner_rss_kib")
    if record_runner_rss is not None and record_runner_rss != runner_rss:
        raise EvidenceContractError(
            f"{field}.record runner RSS differs from process runner RSS"
        )
    return {
        "rate_per_second": _number(
            record.get("rate_per_second"), f"{field}.rate_per_second", positive=True
        ),
        "elapsed_ns": _integer(record.get("elapsed_ns"), f"{field}.elapsed_ns"),
        "operations": _integer(record.get("operations"), f"{field}.operations"),
        "native_rss_kib": _integer(record.get("rss_kib"), f"{field}.rss_kib"),
        "runner_rss_kib": runner_rss,
        "binary_slot": _compact_slot(
            sample.get("binary_slot"),
            side=side,
            manifest_sha256=manifest_sha256,
            required=slot_required,
            field=f"{field}.binary_slot",
        ),
    }


def _validate_raw_sequence(
    summary: Mapping[str, Any],
    key: str,
    expected: Sequence[float],
    field: str,
) -> None:
    raw_summary = _mapping(summary.get(key), f"{field}.{key}")
    raw = _list(raw_summary.get("raw"), f"{field}.{key}.raw")
    if len(raw) != len(expected):
        raise EvidenceContractError(f"{field}.{key}.raw length differs from pairs")
    for index, (actual, wanted) in enumerate(zip(raw, expected)):
        actual_number = _number(actual, f"{field}.{key}.raw[{index}]")
        if not math.isclose(actual_number, wanted, rel_tol=1e-12, abs_tol=0.0):
            raise EvidenceContractError(
                f"{field}.{key}.raw[{index}] differs from paired sample"
            )


def _compact_case(case_value: Any, case_index: int) -> dict[str, Any]:
    field = f"cases[{case_index}]"
    case = _mapping(case_value, field)
    if case.get("schema") != CASE_SCHEMA or case.get("mode") != "paired":
        raise EvidenceContractError(f"{field} is not a paired benchmark payload")
    settings = _mapping(case.get("settings"), f"{field}.settings")
    rotate_slots = settings.get("rotate_binary_slots") is True
    identities = {
        side: _manifest_binary_identity(case, side, field) for side in ("A", "B")
    }
    comparisons = _list(case.get("comparison"), f"{field}.comparison")
    if len(comparisons) != 1:
        raise EvidenceContractError(f"{field} must contain exactly one comparison")
    comparison = _mapping(comparisons[0], f"{field}.comparison[0]")
    workload = _string(comparison.get("workload"), f"{field}.workload")
    fixture = _string(comparison.get("fixture"), f"{field}.fixture")
    digest = _string(comparison.get("digest"), f"{field}.digest")
    ratio = _mapping(comparison.get("B_over_A"), f"{field}.B_over_A")
    ratio_summary = {
        key: deepcopy(value)
        for key, value in ratio.items()
        if key not in {"raw", "pair_raw"}
    }
    if "median" not in ratio_summary:
        raise EvidenceContractError(f"{field}.B_over_A has no median")

    pair_values = _list(case.get("pairs"), f"{field}.pairs")
    if not pair_values:
        raise EvidenceContractError(f"{field}.pairs must not be empty")
    configured_pairs = settings.get("pairs")
    if configured_pairs != len(pair_values):
        raise EvidenceContractError(
            f"{field}.settings.pairs differs from the recorded pair count"
        )
    compact_pairs: list[dict[str, Any]] = []
    pair_ratios: list[float] = []
    slot_identities: dict[str, tuple[int, int]] = {}
    canonical_metadata: dict[str, Any] | None = None
    for expected_index, pair_value in enumerate(pair_values):
        pair_field = f"{field}.pairs[{expected_index}]"
        pair = _mapping(pair_value, pair_field)
        pair_index = _integer(pair.get("pair_index"), f"{pair_field}.pair_index")
        if pair_index != expected_index:
            raise EvidenceContractError(f"{pair_field}.pair_index is not contiguous")
        order = _list(pair.get("order"), f"{pair_field}.order")
        expected_order = ["A", "B"] if expected_index % 2 == 0 else ["B", "A"]
        if order != expected_order:
            raise EvidenceContractError(f"{pair_field}.order does not follow ABBA")
        samples = {
            side: _compact_sample(
                pair.get(side),
                workload=workload,
                fixture=fixture,
                digest=digest,
                side=side,
                manifest_sha256=identities[side]["sha256"],
                slot_required=rotate_slots,
                field=f"{pair_field}.{side}",
            )
            for side in ("A", "B")
        }
        for side in ("A", "B"):
            raw_sample = _mapping(pair.get(side), f"{pair_field}.{side}")
            record = _find_record(raw_sample, workload, fixture, f"{pair_field}.{side}")
            current_metadata = _canonical_record_metadata(
                record, f"{pair_field}.{side}.record"
            )
            if canonical_metadata is None:
                canonical_metadata = current_metadata
                continue
            if not _strict_json_equal(
                canonical_metadata["semantics"], current_metadata["semantics"]
            ):
                raise EvidenceContractError(
                    f"{pair_field}.{side} semantics differ from canonical sample"
                )
            if not _strict_json_equal(
                canonical_metadata["counters"], current_metadata["counters"]
            ):
                raise EvidenceContractError(
                    f"{pair_field}.{side} configuration/correctness counters "
                    "differ from canonical sample"
                )
        ratio_value = samples["B"]["rate_per_second"] / samples["A"]["rate_per_second"]
        pair_ratios.append(ratio_value)
        layout = pair.get("binary_slot_layout")
        if rotate_slots:
            layout_mapping = _mapping(layout, f"{pair_field}.binary_slot_layout")
            for side in ("A", "B"):
                slot = samples[side]["binary_slot"]
                assert slot is not None
                if layout_mapping.get(side) != slot["slot_id"]:
                    raise EvidenceContractError(
                        f"{pair_field} layout differs from side {side} slot"
                    )
                slot_key = slot["slot_id"]
                current_identity = (slot["slot_device"], slot["slot_inode"])
                previous_identity = slot_identities.setdefault(
                    slot_key, current_identity
                )
                if previous_identity != current_identity:
                    raise EvidenceContractError(
                        f"{pair_field} changed fixed inode for {slot_key}"
                    )
        compact_pairs.append(
            {
                "pair_index": pair_index,
                "order": deepcopy(order),
                "binary_slot_layout": deepcopy(layout),
                "A": samples["A"],
                "B": samples["B"],
                "B_over_A": ratio_value,
            }
        )

    reported_pair_ratios = ratio.get("pair_raw")
    if reported_pair_ratios is None and not rotate_slots:
        reported_pair_ratios = ratio.get("raw")
    reported_pairs = _list(reported_pair_ratios, f"{field}.B_over_A.pair_raw")
    if len(reported_pairs) != len(pair_ratios):
        raise EvidenceContractError(f"{field}.B_over_A pair ratio count differs")
    for index, (reported, computed) in enumerate(zip(reported_pairs, pair_ratios)):
        if not math.isclose(
            _number(reported, f"{field}.B_over_A.pair_raw[{index}]"),
            computed,
            rel_tol=1e-12,
            abs_tol=0.0,
        ):
            raise EvidenceContractError(
                f"{field}.B_over_A.pair_raw[{index}] differs from rates"
            )

    crossover_blocks: list[dict[str, Any]] = []
    if rotate_slots:
        if len(pair_ratios) % 2:
            raise EvidenceContractError(f"{field} slot crossover needs even pairs")
        reported_blocks = _list(ratio.get("raw"), f"{field}.B_over_A.raw")
        if len(reported_blocks) != len(pair_ratios) // 2:
            raise EvidenceContractError(
                f"{field}.B_over_A crossover block count differs"
            )
        for block_index, reported in enumerate(reported_blocks):
            first_pair = block_index * 2
            computed = math.sqrt(pair_ratios[first_pair] * pair_ratios[first_pair + 1])
            reported_number = _number(reported, f"{field}.B_over_A.raw[{block_index}]")
            if not math.isclose(reported_number, computed, rel_tol=1e-12, abs_tol=0.0):
                raise EvidenceContractError(
                    f"{field}.B_over_A.raw[{block_index}] differs from rates"
                )
            crossover_blocks.append(
                {
                    "block_index": block_index,
                    "pair_indices": [first_pair, first_pair + 1],
                    "B_over_A_geometric_mean": reported_number,
                }
            )

    absolute = _mapping(comparison.get("absolute"), f"{field}.absolute")
    for side in ("A", "B"):
        _validate_raw_sequence(
            absolute,
            f"{side}_rate_per_second",
            [pair[side]["rate_per_second"] for pair in compact_pairs],
            f"{field}.absolute",
        )
        _validate_raw_sequence(
            absolute,
            f"{side}_native_rss_kib",
            [float(pair[side]["native_rss_kib"]) for pair in compact_pairs],
            f"{field}.absolute",
        )
        _validate_raw_sequence(
            absolute,
            f"{side}_runner_rss_kib",
            [float(pair[side]["runner_rss_kib"]) for pair in compact_pairs],
            f"{field}.absolute",
        )

    return {
        "manifest_binary_identities": identities,
        "settings": deepcopy(settings),
        "workload": workload,
        "fixture": fixture,
        "digest": digest,
        "canonical_semantics": canonical_metadata["semantics"],
        "canonical_counters": canonical_metadata["counters"],
        "absolute_summary": _compact_absolute(
            comparison.get("absolute"), f"{field}.absolute"
        ),
        "B_over_A_summary": ratio_summary,
        "pairs": compact_pairs,
        "crossover_blocks": crossover_blocks,
    }


def compact_inputs(named_inputs: Sequence[tuple[str, Path]]) -> dict[str, Any]:
    """Load and compact named Phase-0 JSON payloads deterministically."""
    if not named_inputs:
        raise EvidenceContractError("at least one named input is required")
    labels: set[str] = set()
    inputs: list[dict[str, Any]] = []
    for label, path in named_inputs:
        if _LABEL_PATTERN.fullmatch(label) is None:
            raise EvidenceContractError(f"invalid input label: {label!r}")
        if label in labels:
            raise EvidenceContractError(f"duplicate input label: {label}")
        labels.add(label)
        try:
            raw_document = path.read_bytes()
            document = json.loads(raw_document)
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            raise EvidenceContractError(
                f"cannot read {label} input: {error}"
            ) from error
        payload = _mapping(document, f"input {label}")
        if payload.get("schema") != INPUT_SCHEMA:
            raise EvidenceContractError(f"input {label} has unsupported schema")
        if payload.get("complete") is not True:
            raise EvidenceContractError(f"input {label} is not complete")
        cases = _list(payload.get("cases"), f"input {label}.cases")
        if not cases:
            raise EvidenceContractError(f"input {label} has no cases")
        inputs.append(
            {
                "label": label,
                "source": {
                    "schema": INPUT_SCHEMA,
                    "sha256": hashlib.sha256(raw_document).hexdigest(),
                    "size_bytes": len(raw_document),
                },
                "settings": deepcopy(
                    _mapping(payload.get("settings"), f"input {label}.settings")
                ),
                "cases": [
                    _compact_case(case, case_index)
                    for case_index, case in enumerate(cases)
                ],
            }
        )
    return {"schema": OUTPUT_SCHEMA, "inputs": inputs}


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        action="append",
        type=parse_named_input,
        required=True,
        dest="named_inputs",
        metavar="LABEL=PATH",
    )
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        evidence = compact_inputs(args.named_inputs)
        atomic_write_json(args.output, evidence)
    except EvidenceContractError as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
