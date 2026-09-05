"""Fast contracts for the reproducible native benchmark tools."""

from __future__ import annotations

import json
import os
import sys
import time
from copy import deepcopy
from pathlib import Path

import pytest

from scripts import benchmark_manifest as manifest_tool
from scripts import run_paired_benchmarks as runner


def _manifest(command="/tmp/a/bench"):
    return {
        "schema": manifest_tool.SCHEMA,
        "command": [command, "--suite", "rules"],
        "host": {
            "uname": {
                "system": "Linux",
                "node": "test-host",
                "release": "test",
                "machine": "x86_64",
            },
            "cpu": {
                "logical_count": 16,
                "benchmark_affinity": [4],
                "governors": {"4": "performance"},
                "selected_cpu_details": {
                    "4": {
                        "core_id": "4",
                        "physical_package_id": "0",
                        "thread_siblings_list": "4,12",
                    }
                },
                "selected_set_contains_smt_siblings": False,
                "topology": {
                    "fields": {
                        "Architecture": "x86_64",
                        "CPU(s)": "16",
                        "Model name": "Test CPU",
                        "Thread(s) per core": "2",
                        "Core(s) per socket": "8",
                        "Socket(s)": "1",
                    }
                },
            },
        },
        "tools": {
            "compiler": {
                "version": "g++ test",
                "resolved_path": "/usr/bin/g++",
                "sha256": "compiler-test",
            },
            "cmake": {"version": "cmake test"},
            "python": {"version": "3.12", "implementation": "CPython"},
        },
        "build": {
            "available": True,
            "benchmark_build_fingerprint_sha256": "build-test",
        },
    }


def _record(rate=100.0, **updates):
    operations = 1000
    elapsed_ns = round(operations * 1_000_000_000 / rate)
    value = {
        "schema": runner.ENGINE_SCHEMA,
        "workload": "legal_count",
        "fixture": "midgame_42_12",
        "seed": 42,
        "operations": operations,
        "elapsed_ns": elapsed_ns,
        "rate_per_second": rate,
        "rss_kib": 1234,
        "digest": "abc123",
        "counters": {"legal_actions": 250},
        "semantics": {"correct": True, "seed": 42, "ply": 12},
        "future_metadata": "preserved",
    }
    value.update(updates)
    return value


def _crossed_pair(index, *, baseline_rate, candidate_rate):
    order = runner.abba_order(index)
    layout = {order[0]: "slot0", order[1]: "slot1"}
    samples = {}
    for side, rate, digest in (
        ("A", baseline_rate, "source-a"),
        ("B", candidate_rate, "source-b"),
    ):
        samples[side] = {
            "records": [_record(rate=rate)],
            "binary_slot": {
                "slot_id": layout[side],
                "layout_index": index % 2,
                "source_binary_sha256": digest,
                "staged_binary_sha256": digest,
                "post_run_sha256": digest,
            },
        }
    return {
        "pair_index": index,
        "order": list(order),
        "binary_slot_layout": {**layout, "layout_id": f"layout-{index % 2}"},
        **samples,
    }


def test_statistics_and_paired_bootstrap_are_deterministic():
    summary = runner.absolute_statistics([4.0, 1.0, 3.0, 2.0])
    assert summary["raw"] == [4.0, 1.0, 3.0, 2.0]
    assert summary["median"] == summary["p50"] == 2.5
    assert summary["p95"] == pytest.approx(3.85)

    baseline = [100.0, 98.0, 102.0, 101.0, 99.0]
    candidate = [110.0, 107.8, 112.2, 111.1, 108.9]
    first = runner.paired_bootstrap_ratio_ci(
        baseline, candidate, iterations=500, seed=7
    )
    second = runner.paired_bootstrap_ratio_ci(
        baseline, candidate, iterations=500, seed=7
    )
    assert first == second
    assert first == pytest.approx((1.1, 1.1, 1.1))


def test_crossover_block_geometric_ratio_cancels_fixed_slot_bias():
    # Pair-level ratios are 1 and 4 because each binary swaps slots.  Their
    # two-pair crossover geometric mean recovers the true 2x effect.
    pairs = [
        _crossed_pair(0, baseline_rate=100.0, candidate_rate=100.0),
        _crossed_pair(1, baseline_rate=50.0, candidate_rate=200.0),
    ]
    comparison = runner._comparison_from_paired_samples(
        pairs,
        bootstrap_iterations=100,
        rotate_binary_slots=True,
    )
    ratio = comparison[0]["B_over_A"]
    assert ratio["method"] == "two_pair_binary_slot_crossover"
    assert ratio["pair_raw"] == pytest.approx([1.0, 4.0])
    assert ratio["raw"] == pytest.approx([2.0])
    assert ratio["median"] == pytest.approx(2.0)
    assert ratio["crossover_block_bootstrap_ci95"] == pytest.approx([2.0, 2.0])


def test_crossover_rejects_semantic_drift_in_any_of_four_cells():
    pairs = [
        _crossed_pair(0, baseline_rate=100.0, candidate_rate=100.0),
        _crossed_pair(1, baseline_rate=100.0, candidate_rate=100.0),
    ]
    # A/B still agree within the second pair, so only a four-cell comparison
    # against the first orientation detects this drift.
    pairs[1]["A"]["records"][0]["digest"] = "drifted"
    pairs[1]["B"]["records"][0]["digest"] = "drifted"
    with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
        runner._comparison_from_paired_samples(
            pairs,
            bootstrap_iterations=20,
            rotate_binary_slots=True,
        )


