"""Test-suite names expose subsystem ownership instead of project history."""

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_python_test_modules_do_not_use_refactoring_phase_names():
    historical = sorted(
        path.name
        for path in (ROOT / "tests").glob("test_*.py")
        if re.match(r"test_phase\d", path.name)
    )
    assert historical == []


def test_native_test_targets_identify_their_subsystem():
    cmake = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    targets = re.findall(
        r"csplendor_add_(?:native_test|scheduler_suite)\(\s*([\w-]+)", cmake
    )
    historical = [target for target in targets if re.search(r"phase\d", target)]
    assert targets
    assert historical == []
