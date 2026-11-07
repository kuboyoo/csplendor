"""Parallel mate search (doc/mate_usage.md「並列の詰み探索」)."""

import os
import random
import subprocess
import sys
import threading
import time

import pytest

import csplendor as cs
from scripts.mate_solver import load_game_from_usi_text
from tests.test_reveal_verified_solver import BENCH_POSITION


def _endgame(seed):
    rng = random.Random(seed)
    game = cs.Game(seed=seed)
    target = rng.choice((7, 8, 9, 10, 11, 12, 13))
    while max(int(p.points) for p in game.board.players) < target:
        actions = game.legal_actions
        if not actions:
            return None
        game.apply(actions[rng.randrange(len(actions))], False)
    return game if game.legal_actions else None


def _solve(game, attacker, depth, **kwargs):
    kwargs.setdefault("exhaustive_attacker_actions", True)
    kwargs.setdefault("exact_reveal_search", True)
    kwargs.setdefault("max_memo_states", 200_000)
    return cs.solve_reveal_verified_mate_cpp(game, attacker=attacker, depth=depth, **kwargs)


def _verdict(raw):
    if raw["unknown_reason"] is not None:
        return "unknown"
    return "mate" if raw["proven"] else "no_mate"


def _without_elapsed(raw):
    payload = dict(raw)
    payload["stats"] = {k: v for k, v in raw["stats"].items() if k != "elapsed_ms"}
    return payload


def test_default_jobs_follow_the_physical_cores():
    script = "import csplendor as cs; print(cs.default_mate_jobs(), cs.physical_core_count())"
    env = {k: v for k, v in os.environ.items() if k != "CSPLENDOR_MATE_JOBS"}
    out = subprocess.run([sys.executable, "-c", script], env=env, check=True,
                         capture_output=True, text=True).stdout.split()
    jobs, cores = int(out[0]), int(out[1])
    assert cores >= 1
    spare = 0 if sys.platform == "darwin" else 2
    assert jobs == min(max(cores - spare, 1), 8)
    env["CSPLENDOR_MATE_JOBS"] = "3"
    out = subprocess.run([sys.executable, "-c", script], env=env, check=True,
                         capture_output=True, text=True).stdout.split()
    assert int(out[0]) == 3


def test_one_job_is_the_serial_search():
    game = _endgame(13)
    for exact in (True, False):
        serial = _solve(game, 1, 3, exact_reveal_search=exact)
        one = _solve(game, 1, 3, exact_reveal_search=exact, jobs=1)
        assert one["jobs"] == 1
        assert _without_elapsed(one) == _without_elapsed(serial)


@pytest.mark.parametrize("jobs", [2, 4, 8])
def test_parallel_verdicts_match_the_serial_search(jobs):
    compared = 0
    for seed in list(range(0, 30)) + [2067, 3023, 3038, 3073]:
        game = _endgame(seed)
        if game is None:
            continue
        for attacker in (0, 1):
            for exact in (True, False):
                serial = _solve(game, attacker, 3, exact_reveal_search=exact)
                parallel = _solve(game, attacker, 3, exact_reveal_search=exact, jobs=jobs)
                assert parallel["jobs"] == jobs
                assert _verdict(parallel) == _verdict(serial), (seed, attacker, exact)
                if parallel["proven"] and parallel["line"]:
                    first = int(parallel["line"][0]["action_code"])
                    recheck = _solve(game, attacker, 3, exact_reveal_search=exact,
                                     required_root_action=first)
                    assert recheck["proven"], (seed, attacker, exact)
                compared += 1
    assert compared >= 100


def test_bench_verdict_and_line():
    game = load_game_from_usi_text(BENCH_POSITION)
    attacker = int(game.current_player)
    serial = _solve(game, attacker, 3, max_memo_states=0)
    parallel = _solve(game, attacker, 3, max_memo_states=0, jobs=4)
    assert _verdict(parallel) == _verdict(serial) == "no_mate"
    assert parallel["line"]  # the refutation is followed from the root


