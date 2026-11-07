"""Memory budget of the mate searches (doc/mate_usage.md「探索のメモリ予算」)."""

from __future__ import annotations

import sys

import pytest

import csplendor as cs
from csplendor import mate_depth
from scripts.mate_solver import load_game_from_usi_text
from tests.test_reveal_verified_solver import BENCH_POSITION

MiB = 1 << 20
GiB = 1 << 30
FALLBACK = 512_000_000
BYTES_PER_STATE = 256


def test_budget_is_a_quarter_of_the_injected_free_memory():
    assert cs.solver_memory_budget_bytes(available_bytes=4 * GiB) == GiB
    assert cs.solver_memory_budget_bytes(available_bytes=1 * GiB) == 256 * MiB


def test_budget_is_clamped():
    assert cs.solver_memory_budget_bytes(available_bytes=100 * MiB) == 64 * MiB
    assert cs.solver_memory_budget_bytes(available_bytes=100 * GiB) == 2 * GiB
    # Unknown free memory: the former fixed default (2,000,000 states).
    assert cs.solver_memory_budget_bytes(available_bytes=0) == FALLBACK
    assert cs.resolve_max_memo_states(memory_budget_bytes=FALLBACK) == 2_000_000


def test_explicit_cap_wins():
    assert cs.resolve_max_memo_states(12_345) == 12_345
    assert cs.resolve_max_memo_states(12_345, memory_budget_bytes=BYTES_PER_STATE) == 12_345


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="reads /proc/meminfo")
def test_free_memory_is_detected_on_linux():
    assert cs.available_memory_bytes() > 0
    detected = cs.resolve_max_memo_states()
    assert 64 * MiB // BYTES_PER_STATE <= detected <= 2 * GiB // BYTES_PER_STATE


def test_dfpn_takes_a_quarter_of_the_budget_and_the_cap_the_rest():
    budget = BYTES_PER_STATE * 4000
    assert cs.resolve_search_table_caps(memory_budget_bytes=budget) == (4000, 0)
    assert cs.resolve_search_table_caps(
        memory_budget_bytes=budget, algorithm="dfpn") == (3000, budget // 4)
    # Explicit values are kept; the other one still takes its share.
    assert cs.resolve_search_table_caps(
        12_345, memory_budget_bytes=budget, algorithm="dfpn") == (12_345, budget // 4)
    assert cs.resolve_search_table_caps(
        0, 1 << 20, algorithm="dfpn", memory_budget_bytes=budget) == (3000, 1 << 20)
    # The depth-first search has no df-pn table.
    assert cs.resolve_search_table_caps(0, 1 << 20, memory_budget_bytes=budget) == (4000, 0)
    with pytest.raises(ValueError, match="algorithm"):
        cs.resolve_search_table_caps(algorithm="bfs")


def test_dfpn_search_sizes_its_table_from_the_budget():
    game = load_game_from_usi_text(BENCH_POSITION)
    _, dfpn_bytes = cs.resolve_search_table_caps(algorithm="dfpn")
    result = cs.solve_reveal_verified_mate_cpp(
        game, attacker=0, depth=3, exhaustive_attacker_actions=True, algorithm="dfpn")
    assert result["proven"] is False and result["unknown_reason"] is None
    assert 0 < result["stats"]["dfpn_table_bytes"] <= dfpn_bytes


def test_depth_sweep_resolves_the_cap_once_per_call(monkeypatch):
    calls = []

    def fake_resolve(max_memo_states=0, dfpn_table_bytes=0, algorithm="dfs",
                     memory_budget_bytes=0):
        calls.append((max_memo_states, dfpn_table_bytes, algorithm))
        return max_memo_states or 50, dfpn_table_bytes or (1024 if algorithm == "dfpn" else 0)

    monkeypatch.setattr(mate_depth, "resolve_search_table_caps", fake_resolve)
    game = load_game_from_usi_text(BENCH_POSITION)
    options = dict(attacker=0, min_depth=1, max_depth=3, time_limit_seconds=60.0)
    result = cs.search_reveal_verified_mate_depths(game, jobs=2, **options)
    assert calls == [(0, 0, "dfs")]  # one shared table: never split by jobs
    # The parallel depth 3 flushes its shared table at the cap too (only the
    # root's and level 1's decided entries are kept beyond it).
    assert result["attempts"][-1]["jobs"] == 2
    assert result["attempts"][-1]["stats"]["memo_flushes"] > 0

    calls.clear()
    serial = cs.search_reveal_verified_mate_depths(game, **options)
    assert serial["stats"]["memo_flushes"] > 0  # the serial search flushes at the cap
    calls.clear()
    explicit = cs.search_reveal_verified_mate_depths(game, max_memo_states=5_000_000, **options)
    assert calls == [(5_000_000, 0, "dfs")]
    assert explicit["stats"]["memo_flushes"] == 0
    for searched in (result, serial, explicit):
        assert searched["status"] == "no_mate_within_max_depth"
        assert searched["verified_no_mate_through_depth"] == 3



@pytest.mark.parametrize("jobs", [1, 2])
def test_depth_sweep_passes_the_dfpn_table_bytes_to_every_depth(monkeypatch, jobs):
    calls = []
    native = mate_depth.solve_reveal_verified_mate_cpp

    def fake_resolve(max_memo_states=0, dfpn_table_bytes=0, algorithm="dfs",
                     memory_budget_bytes=0):
        calls.append(algorithm)
        return 5_000_000, 1024

    def spy(game, **kwargs):
        searches.append((kwargs["algorithm"], kwargs["dfpn_table_bytes"]))
        return native(game, **kwargs)

    searches = []
    monkeypatch.setattr(mate_depth, "resolve_search_table_caps", fake_resolve)
    monkeypatch.setattr(mate_depth, "solve_reveal_verified_mate_cpp", spy)
    game = load_game_from_usi_text(BENCH_POSITION)
    result = cs.search_reveal_verified_mate_depths(
        game, attacker=0, min_depth=1, max_depth=3, time_limit_seconds=60.0,
        jobs=jobs, algorithm="dfpn")
    assert calls == ["dfpn"]
    assert result["verified_no_mate_through_depth"] == 3
    # One native search per depth (df-pn runs on one thread whatever jobs is).
    assert searches == [("dfpn", 1024)] * 3
