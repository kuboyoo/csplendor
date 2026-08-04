from importlib import metadata
from pathlib import Path

import pytest

import csplendor
from _build_support import (
    cpu_target_from_environment,
    macos_architectures,
    macos_deployment_target,
    validate_macos_native_build,
    validate_macos_wheel_architectures,
    validate_wheel_build,
)

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
    assert "include _build_support.py" in manifest
    assert "recursive-include src *.cpp *.h" in manifest
    assert (PROJECT_ROOT / "src" / "bindings.cpp").is_file()


def test_cpu_target_defaults_to_portable_and_normalizes_explicit_values():
    assert cpu_target_from_environment({}) == "portable"
    assert cpu_target_from_environment({"CSPLENDOR_CPU_TARGET": " PORTABLE "}) == (
        "portable"
    )
    assert cpu_target_from_environment({"CSPLENDOR_CPU_TARGET": "NATIVE"}) == "native"


def test_cpu_target_rejects_unknown_or_empty_values():
    for value in ("", "m4", "generic"):
        with pytest.raises(ValueError, match="CSPLENDOR_CPU_TARGET"):
            cpu_target_from_environment({"CSPLENDOR_CPU_TARGET": value})


def test_wheels_require_a_fresh_portable_build():
    validate_wheel_build("portable", skip_build=False)
    with pytest.raises(ValueError, match="local-only"):
        validate_wheel_build("native", skip_build=False)
    with pytest.raises(ValueError, match="skip-build"):
        validate_wheel_build("portable", skip_build=True)


def test_macos_architectures_follow_explicit_and_python_build_requests():
    assert macos_architectures(
        {"CSPLENDOR_OSX_ARCHITECTURES": "arm64"},
        python_platform="macosx-15.0-universal2",
        machine="x86_64",
    ) == ("arm64",)
    assert macos_architectures(
        {"ARCHFLAGS": "-arch x86_64 -arch arm64"},
        python_platform="macosx-15.0-arm64",
        machine="arm64",
    ) == ("arm64", "x86_64")
    assert macos_architectures(
        {}, python_platform="macosx-15.0-universal2", machine="arm64"
    ) == ("arm64", "x86_64")


def test_macos_architectures_reject_invalid_and_conflicting_requests():
    with pytest.raises(ValueError, match="must be one of"):
        macos_architectures({"CSPLENDOR_OSX_ARCHITECTURES": "m4"})
    with pytest.raises(ValueError, match="incomplete"):
        macos_architectures({"ARCHFLAGS": "-arch"})
    with pytest.raises(ValueError, match="conflicting"):
        macos_architectures(
            {
                "CSPLENDOR_OSX_ARCHITECTURES": "arm64",
                "ARCHFLAGS": "-arch x86_64",
            }
        )


def test_macos_native_build_accepts_arm64_process_with_arm64_extension():
    # A universal2 Python can run natively as arm64 and load an arm64-only
    # local extension. Its distributable wheel tag is validated separately.
    validate_macos_native_build("native", ("arm64",), "arm64")
    validate_macos_native_build("portable", ("arm64", "x86_64"), "x86_64")


@pytest.mark.parametrize(
    ("architectures", "machine"),
    [
        (("arm64", "x86_64"), "arm64"),
        (("x86_64",), "arm64"),
        (("arm64",), "x86_64"),
    ],
)
def test_macos_native_build_rejects_non_arm64_only_execution(
    architectures, machine
):
    with pytest.raises(ValueError, match="arm64 process"):
        validate_macos_native_build("native", architectures, machine)


def test_macos_wheel_architectures_must_match_platform_tag():
    validate_macos_wheel_architectures(("arm64",), "macosx_11_0_arm64")
    validate_macos_wheel_architectures(("arm64", "x86_64"), "macosx_11_0_universal2")
    with pytest.raises(ValueError, match="does not match"):
        validate_macos_wheel_architectures(("x86_64",), "macosx_15_0_arm64")
    with pytest.raises(ValueError, match="does not identify"):
        validate_macos_wheel_architectures(("arm64",), "macosx_15_0_m4")


def test_macos_deployment_target_prefers_explicit_environment_value():
    assert (
        macos_deployment_target(
            {"MACOSX_DEPLOYMENT_TARGET": "11.0"}, python_target="15"
        )
        == "11.0"
    )
    assert macos_deployment_target({}, python_target="15") == "15"
    assert macos_deployment_target({}, python_target=None) is None


def test_macos_deployment_target_rejects_invalid_values():
    for value in ("", "latest", "11.x", "11.0;invalid"):
        with pytest.raises(ValueError, match="MACOSX_DEPLOYMENT_TARGET"):
            macos_deployment_target(
                {"MACOSX_DEPLOYMENT_TARGET": value}, python_target="15"
            )
