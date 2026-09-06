"""Contracts for compact, committed Phase-0 evidence."""

from __future__ import annotations

import json
import math
from copy import deepcopy

import pytest

from scripts import compact_phase0_evidence as compactor

_SHA_A = "a" * 64
_SHA_B = "b" * 64


def _summary(raw):
    return {"raw": raw, "median": raw[-1], "p50": raw[-1], "p95": raw[-1]}


def _sample(side, pair_index, rate, native_rss, runner_rss):
    order = ("A", "B") if pair_index % 2 == 0 else ("B", "A")
    slot_id = "slot0" if side == order[0] else "slot1"
    side_sha = _SHA_A if side == "A" else _SHA_B
    layout_id = "A-slot0_B-slot1" if pair_index % 2 == 0 else "A-slot1_B-slot0"
    return {
        "runner_rss_kib": runner_rss,
        "records": [
            {
                "workload": "shared_tree",
                "fixture": "midgame_250",
                "digest": "semantic-digest",
                "rate_per_second": rate,
                "elapsed_ns": round(1_000_000_000 / rate),
                "operations": 1_000,
                "rss_kib": native_rss,
                "runner_rss_kib": runner_rss,
                "semantics": {
                    "correct": True,
                    "fixture_ordered_legal_digest": "ordered-legal-digest",
                    "trace_digest": "canonical-trace",
                },
                "counters": {
                    "instrumentation_enabled": False,
                    "root_visit_digest": "root-visits",
                    "root_q_digest": "root-q",
                    "selected": 100,
                    # Measurement counters may vary and are deliberately omitted
                    # from committed canonical correctness evidence.
                    "global_allocation_calls": pair_index + (side == "B"),
                },
            }
        ],
        "binary_slot": {
            "slot_id": slot_id,
            "slot_device": 50,
            "slot_inode": 100 if slot_id == "slot0" else 101,
            "layout_id": layout_id,
            "layout_index": pair_index % 2,
            "logical_side": side,
            "source_binary_sha256": side_sha,
            "staged_binary_sha256": side_sha,
            "post_run_sha256": side_sha,
        },
    }


def _raw_payload():
    rates_a = [100.0, 80.0]
    rates_b = [110.0, 88.0]
    native_a = [1200, 1201]
    native_b = [1210, 1211]
    runner_a = [1300, 1301]
    runner_b = [1310, 1311]
    pairs = []
    for index in range(2):
        order = ["A", "B"] if index % 2 == 0 else ["B", "A"]
        layout = {order[0]: "slot0", order[1]: "slot1"}
        pairs.append(
            {
                "pair_index": index,
                "order": order,
                "binary_slot_layout": {
                    **layout,
                    "layout_id": (
                        "A-slot0_B-slot1" if index == 0 else "A-slot1_B-slot0"
                    ),
                },
                "A": _sample(
                    "A", index, rates_a[index], native_a[index], runner_a[index]
                ),
                "B": _sample(
                    "B", index, rates_b[index], native_b[index], runner_b[index]
                ),
            }
        )
    pair_ratios = [right / left for left, right in zip(rates_a, rates_b)]
    block_ratio = math.sqrt(pair_ratios[0] * pair_ratios[1])
    case_settings = {
        "pairs": 2,
        "warmups_per_side": 3,
        "bootstrap_iterations": 100,
        "cpu_set": [4],
        "order": "ABBA",
        "rotate_binary_slots": True,
        "statistical_unit": "two_pair_crossover_block",
        "binary_slot_policy": "two_private_fixed_inodes_crossed_every_pair",
    }
    return {
        "schema": compactor.INPUT_SCHEMA,
        "complete": True,
        "settings": deepcopy(case_settings),
        "cases": [
            {
                "schema": compactor.CASE_SCHEMA,
                "mode": "paired",
                "settings": case_settings,
                "manifests": {
                    "A": {
                        "binary": {
                            "path": "/tmp/reference-bench",
                            "sha256": _SHA_A,
                            "size_bytes": 10,
                        }
                    },
                    "B": {
                        "binary": {
                            "path": "/tmp/candidate-bench",
                            "sha256": _SHA_B,
                            "size_bytes": 11,
                        }
                    },
                },
                "pairs": pairs,
                "comparison": [
                    {
                        "workload": "shared_tree",
                        "fixture": "midgame_250",
                        "digest": "semantic-digest",
                        "absolute": {
                            "A_rate_per_second": _summary(rates_a),
                            "B_rate_per_second": _summary(rates_b),
                            "A_native_rss_kib": _summary(native_a),
                            "B_native_rss_kib": _summary(native_b),
                            "A_runner_rss_kib": _summary(runner_a),
                            "B_runner_rss_kib": _summary(runner_b),
                        },
                        "B_over_A": {
                            "method": "two_pair_binary_slot_crossover",
                            "pair_raw": pair_ratios,
                            "raw": [block_ratio],
                            "median": block_ratio,
                            "crossover_block_bootstrap_ci95": [
                                block_ratio,
                                block_ratio,
                            ],
                            "bootstrap_iterations": 100,
                        },
                    }
                ],
            }
        ],
    }


def _write_raw(path, payload=None):
    path.write_text(
        json.dumps(_raw_payload() if payload is None else payload),
        encoding="utf-8",
    )