def test_binary_slot_rotation_requires_even_pairs():
    with pytest.raises(ValueError, match="even number"):
        runner.run_paired(
            ["A", "--suite", "rules"],
            ["B", "--suite", "rules"],
            pairs=1,
            warmups=0,
            rotate_binary_slots=True,
        )


def test_defaults_and_measured_execution_follow_abba(monkeypatch):
    assert runner.DEFAULT_WARMUPS == 3
    assert runner.DEFAULT_PAIRS == 21
    assert [runner.abba_order(index) for index in range(4)] == [
        ("A", "B"),
        ("B", "A"),
        ("A", "B"),
        ("B", "A"),
    ]

    calls = []

    def fake_manifest(command, **_kwargs):
        return _manifest(command[0])

    def fake_run(command, **_kwargs):
        side = command[0]
        calls.append(side)
        return {
            "execution_command": list(command),
            "runner_rss_kib": 1200,
            "records": [_record(rate=100.0 if side == "A" else 110.0)],
        }

    monkeypatch.setattr(runner, "collect_manifest", fake_manifest)
    monkeypatch.setattr(runner, "run_native_command", fake_run)
    result = runner.run_paired(
        ["A", "--suite", "rules"],
        ["B", "--suite", "rules"],
        warmups=3,
        pairs=4,
        cpu_set=[4],
        bootstrap_iterations=50,
    )
    assert calls == [
        "A",
        "B",
        "B",
        "A",
        "A",
        "B",
        "A",
        "B",
        "B",
        "A",
        "A",
        "B",
        "B",
        "A",
    ]
    assert [pair["order"] for pair in result["pairs"]] == [
        ["A", "B"],
        ["B", "A"],
        ["A", "B"],
        ["B", "A"],
    ]
    assert result["comparison"][0]["B_over_A"]["median"] == pytest.approx(1.1)


def test_paired_cli_binary_slot_rotation_is_explicit_opt_in(monkeypatch):
    captured = []

    def fake_paired(*_args, **kwargs):
        captured.append(kwargs)
        return {"schema": runner.RESULT_SCHEMA}

    monkeypatch.setattr(runner, "run_paired", fake_paired)
    monkeypatch.setattr(runner, "_emit", lambda *_args: None)
    assert (
        runner.main(
            [
                "paired",
                "--baseline-command",
                "a --suite rules",
                "--candidate-command",
                "b --suite rules",
                "--pairs",
                "2",
                "--rotate-binary-slots",
            ]
        )
        == 0
    )
    assert captured[0]["rotate_binary_slots"] is True


def test_manifest_and_native_metadata_mismatches_are_rejected():
    baseline_manifest = _manifest()
    candidate_manifest = _manifest("/tmp/b/bench")
    runner.validate_manifest_compatibility(baseline_manifest, candidate_manifest)

    candidate_manifest = deepcopy(candidate_manifest)
    candidate_manifest["host"]["cpu"]["benchmark_affinity"] = [5]
    with pytest.raises(runner.BenchmarkContractError, match="benchmark_affinity"):
        runner.validate_manifest_compatibility(baseline_manifest, candidate_manifest)

    with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
        runner.validate_record_pair(_record(), _record(digest="different"))
    with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
        runner.validate_record_pair(_record(), _record(future_metadata="changed"))
    instrumented = _record(counters={"instrumentation_enabled": True})
    portable = _record(counters={"instrumentation_enabled": False})
    with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
        runner.validate_record_pair(instrumented, portable)


@pytest.mark.parametrize("counter", [
    "solver_visible_purchase_refills", "solver_visible_purchase_generated",
    "solver_visible_purchase_visited", "solver_visible_purchase_visited_0",
    "solver_visible_purchase_visited_1", "solver_visible_purchase_visited_2_to_4",
    "solver_visible_purchase_visited_5_plus", "solver_visible_purchase_apply_calls",
])
def test_purchase_visit_counters_are_diagnostics_not_logical_work(counter):
    baseline = _record(counters={"instrumentation_enabled": True, counter: 30})
    candidate = _record(counters={"instrumentation_enabled": True, counter: 1})
    runner.validate_record_pair(baseline, candidate)
    comparison = runner._comparison_from_paired_samples(
        [{"A": {"records": [baseline]}, "B": {"records": [candidate]}}],
        bootstrap_iterations=20,
    )
    assert comparison[0]["counters"][counter]["classification"] == "measurement"


