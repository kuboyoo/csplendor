"""Domain delta-undo correctness and cost contracts."""

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]


def _compiler():
    compiler = os.environ.get("CXX", "c++")
    if not shutil.which(compiler):
        pytest.skip(f"C++ compiler is unavailable: {compiler}")
    return compiler


def test_undo_record_matches_snapshot_for_all_transitions_and_is_smaller():
    compiler = _compiler()
    with tempfile.TemporaryDirectory(prefix="csplendor-undo-") as directory:
        binary = Path(directory) / "undo_record_probe"
        subprocess.run(
            [
                compiler,
                "-std=c++17",
                "-O3",
                "-DCSPLENDOR_VERIFY_DELTA_UNDO",
                f"-I{ROOT / 'src'}",
                str(ROOT / "tests" / "undo_record_probe.cpp"),
                "-o",
                str(binary),
            ],
            check=True,
            cwd=ROOT,
        )
        result = json.loads(subprocess.check_output([binary], text=True))

    assert result["type_mask"] == (1 << 6) - 1
    assert result["transitions"] >= 500
    assert result["multi_steps"] >= 10
    assert result["all_equal"] == 1
    assert result["failed_rejected"] == 1
    assert result["failed_equal"] == 1
    assert result["trusted_rejected"] == 1
    assert result["failed_trusted_equal"] == 1
    assert result["hash_invalid_equal"] == 1
    assert result["multi_equal"] == 1
    assert result["debug_dual_run"] == 1
    assert result["editor_guard"] == 1
    assert result["invalid_player_guard"] == 1
    assert result["sizeof_record"] < result["sizeof_board"] // 2

    # Allocation counts are deterministic for this fixed provenance fixture.
    # Wall-clock values are emitted for profiling, but intentionally not used
    # as a flaky pass/fail threshold.
    assert result["delta_allocations"] < result["snapshot_allocations"] // 100
