from copy import deepcopy

import pytest

from scripts import validate_engine_differential as differential


def _records(seed: int):
    fixture = {
        "correct": True,
        "fixture_exact_hash": str(1000 + seed),
        "fixture_observable_hash_0": str(2000 + seed),
        "fixture_observable_hash_1": str(3000 + seed),
        "fixture_legal_count": 7,
        "fixture_ordered_legal_digest": f"legal-{seed}",
        "fixture_setup_actions": 4,
        "simple_payment_mode": bool(seed & 1),
    }
    records = []
    for workload in differential.WORKLOADS:
        records.append(
            {
                "schema": "csplendor.engine_hotpath.v1",
                "workload": workload,
                "fixture": "random",
                "seed": seed,
                "operations": 1,
                "elapsed_ns": 10,
                "rate_per_second": 100_000_000.0,
                "digest": f"digest-{seed}",
                "counters": {"actions_per_call": 7},
                "semantics": deepcopy(fixture),
            }
        )
    return records


def test_lockstep_differential_records_binary_digests(tmp_path, monkeypatch):
    baseline = tmp_path / "baseline"
    candidate = tmp_path / "candidate"
    baseline.write_bytes(b"A")
    candidate.write_bytes(b"B")

    monkeypatch.setattr(
        differential,
        "_run_seed",
        lambda _binary, seed, _plies, _timeout: _records(seed),
    )
    result = differential.validate(
        baseline,
        3,
        5,
        12,
        candidate_binary=candidate,
        timeout=1.0,
    )
    assert result["semantic_equal"] is True
    assert result["states_checked"] == 3
    assert result["semantic_record_checks"] == 6 * len(differential.WORKLOADS)
    assert result["semantic_record_failures"] == 0
    assert result["exact_hash_oracle_checks_per_binary"] == 9
    assert result["exact_hash_oracle_checks_total"] == 18
    assert result["hash_oracle_failures"] == 0
    assert result["corpus_digest"] == result["candidate_corpus_digest"]
    assert result["baseline_binary"]["sha256"] != result["candidate_binary"]["sha256"]


def test_lockstep_differential_reports_first_seed_workload_and_field(
    tmp_path, monkeypatch
):
    baseline = tmp_path / "baseline"
    candidate = tmp_path / "candidate"
    baseline.write_bytes(b"A")
    candidate.write_bytes(b"B")

    def fake_run(binary, seed, _plies, _timeout):
        records = _records(seed)
        if binary.name == "candidate":
            records[3]["semantics"]["fixture_exact_hash"] = "wrong"
        return records

    monkeypatch.setattr(differential, "_run_seed", fake_run)
    with pytest.raises(
        RuntimeError,
        match=r"seed 9, workload apply_exact_hash: semantic mismatch at "
        r"record\.semantics\.fixture_exact_hash",
    ):
        differential.validate(
            baseline,
            1,
            9,
            12,
            candidate_binary=candidate,
            timeout=1.0,
        )


def test_differential_rejects_invalid_limits(tmp_path):
    binary = tmp_path / "binary"
    binary.write_bytes(b"x")
    with pytest.raises(ValueError, match="seeds"):
        differential.validate(binary, 0, 0, 12)
    with pytest.raises(ValueError, match="fixture plies"):
        differential.validate(binary, 1, 0, -1)
    with pytest.raises(ValueError, match="timeout"):
        differential.validate(binary, 1, 0, 12, timeout=0.0)