def test_counter_contract_separates_correctness_and_measurement():
    correctness_a = _record(
        counters={"instrumentation_enabled": False, "legal_moves": 20}
    )
    correctness_b = _record(
        counters={"instrumentation_enabled": False, "legal_moves": 21}
    )
    with pytest.raises(runner.BenchmarkContractError, match="correctness counter"):
        runner.validate_record_pair(correctness_a, correctness_b)

    digest_a = _record(
        counters={"instrumentation_enabled": False, "root_visit_digest": "aaaa"}
    )
    digest_b = _record(
        counters={"instrumentation_enabled": False, "root_visit_digest": "bbbb"}
    )
    with pytest.raises(runner.BenchmarkContractError, match="root_visit_digest"):
        runner.validate_record_pair(digest_a, digest_b)

    missing = _record(counters={"instrumentation_enabled": False})
    with pytest.raises(runner.BenchmarkContractError, match="counter key set"):
        runner.validate_record_pair(correctness_a, missing)

    purchase_a = _record(
        workload="purchase_apply",
        counters={"instrumentation_enabled": False, "purchase_transitions": 256},
    )
    purchase_b = deepcopy(purchase_a)
    runner.validate_record_pair(purchase_a, purchase_b)
    purchase_b["counters"]["purchase_transitions"] = 255
    with pytest.raises(runner.BenchmarkContractError, match="correctness counter"):
        runner.validate_record_pair(purchase_a, purchase_b)

    measured_a = _record(
        counters={"instrumentation_enabled": True, "exact_hash_calls": 100}
    )
    measured_b = _record(
        counters={"instrumentation_enabled": True, "exact_hash_calls": 90}
    )
    runner.validate_record_pair(measured_a, measured_b)
    comparison = runner._comparison_from_paired_samples(
        [{"A": {"records": [measured_a]}, "B": {"records": [measured_b]}}],
        bootstrap_iterations=20,
    )
    assert (
        comparison[0]["counters"]["exact_hash_calls"]["classification"] == "measurement"
    )

    parallel_a = _record(
        workload="parallel_scheduler",
        counters={"instrumentation_enabled": False, "root_visit_digest": "aaaa"},
        semantics={"correct": True, "threads": 4},
    )
    parallel_b = _record(
        workload="parallel_scheduler",
        counters={"instrumentation_enabled": False, "root_visit_digest": "bbbb"},
        semantics={"correct": True, "threads": 4},
    )
    runner.validate_record_pair(parallel_a, parallel_b)
    parallel_a["semantics"]["threads"] = 1
    parallel_b["semantics"]["threads"] = 1
    with pytest.raises(runner.BenchmarkContractError, match="root_visit_digest"):
        runner.validate_record_pair(parallel_a, parallel_b)

    # Root-parallel workers own independent deterministic trees, so their
    # merged root result remains an exact correctness contract even above 1T.
    root_a = _record(
        workload="root_parallel",
        counters={"instrumentation_enabled": False, "root_visit_digest": "aaaa"},
        semantics={"correct": True, "threads": 4},
    )
    root_b = _record(
        workload="root_parallel",
        counters={"instrumentation_enabled": False, "root_visit_digest": "bbbb"},
        semantics={"correct": True, "threads": 4},
    )
    with pytest.raises(runner.BenchmarkContractError, match="root_visit_digest"):
        runner.validate_record_pair(root_a, root_b)


def test_root_parallel_selected_action_is_exact_ab_semantics():
    semantics = {
        "correct": True,
        "threads": 8,
        "tree_domain": "exact",
        "selected_action_index": 17,
        "selected_action_code": "530",
        "selected_action_legal": True,
        "worker_result_digest": "0123456789abcdef",
    }
    baseline = _record(workload="root_parallel", semantics=semantics)
    runner.validate_record_pair(baseline, deepcopy(baseline))

    for field, value in (
        ("selected_action_index", 18),
        ("selected_action_code", "648"),
        ("worker_result_digest", "fedcba9876543210"),
    ):
        candidate = deepcopy(baseline)
        candidate["semantics"][field] = value
        with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
            runner.validate_record_pair(baseline, candidate)


def test_trace_and_solver_order_digests_are_exact_ab_semantics():
    trace_digest_fields = {
        "formal_expanded_key_sequence_digest",
        "formal_selected_action_sequence_digest",
        "formal_inference_request_sequence_digest",
        "formal_deterministic_trace_chain_digest",
        "formal_replay_tree_digest",
        "formal_initial_tree_digest",
    }
    trace_semantics = {
        "correct": True,
        "threads": 1,
        "formal_trace_available": True,
        "formal_trace_scope": "fresh_tree_untimed_deterministic_epoch_1t_owner_leaf_order",
        "formal_trace_repeatable": True,
        **{name: "0123456789abcdef" for name in trace_digest_fields},
    }
    baseline = _record(workload="parallel_scheduler", semantics=trace_semantics)
    runner.validate_record_pair(baseline, deepcopy(baseline))
    assert trace_digest_fields <= baseline["semantics"].keys()
    for field in trace_digest_fields:
        drifted = deepcopy(baseline)
        drifted["semantics"][field] = "fedcba9876543210"
        with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
            runner.validate_record_pair(baseline, drifted)

    solver_digest_fields = {
        "principal_line_action_sequence_digest",
        "principal_line_reveal_outcome_sequence_digest",
        "root_ordered_action_sequence_digest",
        "root_ordered_outcome_sequence_digest",
        "root_reveal_candidate_sequence_digest",
    }
    solver_semantics = {
        "correct": True,
        "root_order_probe_complete": True,
        "root_order_observable_scope": (
            "public_split_root_proof_ordered_actions_then_proof_outcomes"
        ),
        **{name: "0123456789abcdef" for name in solver_digest_fields},
    }
    solver = _record(workload="exact_reveal", semantics=solver_semantics)
    runner.validate_record_pair(solver, deepcopy(solver))
    assert solver_digest_fields <= solver["semantics"].keys()
    for field in solver_digest_fields:
        drifted = deepcopy(solver)
        drifted["semantics"][field] = "fedcba9876543210"
        with pytest.raises(runner.BenchmarkContractError, match="metadata mismatch"):
            runner.validate_record_pair(solver, drifted)


def test_native_row_requires_complete_consistent_contract():
    incomplete = _record()
    incomplete.pop("semantics")
    with pytest.raises(runner.BenchmarkContractError, match="semantics"):
        runner.normalize_record(incomplete)

    fractional = _record(operations=1.5)
    with pytest.raises(
        runner.BenchmarkContractError, match="operations must be an integer"
    ):
        runner.normalize_record(fractional)

    inconsistent = _record(rate_per_second=999.0)
    with pytest.raises(runner.BenchmarkContractError, match="inconsistent"):
        runner.normalize_record(inconsistent)


