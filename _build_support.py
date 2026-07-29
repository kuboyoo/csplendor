import os
import re
import shlex

CPU_TARGET_ENV = "CSPLENDOR_CPU_TARGET"
SUPPORTED_CPU_TARGETS = ("portable", "native")
OSX_ARCHITECTURES_ENV = "CSPLENDOR_OSX_ARCHITECTURES"
_SUPPORTED_OSX_ARCHITECTURES = {
    "arm64": ("arm64",),
    "x86_64": ("x86_64",),
    "universal2": ("arm64", "x86_64"),
}
_MACOS_VERSION_PATTERN = re.compile(r"^[0-9]+(?:\.[0-9]+){0,2}$")


def cpu_target_from_environment(environment=None):
    if environment is None:
        environment = os.environ

    value = environment.get(CPU_TARGET_ENV, "portable").strip().lower()
    if value not in SUPPORTED_CPU_TARGETS:
        supported = ", ".join(SUPPORTED_CPU_TARGETS)
        raise ValueError(f"{CPU_TARGET_ENV} must be one of: {supported}; got {value!r}")
    return value


def validate_wheel_build(cpu_target, skip_build):
    if cpu_target == "native":
        raise ValueError(
            "CSPLENDOR_CPU_TARGET=native is local-only and cannot be used "
            "to build a wheel; use an editable install or direct build_ext"
        )
    if skip_build:
        raise ValueError(
            "bdist_wheel --skip-build is not supported because wheel builds "
            "must rebuild the selected portable CPU profile"
        )


def _normalize_osx_architectures(values, source):
    unique = set(values)
    unknown = unique.difference(("arm64", "x86_64"))
    if unknown:
        rendered = ", ".join(sorted(unknown))
        raise ValueError(f"{source} contains unsupported architecture(s): {rendered}")
    if unique == {"arm64", "x86_64"}:
        return ("arm64", "x86_64")
    if unique == {"arm64"}:
        return ("arm64",)
    if unique == {"x86_64"}:
        return ("x86_64",)
    raise ValueError(f"{source} does not select a macOS architecture")


def _architectures_from_archflags(value):
    try:
        tokens = shlex.split(value)
    except ValueError as error:
        raise ValueError(f"ARCHFLAGS is invalid: {error}") from error

    architectures = []
    index = 0
    while index < len(tokens):
        if tokens[index] != "-arch":
            index += 1
            continue
        if index + 1 >= len(tokens):
            raise ValueError("ARCHFLAGS ends with an incomplete -arch option")
        architectures.append(tokens[index + 1])
        index += 2
    if not architectures:
        return None
    return _normalize_osx_architectures(architectures, "ARCHFLAGS")


def _architectures_from_platform(value):
    if not value:
        return None
    normalized = str(value).lower()
    if normalized.endswith("universal2"):
        return ("arm64", "x86_64")
    if normalized.endswith("arm64"):
        return ("arm64",)
    if normalized.endswith("x86_64"):
        return ("x86_64",)
    return None


def macos_architectures(environment=None, python_platform=None, machine=None):
    if environment is None:
        environment = os.environ

    explicit = []
    requested = environment.get(OSX_ARCHITECTURES_ENV)
    if requested is not None:
        normalized = requested.strip().lower()
        if normalized not in _SUPPORTED_OSX_ARCHITECTURES:
            supported = ", ".join(_SUPPORTED_OSX_ARCHITECTURES)
            raise ValueError(
                f"{OSX_ARCHITECTURES_ENV} must be one of: {supported}; "
                f"got {normalized!r}"
            )
        explicit.append(
            (OSX_ARCHITECTURES_ENV, _SUPPORTED_OSX_ARCHITECTURES[normalized])
        )

    archflags = environment.get("ARCHFLAGS")
    if archflags is not None:
        parsed = _architectures_from_archflags(archflags)
        if parsed is not None:
            explicit.append(("ARCHFLAGS", parsed))

    host_platform = environment.get("_PYTHON_HOST_PLATFORM")
    if host_platform is not None:
        parsed = _architectures_from_platform(host_platform)
        if parsed is None:
            raise ValueError(
                "_PYTHON_HOST_PLATFORM does not identify arm64, x86_64, or universal2"
            )
        explicit.append(("_PYTHON_HOST_PLATFORM", parsed))

    if explicit:
        selected = explicit[0][1]
        conflicts = [name for name, value in explicit[1:] if value != selected]
        if conflicts:
            sources = ", ".join([explicit[0][0], *conflicts])
            raise ValueError(f"conflicting macOS architecture requests from: {sources}")
        return selected

    selected = _architectures_from_platform(python_platform)
    if selected is not None:
        return selected
    return _architectures_from_platform(machine)


def validate_macos_wheel_architectures(architectures, wheel_platform):
    tagged_architectures = _architectures_from_platform(wheel_platform)
    if tagged_architectures is None:
        raise ValueError(
            "the macOS wheel platform tag does not identify arm64, x86_64, "
            f"or universal2: {wheel_platform!r}"
        )
    if architectures != tagged_architectures:
        requested = ", ".join(architectures)
        tagged = ", ".join(tagged_architectures)
        raise ValueError(
            "the requested macOS architecture does not match the wheel "
            f"platform tag: requested {requested}; tag {wheel_platform!r} "
            f"identifies {tagged}. Use a matching build Python or "
            "_PYTHON_HOST_PLATFORM."
        )


def macos_deployment_target(environment=None, python_target=None):
    if environment is None:
        environment = os.environ

    value = environment.get("MACOSX_DEPLOYMENT_TARGET")
    if value is None:
        value = python_target
    if value is None:
        return None

    normalized = str(value).strip()
    if not _MACOS_VERSION_PATTERN.fullmatch(normalized):
        raise ValueError(
            "MACOSX_DEPLOYMENT_TARGET must be a numeric macOS version; "
            f"got {normalized!r}"
        )
    return normalized
