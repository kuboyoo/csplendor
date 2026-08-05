"""Machine-checkable API and state-profile contracts for refactoring."""

import json
from pathlib import Path

import csplendor
from csplendor import _csplendor as native

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "doc" / "refactoring_contracts.json"


def _contract():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def test_python_api_stability_classes_are_disjoint_and_complete():
    api = _contract()["python_api"]
    public = set(api["public"])
    experimental = set(api["experimental"])
    internal = set(api["internal_native"])

    assert public.isdisjoint(experimental)
    assert public.isdisjoint(internal)
    assert experimental.isdisjoint(internal)
    assert public | experimental == set(csplendor.__all__)
    assert all(hasattr(csplendor, name) for name in public | experimental)
    assert all(hasattr(native, name) for name in internal)
    assert internal.isdisjoint(csplendor.__all__)


def test_cpp_header_stability_classes_cover_every_product_header():
    headers = _contract()["cpp_headers"]
    classes = [set(headers[name]) for name in ("public", "experimental", "internal")]
    assert classes[0].isdisjoint(classes[1])
    assert classes[0].isdisjoint(classes[2])
    assert classes[1].isdisjoint(classes[2])

    classified = set().union(*classes)
    actual = {path.name for path in (ROOT / "src").glob("*.h")}
    assert classified == actual


def test_all_state_profiles_are_explicitly_versioned():
    contract = _contract()
    assert contract["schema_version"] == 1
    assert contract["state_profiles"] == [
        "reachable",
        "editor",
        "search",
        "serialized",
    ]