def test_jsonl_parser_preserves_unknown_fields_and_rejects_duplicate_keys():
    record = _record()
    parsed = runner.parse_native_json_lines(json.dumps(record) + "\n")
    assert parsed[0]["future_metadata"] == "preserved"
    with pytest.raises(runner.BenchmarkContractError, match="duplicate"):
        runner.parse_native_json_lines(
            "\n".join([json.dumps(record), json.dumps(record)])
        )


def test_runner_rss_is_per_row_only_for_single_workload(monkeypatch):
    first = _record()
    second = _record(workload="legal_codes")

    def fake_run(_command, _cpu_set, _timeout):
        return "\n".join((json.dumps(first), json.dumps(second))), 4321, ["bench"]

    monkeypatch.setattr(runner, "_run_with_rss", fake_run)
    multiple = runner.run_native_command(["bench"], cpu_set=[4])
    assert multiple["runner_rss_kib"] == 4321
    assert multiple["runner_rss_scope"] == "process"
    assert all("runner_rss_kib" not in row for row in multiple["records"])

    monkeypatch.setattr(
        runner,
        "_run_with_rss",
        lambda *_args: (json.dumps(first), 4321, ["bench"]),
    )
    single = runner.run_native_command(["bench"], cpu_set=[4])
    assert single["records"][0]["runner_rss_kib"] == 4321
    assert single["runner_rss_scope"] == "single_workload"


def test_compare_collections_is_unpaired_and_checks_settings():
    manifest_a = _manifest()
    manifest_b = _manifest("/tmp/b/bench")
    common_settings = {
        "warmups": 3,
        "runs": 2,
        "cpu_set": [4],
        "timeout_seconds": None,
    }
    baseline = {
        "schema": runner.RESULT_SCHEMA,
        "mode": "collect",
        "manifest": manifest_a,
        "settings": common_settings,
        "samples": [
            {"records": [_record(rate=100.0)]},
            {"records": [_record(rate=102.0)]},
        ],
    }
    candidate = {
        "schema": runner.RESULT_SCHEMA,
        "mode": "collect",
        "manifest": manifest_b,
        "settings": dict(common_settings),
        "samples": [
            {"records": [_record(rate=110.0)]},
            {"records": [_record(rate=112.0)]},
        ],
    }
    result = runner.compare_collections(baseline, candidate, bootstrap_iterations=50)
    assert result["settings"]["statistical_method"] == "unpaired bootstrap"
    assert result["settings"]["acceptance_grade_paired_ab"] is False
    ratio = result["comparison"][0]["B_over_A"]
    assert "unpaired_bootstrap_ci95" in ratio
    assert "paired_bootstrap_ci95" not in ratio

    candidate["settings"]["warmups"] = 0
    with pytest.raises(runner.BenchmarkContractError, match="warmups"):
        runner.compare_collections(baseline, candidate)


