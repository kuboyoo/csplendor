"""Mate solver output and CLI module contracts."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import csplendor as cs
from csplendor.api.usi_kifu import action_to_usi
from scripts import dfpn_cli, dfpn_output
from scripts import dfpn_mate_solver as legacy

REPO_ROOT = Path(__file__).resolve().parents[1]


def _sha256(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def _strategy_dag_fixture():
    return {
        "format": "strategy_dag_v1",
        "requested": True,
        "complete": True,
        "validated": True,
        "omitted_reason": None,
        "root": 0,
        "nodes": [
            {
                "id": 0,
                "player": 0,
                "depth": 2,
                "kind": "state",
                "resolution": None,
                "children": [
                    {"action_code": 123, "reveal_card": 7, "child": 1},
                    {"action_code": 123, "reveal_card": 3, "child": 1},
                    {"action_code": 456, "reveal_card": None, "child": 2},
                ],
            },
            {
                "id": 1,
                "player": 1,
                "depth": 1,
                "kind": "terminal",
                "resolution": "attacker_win",
                "children": [],
            },
            {
                "id": 2,
                "player": 1,
                "depth": 1,
                "kind": "terminal",
                "resolution": "attacker_win",
                "children": [],
            },
        ],
    }


def test_legacy_dag_exports_keep_byte_and_semantic_digest():
    v1 = _strategy_dag_fixture()
    compact = legacy.proof_dag_to_compact(v1, include_card_classes=True)
    expanded = legacy.compact_proof_dag_to_v1(compact)
    payload = {
        "compact": compact,
        "expanded": expanded,
        "size": legacy.strategy_dag_size_report(v1, compact),
        "nodes": legacy.strategy_dag_node_count(compact),
        "max": legacy.strategy_dag_max_children(compact, player=0),
    }
    serialized = json.dumps(
        payload,
        ensure_ascii=False,
        separators=(",", ":"),
    )

    assert _sha256(serialized) == (
        "88f3fea98cef0c469b2ae6478c7dd340b6e2ca6146e36a7ec64fa89a181ef11d"
    )
    assert expanded["nodes"][0]["children"][0]["reveal_card"] == 3
    assert expanded["nodes"][0]["children"][1]["reveal_card"] == 7
    assert dfpn_output.proof_dag_to_compact(
        v1,
        include_card_classes=True,
    ) == compact
    assert dfpn_output.compact_proof_dag_to_v1(compact) == expanded
    assert legacy.proof_dag_to_compact.__module__ == (
        "scripts.dfpn_mate_solver"
    )


def test_legacy_kifu_export_and_now_iso_monkeypatch_keep_digest(monkeypatch):
    monkeypatch.setattr(
        legacy,
        "now_iso",
        lambda: "2000-01-02T03:04:05+00:00",
    )
    game = cs.Game(seed=42)
    usi = action_to_usi(game.legal_actions[0], game=game)
    text = legacy.principal_line_to_kifu_text(
        game,
        [{"player": 0, "action": {"usi": usi}}],
        attacker=0,
    )
    monkeypatch.setattr(
        dfpn_output,
        "now_iso",
        lambda: "2000-01-02T03:04:05+00:00",
    )
    direct_text = dfpn_output.principal_line_to_kifu_text(
        game,
        [{"player": 0, "action": {"usi": usi}}],
        attacker=0,
    )

    # Only the KIFU format header changed for v1.1; with "v1.0" the digest is
    # fe7de2921e5977be50333249fdae202ad512b63a81c83a26c3601d9cf5d8550d.
    assert "Format: Splendor KIFU v1.2\n" in text
    assert _sha256(text) == (
        "b48ca436e78f4808173e5640dd5143ec727c1b8cd52684ea3493f5bb938fe71f"
    )
    assert direct_text == text


_INVALID_ARGS = ["--visible-only-winner", "--reveal-proof-dag"]


def test_cli_reports_invalid_input_as_json(capsys):
    code = legacy.main(_INVALID_ARGS)
    captured = capsys.readouterr()

    assert code == 1
    assert captured.err == ""
    payload = json.loads(captured.out)
    assert payload["status"] == "InvalidInput"
    assert "--visible-only-winner" in payload["refutation"]["error"]

    direct_code = dfpn_cli.main(_INVALID_ARGS, api=legacy)
    direct = capsys.readouterr()
    assert direct_code == code
    assert direct == captured


def test_script_entry_points_share_the_cli():
    completed = subprocess.run(
        [sys.executable, str(REPO_ROOT / "scripts" / "dfpn_mate_solver.py"),
         *_INVALID_ARGS],
        cwd=REPO_ROOT,
        text=True,
        capture_output=True,
        check=False,
    )
    assert completed.returncode == 1
    assert completed.stderr == ""

    for command, cwd in (
        ([sys.executable, "-m", "scripts.dfpn_cli", *_INVALID_ARGS], REPO_ROOT),
        ([sys.executable, str(REPO_ROOT / "scripts" / "dfpn_cli.py"),
          *_INVALID_ARGS], REPO_ROOT.parent),
    ):
        entry = subprocess.run(
            command, cwd=cwd, text=True, capture_output=True, check=False)
        assert entry.returncode == completed.returncode
        assert entry.stdout == completed.stdout
        assert entry.stderr == completed.stderr
