"""Standalone-PyPI contracts for the optional external AI bridge."""

from __future__ import annotations

import asyncio
import subprocess
import sys
from pathlib import Path

import httpx
import numpy as np
import pytest

import csplendor as cs
from csplendor.api.ai_manager import AIIntegrationUnavailable, AIManager
from csplendor.api.app import app, kifu_sessions, session_records, sessions

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def _request(method, url, **kwargs):
    async def send():
        transport = httpx.ASGITransport(app=app)
        async with httpx.AsyncClient(
            transport=transport, base_url="http://testserver"
        ) as client:
            return await client.request(method, url, **kwargs)

    return asyncio.run(send())


@pytest.fixture(autouse=True)
def _clear_sessions():
    sessions.clear()
    session_records.clear()
    kifu_sessions.clear()
    AIManager._instance = None
    yield
    AIManager._instance = None
    sessions.clear()
    session_records.clear()
    kifu_sessions.clear()


def test_ai_manager_module_import_does_not_eagerly_require_external_stacks():
    script = r'''
import builtins

real_import = builtins.__import__

def guarded_import(name, *args, **kwargs):
    if name == "torch" or name.startswith("dlsplendor"):
        raise ImportError("blocked optional dependency")
    return real_import(name, *args, **kwargs)

builtins.__import__ = guarded_import
from csplendor.api import ai_manager

assert ai_manager.AIManager is not None
try:
    ai_manager._load_base_ai_dependencies()
except ai_manager.AIIntegrationUnavailable as error:
    assert "Optional AI integration is unavailable" in str(error)
else:
    raise AssertionError("dependency loader unexpectedly succeeded")
'''
    completed = subprocess.run(
        [sys.executable, "-c", script],
        cwd=PROJECT_ROOT,
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


def test_core_import_does_not_require_web_or_ml_extras():
    script = r'''
import builtins

real_import = builtins.__import__
blocked = {"fastapi", "httpx", "pydantic", "torch", "uvicorn"}

def guarded_import(name, *args, **kwargs):
    root = name.partition(".")[0]
    if root in blocked or name.startswith("dlsplendor"):
        raise ImportError(f"blocked optional dependency: {name}")
    return real_import(name, *args, **kwargs)

builtins.__import__ = guarded_import
import csplendor

game = csplendor.Game(seed=42)
assert game.legal_action_count > 0
'''
    completed = subprocess.run(
        [sys.executable, "-c", script],
        cwd=PROJECT_ROOT,
        text=True,
        capture_output=True,
        timeout=30,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr


@pytest.mark.parametrize("ai_type", ["deepsets", "set_transformer", "nnue"])
def test_ai_endpoint_validates_fixed_budget_before_loading_models(ai_type):
    sessions["game"] = cs.Game(seed=42)
    response = _request("POST", f"/game/game/ai_move?ai_type={ai_type}")

    assert response.status_code == 400
    assert "positive num_simulations" in response.json()["detail"]


def test_ai_endpoint_rejects_unknown_mode_before_loading_models():
    sessions["game"] = cs.Game(seed=42)
    response = _request("POST", "/game/game/ai_move?ai_type=not-an-ai")

    assert response.status_code == 400
    assert response.json()["detail"] == "Unknown AI type: not-an-ai"


def test_ai_endpoint_maps_missing_optional_stack_to_service_unavailable(
    monkeypatch,
):
    sessions["game"] = cs.Game(seed=42)

    def unavailable(_cls):
        raise AIIntegrationUnavailable("external AI stack is absent")

    monkeypatch.setattr(AIManager, "get_instance", classmethod(unavailable))
    response = _request("POST", "/game/game/ai_move?ai_type=greedy")

    assert response.status_code == 503
    assert response.json()["detail"] == "external AI stack is absent"


def test_ai_endpoint_serializes_a_detached_manager_result(monkeypatch):
    sessions["game"] = cs.Game(seed=42)

    class FakeManager:
        _last_action_debug = {
            "used_mode": "test-greedy",
            "actual_simulations": 7,
            "elapsed_ms": 12.75,
        }

        def get_best_action(self, game, **kwargs):
            assert game is sessions["game"]
            assert kwargs["ai_type"] == "greedy"
            return 0

    monkeypatch.setattr(
        AIManager, "get_instance", classmethod(lambda _cls: FakeManager())
    )
    response = _request("POST", "/game/game/ai_move?ai_type=greedy")

    assert response.status_code == 200
    payload = response.json()
    assert payload["action_idx"] == 0
    assert payload["action_usi"]
    assert payload["used_mode"] == "test-greedy"
    assert payload["used_simulations"] == 7
    assert payload["elapsed_ms"] == 12


def test_ai_manager_dispatch_and_action_equality_without_external_models(
    monkeypatch,
):
    manager = AIManager.__new__(AIManager)
    game = cs.Game(seed=42)
    expected = {
        "greedy": 1,
        "genbu": 2,
        "alphazero": 3,
        "deepsets": 4,
        "set_transformer": 5,
        "nnue": 6,
        "mcts": 7,
    }
    monkeypatch.setattr(manager, "_get_greedy_action", lambda _game: 1)
    monkeypatch.setattr(manager, "_get_genbu_action", lambda _game, **_kw: 2)
    monkeypatch.setattr(
        manager, "_get_alphazero_action", lambda _game, **_kw: 3
    )
    monkeypatch.setattr(
        manager,
        "_get_deepsets_action",
        lambda _game, **kw: 5 if kw["model_type"] == "set_transformer" else 4,
    )
    monkeypatch.setattr(manager, "_get_nnue_action", lambda _game, **_kw: 6)
    monkeypatch.setattr(manager, "_get_mcts_action", lambda _game, **_kw: 7)

    for ai_type, action_index in expected.items():
        assert manager.get_best_action(game, ai_type=ai_type) == action_index

    first = game.legal_actions[0]
    same = cs.Action.unpack(int(first.pack()))
    different = cs.Action.unpack(int(first.pack()))
    different.noble_choice = 0
    assert manager._actions_equal(first, same)
    assert not manager._actions_equal(first, different)


def test_optional_model_path_resolvers_validate_explicit_paths(tmp_path):
    manager = AIManager.__new__(AIManager)
    checkpoint = tmp_path / "model.pt"
    checkpoint.write_bytes(b"not loaded by this test")

    assert manager._resolve_distilled_model_path(
        "deepsets", str(checkpoint)
    ) == str(checkpoint)
    assert manager._resolve_nnue_model_path(str(checkpoint)) == str(checkpoint)

    missing = tmp_path / "missing.pt"
    with pytest.raises(FileNotFoundError, match="Distilled model path"):
        manager._resolve_distilled_model_path("deepsets", str(missing))
    with pytest.raises(FileNotFoundError, match="NNUE model path"):
        manager._resolve_nnue_model_path(str(missing))


def test_deepsets_raw_policy_maps_selected_v3_action(monkeypatch):
    manager = AIManager.__new__(AIManager)
    game = cs.Game(seed=42)
    selected_action = game.legal_actions[0]
    selected_v3 = int(cs.ActionEncoderV3.encode(selected_action, game))
    policy = np.zeros(cs.ActionEncoderV3.ACTION_SIZE, dtype=np.float32)
    policy[selected_v3] = 1.0

    class FakeNetwork:
        def predict(self, _state, valid_actions, *, device):
            assert valid_actions[selected_v3] == 1
            assert device == "cpu"
            return policy, 0.25, 3.0

    manager._ds_initialized = True
    manager._ds_device = "cpu"
    manager._ds_net = FakeNetwork()
    manager._ds_encode_state = lambda _game: np.zeros(1, dtype=np.float32)
    monkeypatch.setattr(manager, "_init_deepsets", lambda **_kwargs: None)

    assert manager._get_deepsets_action(game) == 0
    assert manager._last_action_debug["used_mode"] == "deepsets_raw"