def test_manifest_allowlists_cache_and_compares_build_and_smt_metadata(tmp_path):
    cache = tmp_path / "CMakeCache.txt"
    cache.write_text(
        "CMAKE_BUILD_TYPE:STRING=Release\n"
        "CMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n"
        "CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG -DPRIVATE_VALUE=hidden\n"
        "CSPLENDOR_CARD_EQUIVALENCE_CLASSES:BOOL=OFF\n"
        "CSPLENDOR_COMPACT_FORCED_ACTIONS:BOOL=OFF\n"
        "CSPLENDOR_COMPACT_SOLVER_REASONS:BOOL=OFF\n"
        "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES:BOOL=OFF\n"
        "CSPLENDOR_CPU_TARGET:STRING=portable\n"
        "CSPLENDOR_CLOSED_FORM_RETURN_COUNT:BOOL=OFF\n"
        "CSPLENDOR_INCREMENTAL_EXACT_HASH:BOOL=OFF\n"
        "CSPLENDOR_NOBLE_ELIGIBILITY_TABLE:BOOL=OFF\n"
        "CSPLENDOR_PACKED_CODE_SINK:BOOL=OFF\n"
        "CSPLENDOR_RETURN_PATTERN_TABLE:BOOL=OFF\n"
        "CSPLENDOR_SINGLE_PASS_LEGAL_CODES:BOOL=OFF\n"
        "CSPLENDOR_SOLVER_PATH_STACK:BOOL=OFF\n"
        "CSPLENDOR_VERIFY_INCREMENTAL_HASH:BOOL=OFF\n"
        "UNRELATED_API_TOKEN:STRING=do-not-read-or-emit\n",
        encoding="utf-8",
    )
    metadata, compiler = manifest_tool._cmake_build_metadata(cache)
    rendered = json.dumps(metadata, sort_keys=True)
    assert compiler == "/usr/bin/c++"
    assert "UNRELATED_API_TOKEN" not in rendered
    assert "do-not-read-or-emit" not in rendered
    assert "PRIVATE_VALUE" not in rendered
    assert (
        metadata["allowlisted_entries"]["CSPLENDOR_INCREMENTAL_EXACT_HASH"]
        == "OFF"
    )
    assert (
        metadata["allowlisted_entries"]["CSPLENDOR_NOBLE_ELIGIBILITY_TABLE"]
        == "OFF"
    )
    assert metadata["allowlisted_entries"]["CSPLENDOR_VERIFY_INCREMENTAL_HASH"] == "OFF"
    assert metadata["allowlisted_entries"]["CSPLENDOR_SOLVER_PATH_STACK"] == "OFF"
    assert (
        metadata["allowlisted_entries"]["CSPLENDOR_CARD_EQUIVALENCE_CLASSES"]
        == "OFF"
    )
    assert metadata["allowlisted_entries"]["CSPLENDOR_COMPACT_FORCED_ACTIONS"] == "OFF"
    assert metadata["allowlisted_entries"]["CSPLENDOR_COMPACT_SOLVER_REASONS"] == "OFF"
    assert (
        metadata["allowlisted_entries"]["CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES"]
        == "OFF"
    )
    assert (
        metadata["allowlisted_entries"]["CMAKE_CXX_FLAGS_RELEASE"][
            "redacted_token_count"
        ]
        == 1
    )

    candidate_cache = tmp_path / "candidate-CMakeCache.txt"
    candidate_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_INCREMENTAL_EXACT_HASH:BOOL=OFF",
            "CSPLENDOR_INCREMENTAL_EXACT_HASH:BOOL=ON",
        ),
        encoding="utf-8",
    )
    candidate_metadata, _ = manifest_tool._cmake_build_metadata(candidate_cache)
    assert (
        candidate_metadata["allowlisted_entries"]["CSPLENDOR_INCREMENTAL_EXACT_HASH"]
        == "ON"
    )
    assert (
        candidate_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    noble_cache = tmp_path / "noble-CMakeCache.txt"
    noble_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_NOBLE_ELIGIBILITY_TABLE:BOOL=OFF",
            "CSPLENDOR_NOBLE_ELIGIBILITY_TABLE:BOOL=ON",
        ),
        encoding="utf-8",
    )
    noble_metadata, _ = manifest_tool._cmake_build_metadata(noble_cache)
    assert (
        noble_metadata["allowlisted_entries"]["CSPLENDOR_NOBLE_ELIGIBILITY_TABLE"]
        == "ON"
    )
    assert (
        noble_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    legal_codes_cache = tmp_path / "legal-codes-CMakeCache.txt"
    legal_codes_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_SINGLE_PASS_LEGAL_CODES:BOOL=OFF",
            "CSPLENDOR_SINGLE_PASS_LEGAL_CODES:BOOL=ON",
        ),
        encoding="utf-8",
    )
    legal_codes_metadata, _ = manifest_tool._cmake_build_metadata(
        legal_codes_cache
    )
    assert (
        legal_codes_metadata["allowlisted_entries"][
            "CSPLENDOR_SINGLE_PASS_LEGAL_CODES"
        ]
        == "ON"
    )
    assert (
        legal_codes_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    return_count_cache = tmp_path / "return-count-CMakeCache.txt"
    return_count_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_CLOSED_FORM_RETURN_COUNT:BOOL=OFF",
            "CSPLENDOR_CLOSED_FORM_RETURN_COUNT:BOOL=ON",
        ),
        encoding="utf-8",
    )
    return_count_metadata, _ = manifest_tool._cmake_build_metadata(
        return_count_cache
    )
    assert (
        return_count_metadata["allowlisted_entries"][
            "CSPLENDOR_CLOSED_FORM_RETURN_COUNT"
        ]
        == "ON"
    )
    assert (
        return_count_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    return_pattern_cache = tmp_path / "return-pattern-CMakeCache.txt"
    return_pattern_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_RETURN_PATTERN_TABLE:BOOL=OFF",
            "CSPLENDOR_RETURN_PATTERN_TABLE:BOOL=ON",
        ),
        encoding="utf-8",
    )
    return_pattern_metadata, _ = manifest_tool._cmake_build_metadata(
        return_pattern_cache
    )
    assert (
        return_pattern_metadata["allowlisted_entries"][
            "CSPLENDOR_RETURN_PATTERN_TABLE"
        ]
        == "ON"
    )
    assert (
        return_pattern_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    packed_code_cache = tmp_path / "packed-code-CMakeCache.txt"
    packed_code_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_PACKED_CODE_SINK:BOOL=OFF",
            "CSPLENDOR_PACKED_CODE_SINK:BOOL=ON",
        ),
        encoding="utf-8",
    )
    packed_code_metadata, _ = manifest_tool._cmake_build_metadata(
        packed_code_cache
    )
    assert (
        packed_code_metadata["allowlisted_entries"][
            "CSPLENDOR_PACKED_CODE_SINK"
        ]
        == "ON"
    )
    assert (
        packed_code_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    solver_path_cache = tmp_path / "solver-path-CMakeCache.txt"
    solver_path_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_SOLVER_PATH_STACK:BOOL=OFF",
            "CSPLENDOR_SOLVER_PATH_STACK:BOOL=ON",
        ),
        encoding="utf-8",
    )
    solver_path_metadata, _ = manifest_tool._cmake_build_metadata(
        solver_path_cache
    )
    assert (
        solver_path_metadata["allowlisted_entries"][
            "CSPLENDOR_SOLVER_PATH_STACK"
        ]
        == "ON"
    )
    assert (
        solver_path_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    card_classes_cache = tmp_path / "card-classes-CMakeCache.txt"
    card_classes_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_CARD_EQUIVALENCE_CLASSES:BOOL=OFF",
            "CSPLENDOR_CARD_EQUIVALENCE_CLASSES:BOOL=ON",
        ),
        encoding="utf-8",
    )
    card_classes_metadata, _ = manifest_tool._cmake_build_metadata(
        card_classes_cache
    )
    assert (
        card_classes_metadata["allowlisted_entries"][
            "CSPLENDOR_CARD_EQUIVALENCE_CLASSES"
        ]
        == "ON"
    )
    assert (
        card_classes_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    compact_actions_cache = tmp_path / "compact-actions-CMakeCache.txt"
    compact_actions_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_COMPACT_FORCED_ACTIONS:BOOL=OFF",
            "CSPLENDOR_COMPACT_FORCED_ACTIONS:BOOL=ON",
        ),
        encoding="utf-8",
    )
    compact_actions_metadata, _ = manifest_tool._cmake_build_metadata(
        compact_actions_cache
    )
    assert (
        compact_actions_metadata["allowlisted_entries"][
            "CSPLENDOR_COMPACT_FORCED_ACTIONS"
        ]
        == "ON"
    )
    assert (
        compact_actions_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    compact_reasons_cache = tmp_path / "compact-reasons-CMakeCache.txt"
    compact_reasons_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_COMPACT_SOLVER_REASONS:BOOL=OFF",
            "CSPLENDOR_COMPACT_SOLVER_REASONS:BOOL=ON",
        ),
        encoding="utf-8",
    )
    compact_reasons_metadata, _ = manifest_tool._cmake_build_metadata(
        compact_reasons_cache
    )
    assert (
        compact_reasons_metadata["allowlisted_entries"][
            "CSPLENDOR_COMPACT_SOLVER_REASONS"
        ]
        == "ON"
    )
    assert (
        compact_reasons_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    compact_tt_cache = tmp_path / "compact-tt-CMakeCache.txt"
    compact_tt_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES:BOOL=OFF",
            "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES:BOOL=ON",
        ),
        encoding="utf-8",
    )
    compact_tt_metadata, _ = manifest_tool._cmake_build_metadata(compact_tt_cache)
    assert (
        compact_tt_metadata["allowlisted_entries"][
            "CSPLENDOR_COMPACT_SOLVER_TT_ENTRIES"
        ]
        == "ON"
    )
    assert (
        compact_tt_metadata["allowlisted_fingerprint_sha256"]
        != metadata["allowlisted_fingerprint_sha256"]
    )
    assert (
        compact_tt_metadata["benchmark_build_fingerprint_sha256"]
        == metadata["benchmark_build_fingerprint_sha256"]
    )

    for option in (
        "CSPLENDOR_CACHE_REVEAL_SCORES",
        "CSPLENDOR_REUSE_SEARCH_SCRATCH",
        "CSPLENDOR_SOLVER_NORMAL_ROLLBACK",
        "CSPLENDOR_MCTS_LEGACY_TREE_RECORDS",
        "CSPLENDOR_V3_PAYMENT_DP",
        "CSPLENDOR_VERIFY_SOLVER_ROLLBACK",
        "CSPLENDOR_VERIFY_REVEAL_SCORE_ORDER",
    ):
        option_cache = tmp_path / f"{option}-CMakeCache.txt"
        option_cache.write_text(
            cache.read_text(encoding="utf-8") + f"{option}:BOOL=ON\n",
            encoding="utf-8",
        )
        option_metadata, _ = manifest_tool._cmake_build_metadata(option_cache)
        assert option_metadata["allowlisted_entries"][option] == "ON"
        assert (
            option_metadata["allowlisted_fingerprint_sha256"]
            != metadata["allowlisted_fingerprint_sha256"]
        )
        same_build = (
            option_metadata["benchmark_build_fingerprint_sha256"]
            == metadata["benchmark_build_fingerprint_sha256"]
        )
        assert same_build == (not option.startswith("CSPLENDOR_VERIFY_"))

    verify_cache = tmp_path / "verify-CMakeCache.txt"
    verify_cache.write_text(
        cache.read_text(encoding="utf-8").replace(
            "CSPLENDOR_VERIFY_INCREMENTAL_HASH:BOOL=OFF",
            "CSPLENDOR_VERIFY_INCREMENTAL_HASH:BOOL=ON",
        ),
        encoding="utf-8",
    )
    verify_metadata, _ = manifest_tool._cmake_build_metadata(verify_cache)
    assert (
        verify_metadata["benchmark_build_fingerprint_sha256"]
        != metadata["benchmark_build_fingerprint_sha256"]
    )

    baseline = _manifest()
    candidate = _manifest("/tmp/b/bench")
    baseline["host"]["uname"]["node"] = "host-a"
    candidate["host"]["uname"]["node"] = "host-b"
    with pytest.raises(runner.BenchmarkContractError, match="uname.node"):
        runner.validate_manifest_compatibility(baseline, candidate)

    candidate["host"]["uname"]["node"] = "host-a"
    baseline["host"]["cpu"]["selected_cpu_details"] = {
        "4": {"core_id": "4", "thread_siblings_list": "4,12"}
    }
    candidate["host"]["cpu"]["selected_cpu_details"] = {
        "4": {"core_id": "5", "thread_siblings_list": "4,13"}
    }
    with pytest.raises(runner.BenchmarkContractError, match="selected_cpu_details"):
        runner.validate_manifest_compatibility(baseline, candidate)

    candidate["host"]["cpu"]["selected_cpu_details"] = deepcopy(
        baseline["host"]["cpu"]["selected_cpu_details"]
    )
    baseline["tools"]["compiler"].update(
        {"resolved_path": "/usr/bin/g++-15", "sha256": "compiler-a"}
    )
    candidate["tools"]["compiler"].update(
        {"resolved_path": "/usr/bin/g++-15", "sha256": "compiler-b"}
    )
    with pytest.raises(runner.BenchmarkContractError, match="compiler_sha256"):
        runner.validate_manifest_compatibility(baseline, candidate)
    candidate["tools"]["compiler"]["sha256"] = "compiler-a"

    baseline["build"] = {
        "available": True,
        "benchmark_build_fingerprint_sha256": "aaa",
    }
    candidate["build"] = {
        "available": True,
        "benchmark_build_fingerprint_sha256": "bbb",
    }
    with pytest.raises(runner.BenchmarkContractError, match="build"):
        runner.validate_manifest_compatibility(baseline, candidate)


