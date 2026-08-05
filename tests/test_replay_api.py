"""Security and decoding contracts for the optional legacy replay API."""

from __future__ import annotations

import asyncio
import pickle

import httpx
import numpy as np
import pytest

import csplendor as cs
from csplendor.api import replay
from csplendor.api.app import app


def _request(method, url, **kwargs):
    async def send():
        transport = httpx.ASGITransport(app=app)
        async with httpx.AsyncClient(
            transport=transport, base_url="http://testserver"
        ) as client:
            return await client.request(method, url, **kwargs)

    return asyncio.run(send())


@pytest.fixture(autouse=True)
def _isolated_replay_directory(tmp_path, monkeypatch):
    replay._sessions.clear()
    data_dir = tmp_path / "replays"
    data_dir.mkdir()
    monkeypatch.setattr(replay, "_replay_data_dir", lambda: str(data_dir))
    yield data_dir
    replay._sessions.clear()


def _example(*, turn=0, player=0, value=0.0, final_turn=99):
    policy = np.zeros(406, dtype=np.float32)
    policy[7] = 0.75
    policy[3] = 0.25
    return {
        "board_ori": np.zeros((56, 7), dtype=np.int8),
        "policy_406": policy,
        "player": player,
        "turn": turn,
        "value_target": value,
        "final_turn": final_turn,
    }


def _write_pickle(path, value):
    with path.open("wb") as stream:
        pickle.dump(value, stream)


def test_replay_lookup_tables_match_native_engine_data():
    assert [card["id"] for card in replay._CARDS] == list(range(90))
    for card_id in range(90):
        card = cs.get_card(card_id)
        key = (tuple(card.cost), int(card.bonus), int(card.points))
        assert replay._card_lookup[key] == card_id

        cost_row = np.zeros(7, dtype=np.int8)
        bonus_row = np.zeros(7, dtype=np.int8)
        cost_row[:5] = card.cost
        cost_row[6] = card.points
        bonus_row[int(card.bonus)] = 1
        assert replay._card_id_from_ori(cost_row, bonus_row) == card_id

    assert [noble["id"] for noble in replay._NOBLES] == list(range(12))
    for noble_id in range(12):
        noble = cs.get_noble(noble_id)
        requirement = tuple(noble.requirement)
        assert replay._noble_lookup[requirement] == noble_id

        row = np.zeros(7, dtype=np.int8)
        row[:5] = noble.requirement
        assert replay._noble_id_from_ori(row) == noble_id


def test_replay_list_does_not_unpickle_or_expose_server_paths(
    _isolated_replay_directory,
):
    data_dir = _isolated_replay_directory
    replay_file = data_dir / "sample.pkl"
    replay_file.write_bytes(b"not even a pickle")

    response = _request("GET", "/replay/files")

    assert response.status_code == 200
    assert "administrator-controlled" in response.headers[
        "x-replay-format-warning"
    ]
    assert response.json() == {
        "files": [
            {
                "filename": "sample.pkl",
                "path": "sample.pkl",
                "num_examples": None,
                "size_mb": 0.0,
            }
        ]
    }


def test_replay_load_and_step_decode_trusted_directory_file(
    _isolated_replay_directory,
):
    replay_file = _isolated_replay_directory / "valid.pkl"
    _write_pickle(
        replay_file,
        [
            _example(turn=0, player=0),
            _example(turn=1, player=1, value=1.0, final_turn=1),
        ],
    )

    loaded = _request("POST", "/replay/load", params={"path": "valid.pkl"})
    assert loaded.status_code == 200
    payload = loaded.json()
    assert payload["filename"] == "valid.pkl"
    assert payload["num_games"] == 1
    assert payload["game_lengths"] == [2]

    step = _request("GET", f"/replay/{payload['session_id']}/game/0/1")
    assert step.status_code == 200
    body = step.json()
    assert body["turn"] == 1
    assert body["player"] == 1
    assert body["state"]["board"]["game_over"] is True
    assert body["state"]["board"]["winner"] == 1
    assert body["policy_top5"] == [
        {"action_idx": 7, "probability": 0.75},
        {"action_idx": 3, "probability": 0.25},
    ]

    assert _request("GET", "/replay/missing/game/0/0").status_code == 404
    assert (
        _request("GET", f"/replay/{payload['session_id']}/game/1/0").status_code == 400
    )
    assert (
        _request("GET", f"/replay/{payload['session_id']}/game/0/2").status_code == 400
    )


def test_replay_loader_rejects_traversal_and_arbitrary_absolute_files(
    _isolated_replay_directory, tmp_path
):
    outside = tmp_path / "outside.pkl"
    outside.write_bytes(b"untrusted payload must never be opened")

    for candidate in (str(outside), "../outside.pkl"):
        response = _request("POST", "/replay/load", params={"path": candidate})
        assert response.status_code == 400
        assert "inside the configured data directory" in response.json()["detail"]

    wrong_extension = _isolated_replay_directory / "replay.txt"
    wrong_extension.write_bytes(b"data")
    response = _request("POST", "/replay/load", params={"path": wrong_extension.name})
    assert response.status_code == 400
    assert response.json()["detail"] == "Only .pkl files are supported"


