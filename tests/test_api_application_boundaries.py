"""Contracts for the R7 application and store boundaries."""

from __future__ import annotations

import asyncio
import subprocess
import sys
from pathlib import Path

import httpx
import pytest

import csplendor as cs
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
    yield
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


def test_web_api_does_not_offer_ai_move_selection():
    sessions["game"] = cs.Game(seed=42)

    assert _request("POST", "/game/game/ai_move").status_code == 404
    assert _request("GET", "/models").status_code == 404


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


def test_core_import_does_not_require_web_extras():
    script = r'''
import builtins

real_import = builtins.__import__
blocked = {"fastapi", "httpx", "pydantic", "uvicorn"}

def guarded_import(name, *args, **kwargs):
    if name.partition(".")[0] in blocked:
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
