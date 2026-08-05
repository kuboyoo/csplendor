"""Compatibility of focused USI modules with the canonical v1 specification."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from csplendor import Game
from csplendor.api.kifu_codec import build_kifu_text, parse_kifu_text
from csplendor.api.usi_parser import parse_usi_move
from csplendor.api.usi_resolver import find_legal_action_index_by_usi
from csplendor.api.usi_serializer import action_to_usi
from csplendor.api.usi_tokens import GEM_LETTERS

CANONICAL_COLOR_ORDER = ("W", "U", "G", "R", "K", "D")


@pytest.mark.parametrize(
    ("text", "kind"),
    [
        ("take:WUG", "take"),
        ("take:RR/return:D", "take"),
        ("reserve:C42", "reserve_visible"),
        ("reserve:L2", "reserve_deck"),
        ("buy:C71/gold:W2U1", "buy"),
        ("noble:N7", "noble"),
        ("pass", "pass"),
    ],
)
def test_canonical_v1_move_examples_parse(text, kind):
    assert parse_usi_move(text).kind == kind


def test_focused_serializer_and_resolver_round_trip_ordered_legal_actions():
    for seed in range(32):
        game = Game(seed=seed)
        for expected_index, action in enumerate(game.legal_actions):
            move = action_to_usi(action, game=game)
            assert find_legal_action_index_by_usi(game, move) == expected_index


def test_kifu_codec_has_stable_text_and_parse_contract():
    text = build_kifu_text(
        headers={"Format": "Splendor KIFU v1.0", "Players": "2"},
        position="startpos 2",
        moves=[{"player": 0, "usi": "take:WUG", "time_ms": 12}],
        result="ONGOING",
        final_scores=[0, 0],
        total_turns=0,
    )
    assert text == (
        "Format: Splendor KIFU v1.0\n"
        "Players: 2\n\n"
        "Position: startpos 2\n\n"
        "1. P0 take:WUG [12]\n\n"
        "Result: ONGOING\n"
        "FinalScores: P0=0 P1=0\n"
        "TotalTurns: 0\n"
    )
    assert parse_kifu_text(text)["moves"] == [
        {"player": 0, "usi": "take:WUG", "time_ms": 12}
    ]


def test_sibling_usi_repository_color_contract_when_checkout_is_available():
    canonical = Path(__file__).resolve().parents[2] / "usi" / "docs" / "USI.md"
    if not canonical.is_file():
        pytest.skip("canonical usi repository is not checked out beside csplendor")
    text = canonical.read_text(encoding="utf-8")
    rows = re.findall(
        r"\| `([WUGRKD])` \|.*?`(?:DIAMOND|SAPPHIRE|EMERALD|RUBY|ONYX|GOLD) = (\d)`",
        text,
    )
    assert tuple(symbol for symbol, _ in rows[:6]) == CANONICAL_COLOR_ORDER
    assert tuple(int(index) for _, index in rows[:6]) == tuple(range(6))
    assert GEM_LETTERS == CANONICAL_COLOR_ORDER
