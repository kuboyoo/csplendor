import importlib.util
from pathlib import Path


def _load_runner():
    path = Path(__file__).parents[1] / "scripts" / "refactor_build_baseline.py"
    spec = importlib.util.spec_from_file_location("refactor_build_baseline", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def test_include_graph_counts_direct_and_transitive_dependencies(tmp_path):
    runner = _load_runner()
    (tmp_path / "leaf.h").write_text("#pragma once\n", encoding="utf-8")
    (tmp_path / "middle.h").write_text(
        '#pragma once\n#include "leaf.h"\n', encoding="utf-8"
    )
    (tmp_path / "module.cpp").write_text(
        '#include "middle.h"\n#include <vector>\n', encoding="utf-8"
    )

    graph = runner.collect_include_graph(tmp_path)

    assert graph["node_count"] == 3
    assert graph["translation_unit_count"] == 1
    assert graph["direct_edge_count"] == 2
    assert graph["transitive_dependency_count"]["module.cpp"] == 2
    assert graph["max_translation_unit_transitive_dependency_count"] == 2
    assert graph["unresolved_local_includes"] == []


def test_build_comparison_reports_environment_and_five_percent_regression():
    runner = _load_runner()
    environment = {
        "platform": "test",
        "machine": "test",
        "python": "3.12",
        "cmake": "cmake",
        "compiler": "c++",
        "cpu_target": "portable",
        "parallel": 2,
    }

    def payload(clean, incremental, size):
        return {
            "environment": environment,
            "include_graph": {
                "direct_edge_count": 4,
                "max_translation_unit_transitive_dependency_count": 3,
            },
            "build": {
                "clean": {
                    "seconds": clean,
                    "max_child_rss_kib_so_far": 1_000,
                },
                "noop_incremental": {"seconds": incremental},
                "extension_size_bytes": size,
            },
        }

    comparison = runner.compare_payloads(payload(10, 1, 100), payload(10.6, 1, 104))

    assert comparison["environment_mismatches"] == {}
    assert comparison["metrics"]["clean_build_seconds"]["over_five_percent"]
    assert not comparison["metrics"]["extension_size_bytes"]["over_five_percent"]
