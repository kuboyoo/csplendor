"""V3SearchSession: retries after a collision, several batches in flight, set_budget (doc/local_engine_20261106.md of dlsplendor)."""

import os
import subprocess
import sys

import numpy as np
import pytest

import csplendor


def config(leaf=64, simulations=3200, retries=0, in_flight=1):
    c = csplendor.V3SearchConfig()
    c.num_simulations = simulations
    c.leaf_batch_size = leaf
    c.collision_retries = retries
    c.max_in_flight = in_flight
    c.chance_widening_base = 1.0
    c.chance_widening_alpha = 0.5
    return c


def uniform(legal, offsets):
    """Priors 1/n per row and a value from the row count, deterministic."""
    legal, offsets = np.asarray(legal), np.asarray(offsets)
    counts = np.diff(offsets)
    priors = np.repeat(1.0 / counts, counts).astype(np.float32)
    values = (np.sin(np.arange(len(counts)) + counts) * 0.3).astype(np.float32)
    return legal, offsets, priors, values


def apply(session, batch):
    _features, legal, offsets, _slots = batch
    session.apply(*uniform(legal, offsets))


def search(c, seed=3):
    session = csplendor.V3SearchSession(c)
    slot = session.add_game(csplendor.Game(seed), 0, seed, False, c.num_simulations)
    rows = []
    while not session.done(slot):
        batch = session.collect()
        rows.append(len(batch[3]))
        if rows[-1]:
            apply(session, batch)
    return session, slot, rows


def test_defaults_keep_the_old_behaviour():
    c = csplendor.V3SearchConfig()
    assert c.collision_retries == 0 and c.max_in_flight == 1


@pytest.mark.parametrize("threads", [1, 4])
def test_defaults_match_the_main_build_exactly(threads):
    """Root visits with the defaults equal those of the installed csplendor (built without these changes)."""
    code = (
        "import sys, json; sys.path.insert(0, sys.argv[1]); import csplendor, numpy as np\n"
        "c = csplendor.V3SearchConfig(); c.num_simulations = 1600; c.leaf_batch_size = 16; c.num_threads = int(sys.argv[2])\n"
        "c.chance_widening_base = 1.0; c.chance_widening_alpha = 0.5\n"
        "s = csplendor.V3SearchSession(c); slots = [s.add_game(csplendor.Game(5 + i), i % 2, 5 + i, False, 1600) for i in range(6)]\n"
        "while not s.all_done():\n"
        "    f, l, o, sl = s.collect()\n"
        "    if len(sl):\n"
        "        l, o = np.asarray(l), np.asarray(o); n = np.diff(o)\n"
        "        s.apply(l, o, np.repeat(1.0 / n, n).astype(np.float32), (np.sin(np.arange(len(n)) + n) * 0.3).astype(np.float32))\n"
        "print(json.dumps([sorted(s.root_visits(slot).items()) for slot in slots]))\n"
    )
    here = os.path.dirname(os.path.dirname(os.path.abspath(csplendor.__file__)))
    main = os.environ.get("CSPLENDOR_MAIN_BUILD")
    if not main:
        pytest.skip("set CSPLENDOR_MAIN_BUILD to a csplendor checkout built from main")
    ours = subprocess.run([sys.executable, "-c", code, here, str(threads)], capture_output=True, text=True, check=True).stdout
    theirs = subprocess.run([sys.executable, "-c", code, main, str(threads)], capture_output=True, text=True, check=True).stdout
    assert ours == theirs


def test_collision_retries_fill_the_batch_of_one_tree():
    _s, _slot, plain = search(config(retries=0))
    _s, _slot, retried = search(config(retries=64))
    full = [r for r in retried[:-1] if r]
    assert np.mean(full) >= 0.9 * 64
    assert np.mean(full) > np.mean([r for r in plain[:-1] if r]) + 10


def test_retried_search_spends_its_budget_and_leaves_no_virtual_visits():
    session, slot, _rows = search(config(retries=64))
    assert session.simulations(slot) >= 3200
    assert session.virtual_visit_total(slot) == 0
    assert sum(session.root_visits(slot).values()) >= 3199  # the root's own expansion has no edge


def test_two_batches_in_flight_apply_oldest_first():
    c = config(retries=64, in_flight=2)
    session = csplendor.V3SearchSession(c)
    slot = session.add_game(csplendor.Game(9), 0, 9, False, 3200)
    for _ in range(4):  # past the root's own expansion, so two batches find leaves
        apply(session, session.collect())
    first = session.collect()
    second = session.collect()
    assert session.in_flight() == 2 and session.pending() == len(first[3]) + len(second[3])
    with pytest.raises(RuntimeError):
        session.collect()  # a third batch is over max_in_flight
    apply(session, first)
    apply(session, second)
    assert session.in_flight() == 0 and session.pending() == 0
    while not session.done(slot):
        a = session.collect()
        b = session.collect() if not session.done(slot) else None
        for batch in (a, b):
            if batch is not None and len(batch[3]):
                apply(session, batch)
    assert session.virtual_visit_total(slot) == 0
    assert sum(session.root_visits(slot).values()) >= 3199  # the root's own expansion has no edge


def test_reset_with_batches_in_flight_drops_their_results():
    c = config(retries=64, in_flight=2)
    session = csplendor.V3SearchSession(c)
    slot = session.add_game(csplendor.Game(4), 0, 4, False, 3200)
    for _ in range(4):
        apply(session, session.collect())
    first, second = session.collect(), session.collect()
    assert session.in_flight() == 2
    session.reset_game(slot, csplendor.Game(11), 0, 11, False, 800)
    apply(session, first)
    apply(session, second)
    assert session.simulations(slot) == 0 and session.virtual_visit_total(slot) == 0


def test_advance_keeps_the_tree_only_when_nothing_is_in_flight():
    c = config(retries=64, in_flight=2)
    game = csplendor.Game(6)
    child = game.clone()
    action = child.legal_actions[0]
    action_id = csplendor.ActionEncoderV5.encode(action, game)
    child.apply(action)

    def grown():
        session = csplendor.V3SearchSession(c)
        slot = session.add_game(game, 0, 6, False, 3200)
        for _ in range(20):
            apply(session, session.collect())
        return session, slot

    session, slot = grown()
    assert session.advance_game(slot, child, [action_id], 0, 6, False, 800) is True
    session, slot = grown()
    session.collect()
    assert session.advance_game(slot, child, [action_id], 0, 6, False, 800) is False


def test_set_budget_extends_the_search_and_keeps_the_tree():
    c = config(simulations=256, retries=64)
    session = csplendor.V3SearchSession(c)
    slot = session.add_game(csplendor.Game(8), 0, 8, False, 256)
    while not session.done(slot):
        batch = session.collect()
        if len(batch[3]):
            apply(session, batch)
    before = sum(session.root_visits(slot).values())
    session.set_budget(slot, session.simulations(slot) + 512)
    assert not session.done(slot)
    while not session.done(slot):
        batch = session.collect()
        if len(batch[3]):
            apply(session, batch)
    assert sum(session.root_visits(slot).values()) >= before + 512