def test_parallel_proof_dag_is_rebuilt_and_validated():
    # The DAG of a parallel proof is built by the serial search, which
    # re-proves the position and validates the DAG.
    game = _endgame(18)
    serial = _solve(game, 0, 3, include_proof_dag=True)
    raw = _solve(game, 0, 3, include_proof_dag=True, jobs=4)
    assert raw["proven"] and raw["jobs"] == 4
    dag = raw["proof_dag"]
    assert dag["complete"] and dag["validated"]
    assert len(dag["nodes"]) == len(serial["proof_dag"]["nodes"])


def test_parallel_limits_are_unknown():
    game = load_game_from_usi_text(BENCH_POSITION)
    attacker = int(game.current_player)
    started = time.perf_counter()
    timed = _solve(game, attacker, 5, time_limit_seconds=0.05, max_memo_states=0, jobs=4)
    assert timed["unknown_reason"] == "time limit exceeded"
    assert time.perf_counter() - started < 2.0
    limited = _solve(game, attacker, 4, max_nodes=5000, max_memo_states=0, jobs=4)
    assert limited["unknown_reason"] == "node limit exceeded"
    # Each thread takes the budget 64 nodes at a time.
    assert limited["stats"]["nodes"] <= 5000 + 5 * 64


def test_parallel_cancellation_from_another_thread():
    game = load_game_from_usi_text(BENCH_POSITION)
    attacker = int(game.current_player)
    token = cs.MateSearchCancellationToken()
    timer = threading.Timer(0.05, token.request_cancel)
    timer.start()
    try:
        raw = _solve(game, attacker, 5, cancellation_token=token, max_memo_states=0, jobs=4)
    finally:
        timer.cancel()
    assert raw["unknown_reason"] == "search cancelled"
    assert not raw["proven"]


def test_shallow_searches_and_dfpn_run_on_one_thread():
    game = _endgame(13)
    assert _solve(game, 1, 2, jobs=4)["jobs"] == 1
    assert _solve(game, 1, 3, jobs=4, algorithm="dfpn")["jobs"] == 1


@pytest.mark.skipif(not sys.platform.startswith("linux"), reason="/proc/self/task")
def test_no_thread_outlives_the_search():
    def threads():
        return len(os.listdir("/proc/self/task"))

    game = load_game_from_usi_text(BENCH_POSITION)
    before = threads()
    _solve(game, int(game.current_player), 3, jobs=8, max_memo_states=0)
    assert threads() == before


def test_depth_sweeps_pass_jobs_to_the_native_search():
    game = load_game_from_usi_text(BENCH_POSITION)
    attacker = int(game.current_player)
    serial = cs.search_reveal_verified_mate_depths(game, attacker=attacker, min_depth=1, max_depth=3)
    parallel = cs.search_reveal_verified_mate_depths(
        game, attacker=attacker, min_depth=1, max_depth=3, jobs=4)
    assert parallel["status"] == serial["status"] == "no_mate_within_max_depth"
    assert [a["jobs"] for a in parallel["attempts"]] == [1, 1, 4]
    assert parallel["jobs"] == 4
    anytime = cs.search_reveal_verified_mate_anytime(
        game, attacker=attacker, min_depth=3, max_depth=3, jobs=4)
    assert anytime["attempts"][0]["jobs"] == 4
    assert cs.search_reveal_verified_mate_depths(
        game, attacker=attacker, min_depth=1, max_depth=1, jobs=0)["jobs"] == cs.default_mate_jobs()


def test_exact_search_and_root_probe_take_jobs():
    assert cs.ExactMateSearchConfig().jobs == 1
    assert cs.RootMateProbeConfig().jobs == 1
    game = load_game_from_usi_text(BENCH_POSITION)
    results = []
    for jobs in (1, 4):
        config = cs.ExactMateSearchConfig()
        config.attacker = int(game.current_player)
        config.min_depth, config.max_depth = 1, 3
        config.jobs = jobs
        results.append(cs.exact_mate_search(game, config))
    assert results[0].status == results[1].status == "no_mate_within_max_depth"
    probes = []
    for jobs in (1, 4):
        config = cs.RootMateProbeConfig()
        config.jobs = jobs
        config.endgame_exact_depth = 3
        probes.append(cs.root_mate_probe(game, config, previous_value=0.72, time_limit_seconds=0.0))
    assert probes[0].proven == probes[1].proven
    assert probes[0].depth == probes[1].depth
