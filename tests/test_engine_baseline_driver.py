"""Contracts for the reproducible engine baseline driver."""

import json

import pytest

from scripts import run_phase0_baseline as baseline


def test_phase0_cases_are_complete_unique_and_safe_at_tree_capacity(tmp_path):
    assert baseline.DEFAULT_PHASE0_PAIRS == 22
    names = [case.workload for case in baseline.CASES]
    assert len(names) == 23
    assert len(set(names)) == len(names)
    scheduler = next(
        case for case in baseline.CASES if case.workload == "parallel_scheduler"
    )
    assert scheduler.iterations == 4096
    assert scheduler.extra[scheduler.extra.index("--threads") + 1] == "1"

    command = baseline.case_command(tmp_path / "bench", baseline.CASES[0])
    assert command[1:5] == [
        "--workload",
        "legal_count",
        "--fixture",
        "midgame_250",
    ]


def test_phase0_csv_is_atomically_complete(tmp_path):
    output = tmp_path / "baseline.csv"
    baseline._atomic_write_csv(
        output,
        [
            {
                "workload": "legal_count",
                "fixture": "midgame_250",
                "operations": 1,
                "A_median_rate_per_second": 10.0,
                "B_median_rate_per_second": 11.0,
                "B_over_A_median": 1.1,
                "ci95_low": 1.0,
                "ci95_high": 1.2,
                "A_p95_rate_per_second": 10.5,
                "B_p95_rate_per_second": 11.5,
                "A_median_runner_rss_kib": 1000,
                "B_median_runner_rss_kib": 1001,
                "digest": "abc",
            }
        ],
    )
    text = output.read_text(encoding="utf-8")
    assert text.count("\n") == 2
    assert "legal_count,midgame_250" in text
    assert list(tmp_path.glob(".baseline.csv.*.tmp")) == []


def test_phase0_payload_schema_is_json_serializable():
    payload = {
        "schema": baseline.SCHEMA,
        "cases": [case.__dict__ for case in baseline.CASES],
    }
    assert json.loads(json.dumps(payload))["schema"] == baseline.SCHEMA


def test_phase0_enables_slot_crossover_and_22_pairs_by_default(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(baseline, "CASES", (baseline.CASES[0],))

    def fake_run(*_args, **kwargs):
        calls.append(kwargs)
        return {"comparison": []}

    monkeypatch.setattr(baseline, "run_paired", fake_run)
    monkeypatch.setattr(
        baseline,
        "_csv_row",
        lambda _result: {
            "workload": "legal_count",
            "fixture": "midgame_250",
            "operations": 1,
            "A_median_rate_per_second": 10.0,
            "B_median_rate_per_second": 11.0,
            "B_over_A_median": 1.1,
            "ci95_low": 1.0,
            "ci95_high": 1.2,
            "A_p95_rate_per_second": 10.5,
            "B_p95_rate_per_second": 11.5,
            "A_median_runner_rss_kib": 1000,
            "B_median_runner_rss_kib": 1001,
            "digest": "abc",
        },
    )
    output_json = tmp_path / "baseline.json"
    output_csv = tmp_path / "baseline.csv"
    assert (
        baseline.main(
            [
                "--baseline-binary",
                str(tmp_path / "a"),
                "--candidate-binary",
                str(tmp_path / "b"),
                "--baseline-repo-root",
                str(tmp_path),
                "--candidate-repo-root",
                str(tmp_path),
                "--output-json",
                str(output_json),
                "--output-csv",
                str(output_csv),
            ]
        )
        == 0
    )
    assert calls[0]["pairs"] == 22
    assert calls[0]["rotate_binary_slots"] is True
    payload = json.loads(output_json.read_text(encoding="utf-8"))
    assert payload["settings"]["rotate_binary_slots"] is True
    assert payload["settings"]["statistical_unit"] == "two_pair_crossover_block"
    assert (
        payload["settings"]["binary_slot_policy"]
        == "two_private_fixed_inodes_crossed_every_pair"
    )


def test_phase0_rejects_odd_pairs_when_slot_crossover_is_enabled(tmp_path):
    with pytest.raises(SystemExit):
        baseline.main(
            [
                "--baseline-binary",
                str(tmp_path / "a"),
                "--candidate-binary",
                str(tmp_path / "b"),
                "--baseline-repo-root",
                str(tmp_path),
                "--candidate-repo-root",
                str(tmp_path),
                "--output-json",
                str(tmp_path / "baseline.json"),
                "--output-csv",
                str(tmp_path / "baseline.csv"),
                "--pairs",
                "21",
            ]
        )