def test_supplied_manifest_must_match_command_and_binary(tmp_path):
    binary = tmp_path / "bench"
    binary.write_bytes(b"benchmark")
    binary.chmod(0o755)
    manifest = {
        "command": [str(binary), "--workload", "legal_count"],
        "binary": {
            "path": str(binary.resolve()),
            "sha256": manifest_tool.sha256_file(binary),
        },
    }
    runner.validate_manifest_matches_command(
        manifest, [str(binary), "--workload", "legal_count"]
    )
    with pytest.raises(runner.BenchmarkContractError, match="command"):
        runner.validate_manifest_matches_command(
            manifest, [str(binary), "--workload", "legal_codes"]
        )
    stale = deepcopy(manifest)
    stale["binary"]["sha256"] = "0" * 64
    with pytest.raises(runner.BenchmarkContractError, match="SHA-256"):
        runner.validate_manifest_matches_command(
            stale, [str(binary), "--workload", "legal_count"]
        )


def test_rotated_slots_keep_fixed_inodes_sha_and_original_manifests(
    tmp_path, monkeypatch
):
    baseline_binary = tmp_path / "baseline-bench"
    candidate_binary = tmp_path / "candidate-bench"
    baseline_binary.write_bytes(b"\x7fELF\x02baseline-binary")
    candidate_binary.write_bytes(b"\x7fELF\x02candidate-binary")
    baseline_binary.chmod(0o700)
    candidate_binary.chmod(0o700)

    commands = {
        "A": [str(baseline_binary), "--suite", "rules"],
        "B": [str(candidate_binary), "--suite", "rules"],
    }
    manifests = {}
    for side, binary in (("A", baseline_binary), ("B", candidate_binary)):
        manifests[side] = _manifest(str(binary))
        manifests[side]["binary"] = {
            "path": str(binary.resolve()),
            "sha256": manifest_tool.sha256_file(binary),
            "size_bytes": binary.stat().st_size,
        }

    calls = []

    def fake_run(command, **_kwargs):
        slot = command[0]
        content = Path(slot).read_bytes()
        side = "A" if content == baseline_binary.read_bytes() else "B"
        metadata = Path(slot).stat()
        calls.append(
            {
                "side": side,
                "path": slot,
                "inode": metadata.st_ino,
                "device": metadata.st_dev,
                "arguments": command[1:],
                "sha256": manifest_tool.sha256_file(slot),
            }
        )
        return {
            "execution_command": list(command),
            "runner_rss_kib": 1200,
            "runner_rss_scope": "single_workload",
            "records": [_record(rate=100.0 if side == "A" else 110.0)],
        }

    monkeypatch.setattr(runner, "run_native_command", fake_run)
    result = runner.run_paired(
        commands["A"],
        commands["B"],
        pairs=4,
        warmups=0,
        cpu_set=[4],
        bootstrap_iterations=50,
        baseline_manifest=manifests["A"],
        candidate_manifest=manifests["B"],
        rotate_binary_slots=True,
    )

    assert [(call["side"], Path(call["path"]).name) for call in calls] == [
        ("A", "slot0.elf"),
        ("B", "slot1.elf"),
        ("B", "slot0.elf"),
        ("A", "slot1.elf"),
        ("A", "slot0.elf"),
        ("B", "slot1.elf"),
        ("B", "slot0.elf"),
        ("A", "slot1.elf"),
    ]
    by_name = {}
    for call in calls:
        by_name.setdefault(Path(call["path"]).name, set()).add(
            (call["device"], call["inode"])
        )
        assert call["arguments"] == ["--suite", "rules"]
    assert all(len(identities) == 1 for identities in by_name.values())
    assert all(not Path(call["path"]).exists() for call in calls)
    assert result["manifests"]["A"]["binary"]["path"] == str(baseline_binary.resolve())
    assert result["manifests"]["B"]["binary"]["path"] == str(candidate_binary.resolve())
    assert [pair["binary_slot_layout"] for pair in result["pairs"]] == [
        {"A": "slot0", "B": "slot1", "layout_id": "A-slot0_B-slot1"},
        {"A": "slot1", "B": "slot0", "layout_id": "A-slot1_B-slot0"},
        {"A": "slot0", "B": "slot1", "layout_id": "A-slot0_B-slot1"},
        {"A": "slot1", "B": "slot0", "layout_id": "A-slot1_B-slot0"},
    ]
    for pair in result["pairs"]:
        for side in ("A", "B"):
            slot = pair[side]["binary_slot"]
            assert slot["source_binary_sha256"] == slot["staged_binary_sha256"]
            assert slot["post_run_sha256"] == slot["staged_binary_sha256"]
    ratio = result["comparison"][0]["B_over_A"]
    assert ratio["median"] == pytest.approx(1.1)
    assert ratio["crossover_blocks"] == 2


