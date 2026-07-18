from importlib import metadata
from pathlib import Path

import csplendor

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def test_runtime_version_matches_distribution_metadata():
    try:
        installed_version = metadata.version("csplendor")
    except metadata.PackageNotFoundError:
        assert csplendor.__version__ == "0+unknown"
    else:
        assert csplendor.__version__ == installed_version


def test_sdist_manifest_contains_native_build_inputs():
    manifest = (PROJECT_ROOT / "MANIFEST.in").read_text(encoding="utf-8")

    assert "include CMakeLists.txt" in manifest
    assert "recursive-include src *.cpp *.h" in manifest
    assert (PROJECT_ROOT / "src" / "bindings.cpp").is_file()