def test_compacts_every_pair_and_fixed_slot_chain_atomically(tmp_path):
    raw = tmp_path / "raw.json"
    output = tmp_path / "compact.json"
    _write_raw(raw)

    assert compactor.main(["--input", f"formal={raw}", "--output", str(output)]) == 0
    first_bytes = output.read_bytes()
    evidence = json.loads(first_bytes)
    assert evidence["schema"] == compactor.OUTPUT_SCHEMA
    experiment = evidence["inputs"][0]
    assert experiment["label"] == "formal"
    assert experiment["source"]["size_bytes"] == raw.stat().st_size
    case = experiment["cases"][0]
    assert case["manifest_binary_identities"]["A"]["sha256"] == _SHA_A
    assert case["workload"] == "shared_tree"
    assert case["canonical_semantics"] == {
        "correct": True,
        "fixture_ordered_legal_digest": "ordered-legal-digest",
        "trace_digest": "canonical-trace",
    }
    assert case["canonical_counters"] == {
        "contract_schema": "csplendor.counter_contract.v1",
        "configuration": {"instrumentation_enabled": False},
        "correctness": {
            "root_q_digest": "root-q",
            "root_visit_digest": "root-visits",
            "selected": 100,
        },
    }
    assert "global_allocation_calls" not in json.dumps(case["canonical_counters"])
    assert "raw" not in case["absolute_summary"]["A_rate_per_second"]
    assert "raw" not in case["B_over_A_summary"]
    assert "pair_raw" not in case["B_over_A_summary"]
    assert [pair["pair_index"] for pair in case["pairs"]] == [0, 1]
    assert case["pairs"][0]["B_over_A"] == pytest.approx(1.1)
    assert case["pairs"][1]["A"]["runner_rss_kib"] == 1301
    slot = case["pairs"][1]["A"]["binary_slot"]
    assert slot == {
        "layout_id": "A-slot1_B-slot0",
        "layout_index": 1,
        "logical_side": "A",
        "post_run_sha256": _SHA_A,
        "slot_device": 50,
        "slot_id": "slot1",
        "slot_inode": 101,
        "source_binary_sha256": _SHA_A,
        "staged_binary_sha256": _SHA_A,
    }
    assert case["crossover_blocks"] == [
        {
            "block_index": 0,
            "pair_indices": [0, 1],
            "B_over_A_geometric_mean": pytest.approx(1.1),
        }
    ]
    assert list(tmp_path.glob(".compact.json.*.tmp")) == []

    assert compactor.main(["--input", f"formal={raw}", "--output", str(output)]) == 0
    assert output.read_bytes() == first_bytes


def test_multiple_named_inputs_are_kept_in_argument_order(tmp_path):
    formal = tmp_path / "formal.json"
    shared = tmp_path / "shared.json"
    _write_raw(formal)
    second = _raw_payload()
    second["cases"][0]["comparison"][0]["digest"] = "shared-digest"
    for pair in second["cases"][0]["pairs"]:
        pair["A"]["records"][0]["digest"] = "shared-digest"
        pair["B"]["records"][0]["digest"] = "shared-digest"
    _write_raw(shared, second)

    evidence = compactor.compact_inputs([("formal", formal), ("shared-audit", shared)])
    assert [item["label"] for item in evidence["inputs"]] == [
        "formal",
        "shared-audit",
    ]
    assert evidence["inputs"][1]["cases"][0]["digest"] == "shared-digest"


def test_rejects_incomplete_duplicate_or_tampered_evidence(tmp_path):
    raw = tmp_path / "raw.json"
    incomplete = _raw_payload()
    incomplete["complete"] = False
    _write_raw(raw, incomplete)
    with pytest.raises(compactor.EvidenceContractError, match="not complete"):
        compactor.compact_inputs([("formal", raw)])

    _write_raw(raw)
    with pytest.raises(compactor.EvidenceContractError, match="duplicate"):
        compactor.compact_inputs([("formal", raw), ("formal", raw)])

    tampered = _raw_payload()
    tampered["cases"][0]["pairs"][1]["A"]["binary_slot"]["post_run_sha256"] = "c" * 64
    _write_raw(raw, tampered)
    with pytest.raises(compactor.EvidenceContractError, match="does not match"):
        compactor.compact_inputs([("formal", raw)])


def test_rejects_semantics_or_correctness_counter_drift(tmp_path):
    raw = tmp_path / "raw.json"
    semantics_drift = _raw_payload()
    semantics_drift["cases"][0]["pairs"][1]["B"]["records"][0]["semantics"][
        "trace_digest"
    ] = "changed"
    _write_raw(raw, semantics_drift)
    with pytest.raises(compactor.EvidenceContractError, match="semantics differ"):
        compactor.compact_inputs([("formal", raw)])

    counter_drift = _raw_payload()
    counter_drift["cases"][0]["pairs"][1]["A"]["records"][0]["counters"][
        "root_visit_digest"
    ] = "changed"
    _write_raw(raw, counter_drift)
    with pytest.raises(
        compactor.EvidenceContractError,
        match="configuration/correctness counters differ",
    ):
        compactor.compact_inputs([("formal", raw)])


def test_named_input_parser_rejects_ambiguous_values():
    assert compactor.parse_named_input("formal=/tmp/raw.json") == (
        "formal",
        compactor.Path("/tmp/raw.json"),
    )
    with pytest.raises(compactor.argparse.ArgumentTypeError):
        compactor.parse_named_input("missing-separator")
    with pytest.raises(compactor.argparse.ArgumentTypeError):
        compactor.parse_named_input("bad label=/tmp/raw.json")
