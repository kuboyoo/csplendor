from __future__ import annotations

import numpy as np

import csplendor
from csplendor.api.schemas import GemType as ApiGemType
from csplendor.api.usi_kifu import GEM_LETTERS, action_to_usi, parse_usi_move

CANONICAL_GEMS = (
    ("DIAMOND", 0, "Diamond", "D", "W"),
    ("SAPPHIRE", 1, "Sapphire", "S", "U"),
    ("EMERALD", 2, "Emerald", "E", "G"),
    ("RUBY", 3, "Ruby", "R", "R"),
    ("ONYX", 4, "Onyx", "O", "K"),
    ("GOLD", 5, "Gold", "G", "D"),
)


def test_native_schema_names_and_symbols_share_the_canonical_order() -> None:
    assert [
        (name, int(getattr(csplendor.GemType, name)))
        for name, *_ in CANONICAL_GEMS
    ] == [(name, index) for name, index, *_ in CANONICAL_GEMS]
    assert [(member.name, int(member)) for member in ApiGemType] == [
        (name, index) for name, index, *_ in CANONICAL_GEMS
    ]
    assert csplendor.GEM_NAMES == [entry[2] for entry in CANONICAL_GEMS]
    assert csplendor.GEM_SYMBOLS == [entry[3] for entry in CANONICAL_GEMS]
    assert csplendor.GEM_USI_SYMBOLS == [entry[4] for entry in CANONICAL_GEMS]
    assert GEM_LETTERS == tuple(csplendor.GEM_USI_SYMBOLS)


def test_usi_letters_address_the_same_native_array_indices() -> None:
    for _, index, _, _, usi_symbol in CANONICAL_GEMS[:5]:
        parsed = parse_usi_move(f"take:{usi_symbol}")
        expected = [0] * 6
        expected[index] = 1
        assert parsed.take == expected

        action = csplendor.Action()
        action.type = csplendor.ActionType.TAKE_DIFFERENT
        action.take = expected[:5]
        assert action_to_usi(action) == f"take:{usi_symbol}"

    parsed_with_gold_return = parse_usi_move("take:W/return:D")
    assert parsed_with_gold_return.return_gems == [0, 0, 0, 0, 0, 1]


def test_card_state_and_v3_action_layout_keep_the_canonical_color_meaning() -> None:
    diamond_card = csplendor.get_card(15)
    assert list(diamond_card.cost) == [4, 0, 0, 0, 0]
    assert diamond_card.cost[int(csplendor.GemType.DIAMOND)] == 4

    game = csplendor.Game(seed=20260805)
    game.board.bank = [1, 2, 3, 4, 0, 5]
    features = np.asarray(csplendor.StateEncoder.encode(game), dtype=np.float32)
    np.testing.assert_allclose(
        features[:6], np.asarray(game.board.bank, dtype=np.float32) / 7.0
    )

    # ActionEncoderV3 reserves IDs 840..979 for five TAKE_SAME colors,
    # with 28 return patterns per color and pattern zero meaning no return.
    for _, color, *_ in CANONICAL_GEMS[:5]:
        action = csplendor.ActionEncoderV3.decode(840 + color * 28, game)
        expected_take = [0] * 5
        expected_take[color] = 2
        assert list(action.take) == expected_take


def test_state_schema_identifier_covers_the_frozen_color_layout() -> None:
    assert csplendor.StateEncoder.schema_version() == 1
    assert csplendor.StateEncoder.schema_fingerprint() == (
        "csplendor.state.v1;base=196;public=117;gems="
        "diamond,sapphire,emerald,ruby,onyx,gold"
    )


def test_board_debug_output_uses_the_canonical_color_order() -> None:
    game = csplendor.Game(seed=0)
    game.board.bank = [1, 2, 3, 4, 5, 6]
    player = game.board.get_player(0)
    player.gems = [6, 5, 4, 3, 2, 1]
    player.bonuses = [1, 2, 3, 4, 5]
    game.board.set_player(0, player)

    rendered = repr(game.board)
    assert "Bank: [D:1 S:2 E:3 R:4 O:5 G:6]" in rendered
    assert "Gems: [D:6 S:5 E:4 R:3 O:2 G:1]" in rendered
    assert "Bonuses: [D:1 S:2 E:3 R:4 O:5]" in rendered
