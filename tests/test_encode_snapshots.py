import numpy as np
import pytest

import csplendor as cs

LAYOUTS = [
    (False, False, False),
    (True, False, False),
    (True, True, False),
    (True, True, True),
]


def _positions(count=40):
    """Varied positions, including pending token returns and noble choices."""
    rng = np.random.default_rng(7)
    positions = []
    for seed in range(count):
        game = cs.Game(seed=seed)
        stop_at_return = seed % 4 == 0
        for _ in range(int(rng.integers(0, 120)) if not stop_at_return else 400):
            if game.is_game_over() or (stop_at_return and game.board.waiting_return):
                break
            game.apply_random_action(int(rng.integers(1 << 30)))
        positions.append(game)
    assert any(g.board.waiting_return for g in positions)
    return positions


def _reference(game, player, public, seat, pending):
    parts = [cs.StateEncoder.encode_canonical(game, player, player)]
    if public:
        parts.append(cs.StateEncoder.encode_public_card_statistics(game, player, player))
    if seat:
        parts.append([2 * player - 1])
    if pending:
        parts.append([float(game.board.waiting_return), float(game.board.waiting_noble)])
    return np.concatenate([np.asarray(p, dtype=np.float32) for p in parts])


def _pack(games):
    snapshots = [g.serialize_snapshot() for g in games]
    data = np.frombuffer(b"".join(snapshots), dtype=np.uint8)
    offsets = np.concatenate([[0], np.cumsum([len(s) for s in snapshots])]).astype(np.int64)
    return data, offsets


@pytest.mark.parametrize("public,seat,pending", LAYOUTS)
def test_encode_snapshots_matches_the_per_game_encoders(public, seat, pending):
    games = _positions()
    players = np.array([g.current_player if i % 3 else 1 - g.current_player
                        for i, g in enumerate(games)], dtype=np.int8)
    data, offsets = _pack(games)
    encoded = cs.StateEncoder.encode_snapshots(
        data, offsets, players, public_card_features=public,
        physical_seat_feature=seat, pending_decision_features=pending)
    expected = np.stack([_reference(g, int(p), public, seat, pending)
                         for g, p in zip(games, players)])
    assert encoded.dtype == np.float32 and encoded.shape == expected.shape
    assert np.array_equal(encoded, expected)


def test_encode_snapshots_matches_the_v3_leaf_layout():
    game = _positions(5)[3]
    session = cs.V3SearchSession(cs.V3SearchConfig())
    session.add_game(game, game.current_player, seed=1)
    features, _, _, _ = session.collect()  # the root row
    data, offsets = _pack([game])
    encoded = cs.StateEncoder.encode_snapshots(
        data, offsets, np.array([game.current_player], dtype=np.int8))
    assert np.array_equal(encoded, features)


def test_encode_snapshots_of_no_rows():
    encoded = cs.StateEncoder.encode_snapshots(
        np.zeros(0, dtype=np.uint8), np.zeros(1, dtype=np.int64), np.zeros(0, dtype=np.int8))
    assert encoded.shape == (0, 316) and encoded.dtype == np.float32


def test_encode_snapshots_rejects_bad_input():
    games = _positions(3)
    data, offsets = _pack(games)
    players = np.zeros(3, dtype=np.int8)
    encode = cs.StateEncoder.encode_snapshots
    with pytest.raises(ValueError):
        encode(data, offsets, players, public_card_features=False, physical_seat_feature=True)
    with pytest.raises(ValueError):
        encode(data, offsets, players, physical_seat_feature=False, pending_decision_features=True)
    with pytest.raises(ValueError):
        encode(data, offsets[:-1], players)  # offsets must have N + 1 entries
    bad = offsets.copy()
    bad[1], bad[2] = bad[2], bad[1]
    with pytest.raises(ValueError):
        encode(data, bad, players)  # decreasing
    bad = offsets.copy()
    bad[-1] += 1
    with pytest.raises(ValueError):
        encode(data, bad, players)  # past the data
    with pytest.raises(ValueError):
        encode(data, offsets, np.array([0, 2, 1], dtype=np.int8))
    corrupt = data.copy()
    corrupt[offsets[1] + 30] ^= 0xFF
    with pytest.raises(ValueError):
        encode(corrupt, offsets, players)
