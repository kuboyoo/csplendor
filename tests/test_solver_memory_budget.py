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


def test_budget_is_split_across_jobs():
    assert cs.solver_memory_budget_bytes(available_bytes=4 * GiB, jobs=4) == GiB // 4
    assert cs.solver_memory_budget_bytes(available_bytes=0, jobs=2) == FALLBACK // 2
    assert cs.resolve_max_memo_states(memory_budget_bytes=BYTES_PER_STATE * 1000, jobs=2) == 500


def test_explicit_cap_wins():
    assert cs.resolve_max_memo_states(12_345) == 12_345
    assert cs.resolve_max_memo_states(12_345, memory_budget_bytes=BYTES_PER_STATE, jobs=8) == 12_345


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="reads /proc/meminfo")
def test_free_memory_is_detected_on_linux():
    assert cs.available_memory_bytes() > 0
    detected = cs.resolve_max_memo_states()
    assert 64 * MiB // BYTES_PER_STATE <= detected <= 2 * GiB // BYTES_PER_STATE


def test_depth_sweep_resolves_the_cap_once_per_call_with_its_jobs(monkeypatch):
    calls = []

    def fake_resolve(max_memo_states=0, memory_budget_bytes=0, jobs=1):
        calls.append((max_memo_states, jobs))
        return max_memo_states or 50

    monkeypatch.setattr(mate_depth, "resolve_max_memo_states", fake_resolve)
    game = load_game_from_usi_text(BENCH_POSITION)
    options = dict(attacker=0, min_depth=1, max_depth=3, time_limit_seconds=60.0)
    result = cs.search_reveal_verified_mate_depths(game, jobs=2, **options)
    assert calls == [(0, 2)]
    assert result["stats"]["memo_flushes"] > 0

    calls.clear()
    explicit = cs.search_reveal_verified_mate_depths(game, max_memo_states=5_000_000, **options)
    assert calls == [(5_000_000, 1)]
    assert explicit["stats"]["memo_flushes"] == 0
    for searched in (result, explicit):
        assert searched["status"] == "no_mate_within_max_depth"
        assert searched["verified_no_mate_through_depth"] == 3
