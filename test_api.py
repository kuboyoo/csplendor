from fastapi.testclient import TestClient
import pytest
from csplendor.api import app

client = TestClient(app)

def test_api_flow():
    # 1. Create a game
    response = client.post("/game?seed=42")
    assert response.status_code == 200
    data = response.json()
    session_id = data["session_id"]
    assert session_id is not None
    
    # 2. Get state
    response = client.get(f"/game/{session_id}")
    assert response.status_code == 200
    state = response.json()
    assert state["board"]["turn"] == 0
    assert len(state["legal_actions"]) > 0
    
    # 3. Apply an action
    # Take different colors
    response = client.post(f"/game/{session_id}/action?action_idx=0")
    assert response.status_code == 200
    new_state = response.json()
    assert new_state["board"]["turn"] == 0 # Still turn 0 until Player 1 moves
    assert new_state["board"]["current_player"] == 1
    
    # Apply another move (Player 1)
    response = client.post(f"/game/{session_id}/action?action_idx=0")
    assert response.status_code == 200
    final_state = response.json()
    assert final_state["board"]["turn"] == 1
    assert final_state["board"]["current_player"] == 0
    
    # 4. Undo (now at turn 1, current_player 0)
    response = client.post(f"/game/{session_id}/undo")
    assert response.status_code == 200
    undone_state = response.json()
    assert undone_state["board"]["turn"] == 0
    assert undone_state["board"]["current_player"] == 1
    
    # Undo again (back to start)
    response = client.post(f"/game/{session_id}/undo")
    assert response.status_code == 200
    start_state = response.json()
    assert start_state["board"]["turn"] == 0
    assert start_state["board"]["current_player"] == 0
    
    print("API Flow test passed!")

if __name__ == "__main__":
    test_api_flow()
