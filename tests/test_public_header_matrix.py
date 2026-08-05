"""Public C++ header standalone-build matrix contracts."""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_cmake_standalone_header_matrix_matches_versioned_public_contract():
    manifest = json.loads(
        (ROOT / "doc" / "refactoring_contracts.json").read_text(
            encoding="utf-8"
        )
    )
    expected = set(manifest["cpp_headers"]["public"])
    cmake = (ROOT / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(
        r"set\(CSPLENDOR_STANDALONE_HEADERS\s+(.*?)\)",
        cmake,
        flags=re.DOTALL,
    )
    assert match is not None
    configured = set(re.findall(r"\b[\w_]+\.h\b", match.group(1)))
    assert configured == expected