def test_replay_loader_accepts_legacy_absolute_path_inside_data_directory(
    _isolated_replay_directory,
):
    replay_file = _isolated_replay_directory / "absolute.pkl"
    _write_pickle(replay_file, [_example()])

    response = _request("POST", "/replay/load", params={"path": str(replay_file)})

    assert response.status_code == 200
    assert response.json()["filename"] == "absolute.pkl"


def test_replay_symlinks_cannot_escape_configured_directory(
    _isolated_replay_directory, tmp_path
):
    outside = tmp_path / "outside.pkl"
    _write_pickle(outside, [_example()])
    escaping_link = _isolated_replay_directory / "escaping.pkl"
    escaping_link.symlink_to(outside)
    broken_link = _isolated_replay_directory / "broken.pkl"
    broken_link.symlink_to(tmp_path / "does-not-exist.pkl")

    listed = _request("GET", "/replay/files")
    loaded = _request("POST", "/replay/load", params={"path": escaping_link.name})

    assert listed.status_code == 200
    assert listed.json() == {"files": []}
    assert loaded.status_code == 400
    assert "inside the configured data directory" in loaded.json()["detail"]


def test_replay_loader_rejects_embedded_nul_without_internal_error(
    _isolated_replay_directory,
):
    response = _request("POST", "/replay/load", params={"path": "bad\0.pkl"})

    assert response.status_code == 400
    assert response.json()["detail"] == "Invalid replay filename"


def test_replay_loader_rejects_missing_large_corrupt_and_invalid_payloads(
    _isolated_replay_directory, monkeypatch
):
    missing = _request("POST", "/replay/load", params={"path": "missing.pkl"})
    assert missing.status_code == 404

    corrupt = _isolated_replay_directory / "corrupt.pkl"
    corrupt.write_bytes(b"not a pickle")
    response = _request("POST", "/replay/load", params={"path": corrupt.name})
    assert response.status_code == 400
    assert response.json()["detail"] == "Failed to load replay"

    monkeypatch.setattr(replay, "_MAX_REPLAY_BYTES", 1)
    response = _request("POST", "/replay/load", params={"path": corrupt.name})
    assert response.status_code == 413
    monkeypatch.setattr(replay, "_MAX_REPLAY_BYTES", 512 * 1024 * 1024)

    invalid_cases = [
        [],
        [{}],
        [{**_example(), "board_ori": np.zeros((55, 7), dtype=np.int8)}],
        [{**_example(), "policy_406": np.zeros((2, 2), dtype=np.float32)}],
        [{**_example(), "policy_406": np.zeros(405, dtype=np.float32)}],
        [{**_example(), "policy_406": np.full(406, np.nan, dtype=np.float32)}],
        [{**_example(), "board_ori": np.zeros((56, 7), dtype=np.float32)}],
        [{**_example(), "player": 2}],
        [{**_example(), "turn": 0.5}],
        [{**_example(), "value_target": np.inf}],
        [{**_example(), "turn": object()}],
    ]
    for index, value in enumerate(invalid_cases):
        path = _isolated_replay_directory / f"invalid-{index}.pkl"
        _write_pickle(path, value)
        response = _request("POST", "/replay/load", params={"path": path.name})
        assert response.status_code == 400


def test_replay_loader_enforces_step_count_and_normalizes_numpy_scalars(
    _isolated_replay_directory, monkeypatch
):
    replay_file = _isolated_replay_directory / "normalized.pkl"
    _write_pickle(
        replay_file,
        [
            _example(
                turn=np.float64(0.0),
                player=np.int8(0),
                value=np.float32(0.25),
                final_turn=np.int16(9),
            ),
            _example(turn=np.int64(1), player=np.float64(1.0)),
        ],
    )

    monkeypatch.setattr(replay, "_MAX_REPLAY_EXAMPLES", 1)
    too_many = _request("POST", "/replay/load", params={"path": replay_file.name})
    assert too_many.status_code == 413

    monkeypatch.setattr(replay, "_MAX_REPLAY_EXAMPLES", 2)
    loaded = _request("POST", "/replay/load", params={"path": replay_file.name})
    assert loaded.status_code == 200
    session = replay._sessions[loaded.json()["session_id"]]
    assert session.examples[0]["turn"] == 0
    assert type(session.examples[0]["turn"]) is int
    assert type(session.examples[0]["value_target"]) is float


def test_replay_session_splits_on_nonincreasing_turns():
    session = replay.ReplaySession(
        "fixture.pkl",
        [
            _example(turn=0),
            _example(turn=1),
            _example(turn=0),
            _example(turn=1),
        ],
    )
    assert [len(game) for game in session.games] == [2, 2]
