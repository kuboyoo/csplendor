import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def test_module_registration_order_is_explicit():
    source = (ROOT / "src" / "bindings.cpp").read_text(encoding="utf-8")
    calls = [
        "bind_domain(module);",
        "bind_rules(module);",
        "bind_encoding(module);",
        "bind_mcts(module);",
        "bind_solvers(module);",
    ]

    positions = [source.index(call) for call in calls]
    assert positions == sorted(positions)
    assert source.count("PYBIND11_MODULE") == 1
    assert len(source.splitlines()) <= 20

    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    for section in ("domain", "rules", "encoding", "mcts", "solvers"):
        assert f"src/bindings_{section}.cpp" in cmake


def test_fresh_process_registers_cross_section_types_and_functions():
    code = """
import csplendor._csplendor as native

game = native.Game(42)
assert isinstance(game.board, native.Board)
assert isinstance(game.legal_actions[0], native.Action)
assert len(native.StateEncoder.encode(game)) == 196
config = native.MCTSConfig()
assert native.MCTS(config).tree_size() == 0
result = native.solve_visible_only_winner_cpp(game, 1, 0.0)
assert set(result) >= {"winner", "stats", "line"}
"""
    subprocess.run(
        [sys.executable, "-c", code], cwd=ROOT, check=True, timeout=30
    )