def test_binary_slot_rotation_rejects_non_elf(tmp_path):
    binaries = []
    manifests = []
    for name in ("a", "b"):
        binary = tmp_path / name
        binary.write_bytes(b"not-an-elf")
        binary.chmod(0o700)
        manifest = _manifest(str(binary))
        manifest["binary"] = {
            "path": str(binary.resolve()),
            "sha256": manifest_tool.sha256_file(binary),
        }
        binaries.append(binary)
        manifests.append(manifest)
    with pytest.raises(runner.BenchmarkContractError, match="native ELF"):
        runner.run_paired(
            [str(binaries[0]), "--suite", "rules"],
            [str(binaries[1]), "--suite", "rules"],
            pairs=2,
            warmups=0,
            baseline_manifest=manifests[0],
            candidate_manifest=manifests[1],
            rotate_binary_slots=True,
        )


def test_binary_slot_rotation_snapshots_sources_and_cleans_up_on_failure(tmp_path):
    binaries = {}
    commands = {}
    manifests = {}
    original_contents = {}
    for side in ("A", "B"):
        binary = tmp_path / side.lower()
        content = b"\x7fELF\x02" + side.encode("ascii") * 16
        binary.write_bytes(content)
        binary.chmod(0o700)
        command = [str(binary), "--suite", "rules"]
        manifest = _manifest(str(binary))
        manifest["binary"] = {
            "path": str(binary.resolve()),
            "sha256": manifest_tool.sha256_file(binary),
        }
        binaries[side] = binary
        commands[side] = command
        manifests[side] = manifest
        original_contents[side] = content

    rotator = runner._FixedBinarySlotRotator(commands, manifests)
    binaries["A"].write_bytes(b"\x7fELF\x02changed-after-snapshot")
    slot_paths = []
    with pytest.raises(RuntimeError, match="simulated benchmark failure"):
        with rotator as slots:
            layout = slots.prepare(0)
            slot_paths = [Path(layout[side]["slot_path"]) for side in ("A", "B")]
            assert slot_paths[0].read_bytes() == original_contents["A"]
            assert slot_paths[1].read_bytes() == original_contents["B"]
            raise RuntimeError("simulated benchmark failure")
    assert all(not path.exists() for path in slot_paths)


