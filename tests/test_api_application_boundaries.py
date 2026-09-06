"""Contracts for the R7 application, store and optional-AI boundaries."""

from __future__ import annotations

import asyncio
import logging
import subprocess
import sys
from pathlib import Path

import httpx
import pytest

import csplendor as cs
from csplendor.api import ai_manager
from csplendor.api.ai_provider import (
    AIDecision,
    AIRequest,
    reset_ai_provider,
    set_ai_provider,
)
from csplendor.api.app import app, kifu_sessions, session_records, sessions
from csplendor.api.game_service import GameSessionService
from csplendor.api.stores import InMemoryStore, KifuStore, SessionStore

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
def _isolated_application_state():
    sessions.clear()
    session_records.clear()
    kifu_sessions.clear()
    reset_ai_provider()
    yield
    reset_ai_provider()
    sessions.clear()
    session_records.clear()
    kifu_sessions.clear()


def test_in_memory_store_satisfies_replaceable_store_interfaces():
    game_store = InMemoryStore()
    record_store = InMemoryStore()
    assert isinstance(game_store, SessionStore)
    assert isinstance(record_store, KifuStore)

    service = GameSessionService(
        game_store,
        record_store,
        id_factory=lambda: "fixed-session",
    )
    session_id = service.create_game(
        seed=42,
        simple_payment_mode=True,
        player0_name="A",
        player1_name="B",
    )
    assert session_id == "fixed-session"
    assert service.require_game(session_id) is game_store[session_id]
    assert record_store[session_id]["meta"]["Player0"] == "A"


def test_fake_ai_provider_controls_move_and_model_endpoints():
    class FakeProvider:
        def choose_action(self, game, request: AIRequest) -> AIDecision:
            assert game is sessions["game"]
            assert request.ai_type == "greedy"
            return AIDecision(
                action_index=0,
                used_mode="fake-provider",
                used_simulations=11,
                elapsed_ms=3,
            )

        def list_models(self):
            return [
                {
                    "path": "/virtual/model.pt",
                    "name": "model.pt",
                    "display": "virtual/model.pt",
                    "dir": "virtual",
                }
            ]

    sessions["game"] = cs.Game(seed=42)
    set_ai_provider(FakeProvider())

    move = _request("POST", "/game/game/ai_move?ai_type=greedy")
    models = _request("GET", "/models")

    assert move.status_code == 200
    assert move.json()["used_mode"] == "fake-provider"
    assert move.json()["used_simulations"] == 11
    assert models.json()["models"][0]["name"] == "model.pt"


def test_web_application_import_does_not_scan_external_models_or_repositories():
    script = r'''
import glob
import os

import csplendor
import fastapi

def forbidden(*_args, **_kwargs):
    raise AssertionError("external discovery happened during import")

glob.glob = forbidden
os.path.exists = forbidden
os.path.isdir = forbidden
from csplendor.api import app

assert app is not None
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


def test_legacy_ai_diagnostics_are_structured_log_records(caplog):
    caplog.set_level(logging.WARNING, logger="csplendor.api.ai_manager")
    ai_manager._structured_diagnostic("WARNING: fixture diagnostic")

    record = caplog.records[-1]
    assert record.getMessage() == "WARNING: fixture diagnostic"
    assert record.event == "legacy_ai_diagnostic"
    assert record.component == "ai_manager"
