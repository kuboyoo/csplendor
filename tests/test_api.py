from fastapi.testclient import TestClient

from csplendor.api.app import app, kifu_sessions, session_records, sessions

client = TestClient(app)


def setup_function():
    sessions.clear()
    session_records.clear()
    kifu_sessions.clear()


def _new_session(seed=42):
    response = client.post("/game", params={"seed": seed, "simple_payment_mode": True})
    assert response.status_code == 200
    session_id = response.json()["session_id"]
    assert session_id
    return session_id


def test_api_flow_apply_undo_and_errors():
    assert client.get("/game/missing").status_code == 404

    session_id = _new_session()
    state_response = client.get(f"/game/{session_id}")
    assert state_response.status_code == 200
    state = state_response.json()
    assert state["board"]["turn"] == 0
    assert state["board"]["current_player"] == 0
    assert len(state["legal_actions"]) > 0

    invalid = client.post(f"/game/{session_id}/action", params={"action_idx": 99999})
    assert invalid.status_code == 400

    response = client.post(f"/game/{session_id}/action", params={"action_idx": 0})
    assert response.status_code == 200
    after_first = response.json()
    assert after_first["board"]["turn"] == 0
    assert after_first["board"]["current_player"] == 1

    response = client.post(f"/game/{session_id}/action", params={"action_idx": 0})
    assert response.status_code == 200
    after_second = response.json()
    assert after_second["board"]["turn"] == 1
    assert after_second["board"]["current_player"] == 0

    response = client.post(f"/game/{session_id}/undo")
    assert response.status_code == 200
    undone = response.json()
    assert undone["board"]["turn"] == 0
    assert undone["board"]["current_player"] == 1

    response = client.post(f"/game/{session_id}/undo")
    assert response.status_code == 200
    start = response.json()
    assert start["board"]["turn"] == 0
    assert start["board"]["current_player"] == 0


def test_action_usi_endpoint_accepts_canonical_legal_move():
    session_id = _new_session(seed=100)
    state = client.get(f"/game/{session_id}").json()
    usi_move = state["legal_actions"][0]["usi"]
    assert usi_move

    response = client.post(
        f"/game/{session_id}/action_usi",
        json={"usi_move": usi_move, "time_ms": 123, "comment": "pytest"},
    )
    assert response.status_code == 200
    body = response.json()
    assert body["action_idx"] >= 0
    assert body["action_usi"] == usi_move
    assert body["state"]["board"]["current_player"] == 1

    bad = client.post(f"/game/{session_id}/action_usi", json={"usi_move": "take:DDD"})
    assert bad.status_code == 400


def test_kifu_meta_tracks_moves_and_undo():
    session_id = _new_session(seed=3)
    assert session_records[session_id]["moves"] == []

    response = client.post(f"/game/{session_id}/action", params={"action_idx": 0, "time_ms": 10})
    assert response.status_code == 200
    assert len(session_records[session_id]["moves"]) == 1
    assert session_records[session_id]["moves"][0]["time_ms"] == 10

    response = client.post(f"/game/{session_id}/undo")
    assert response.status_code == 200
    assert session_records[session_id]["moves"] == []