def test_binary_slot_rotation_rejects_origin_dependent_elf(tmp_path):
    binaries = []
    manifests = []
    for name, payload in (("a", b"\x7fELF$ORIGIN/lib"), ("b", b"\x7fELFplain")):
        binary = tmp_path / name
        binary.write_bytes(payload)
        binary.chmod(0o700)
        manifest = _manifest(str(binary))
        manifest["binary"] = {
            "path": str(binary.resolve()),
            "sha256": manifest_tool.sha256_file(binary),
        }
        binaries.append(binary)
        manifests.append(manifest)
    with pytest.raises(runner.BenchmarkContractError, match="RPATH/RUNPATH"):
        runner.run_paired(
            [str(binaries[0]), "--suite", "rules"],
            [str(binaries[1]), "--suite", "rules"],
            pairs=2,
            warmups=0,
            baseline_manifest=manifests[0],
            candidate_manifest=manifests[1],
            rotate_binary_slots=True,
        )


@pytest.mark.skipif(not hasattr(os, "killpg"), reason="POSIX process groups required")
def test_runner_timeout_kills_descendant_process_group(tmp_path):
    child_pid_path = tmp_path / "child.pid"
    child_program = (
        "import signal,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);time.sleep(30)"
    )
    parent_program = (
        "import pathlib,signal,subprocess,sys,time;"
        f"p=subprocess.Popen([sys.executable,'-c',{child_program!r}]);"
        f"pathlib.Path({str(child_pid_path)!r}).write_text(str(p.pid));"
        "signal.signal(signal.SIGTERM,signal.SIG_IGN);"
        "time.sleep(30)"
    )
    cpu = min(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else 0
    with pytest.raises(runner.subprocess.TimeoutExpired):
        runner._run_with_rss(
            [sys.executable, "-c", parent_program],
            [cpu],
            timeout=0.3,
        )
    child_pid = int(child_pid_path.read_text(encoding="ascii"))
    process_state = Path(f"/proc/{child_pid}/stat")
    deadline = time.monotonic() + 2.0
    while process_state.exists() and time.monotonic() < deadline:
        fields = process_state.read_text(encoding="ascii").split()
        if len(fields) >= 3 and fields[2] == "Z":
            break
        time.sleep(0.02)
    if process_state.exists():
        fields = process_state.read_text(encoding="ascii").split()
        assert len(fields) >= 3 and fields[2] == "Z"


def test_atomic_json_replacement_never_exposes_partial_document(tmp_path, monkeypatch):
    destination = tmp_path / "result.json"
    manifest_tool.atomic_write_json(destination, {"generation": 1})
    assert json.loads(destination.read_text()) == {"generation": 1}

    real_replace = manifest_tool.os.replace

    def fail_replace(_source, _destination):
        raise OSError("simulated interrupted replace")

    monkeypatch.setattr(manifest_tool.os, "replace", fail_replace)
    with pytest.raises(OSError, match="interrupted"):
        manifest_tool.atomic_write_json(destination, {"generation": 2})
    assert json.loads(destination.read_text()) == {"generation": 1}
    assert list(tmp_path.glob(".result.json.*.tmp")) == []

    monkeypatch.setattr(manifest_tool.os, "replace", real_replace)
    manifest_tool.atomic_write_json(destination, {"generation": 3})
    assert json.loads(destination.read_text()) == {"generation": 3}
