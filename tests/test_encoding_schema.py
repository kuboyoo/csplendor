from __future__ import annotations

import numpy as np

import csplendor as cs


def _legacy_card(card_id: int) -> np.ndarray:
    result = np.zeros(8, dtype=np.float32)
    if card_id == -1:
        return result
    card = cs.get_card(card_id)
    result[0] = card.points / 5.0
    for color in range(5):
        result[1 + color] = card.cost[color] / 7.0
    result[6] = int(card.bonus) / 5.0
    result[7] = card.level / 3.0
    return result


def _legacy_noble(noble_id: int) -> np.ndarray:
    result = np.zeros(6, dtype=np.float32)
    noble = cs.get_noble(noble_id)
    result[0] = noble.points / 3.0
    for color in range(5):
        result[1 + color] = noble.requirement[color] / 4.0
    return result


def _legacy_featurize(game: cs.Game, observer: int) -> np.ndarray:
    board = game.board
    parts = [np.asarray(board.bank, dtype=np.float32) / 7.0]
    for player_index, player in enumerate(board.players):
        reserved = []
        for slot, card_id in enumerate(player.reserved):
            hidden = (
                observer != -1
                and player_index != observer
                and player.reserved_is_hidden[slot]
            )
            if hidden:
                card = cs.get_card(card_id)
                feature = np.zeros(8, dtype=np.float32)
                feature[7] = card.level / 3.0
                reserved.append(feature)
            else:
                reserved.append(_legacy_card(card_id))
        parts.append(
            np.concatenate(
                [
                    np.asarray(player.gems, dtype=np.float32) / 10.0,
                    np.asarray(player.bonuses, dtype=np.float32) / 10.0,
                    np.asarray([player.points], dtype=np.float32) / 15.0,
                    *reserved,
                ]
            )
        )
    parts.extend(
        _legacy_card(board.visible[level][slot])
        for level in range(3)
        for slot in range(4)
    )
    parts.append(
        np.asarray([len(deck) for deck in board.decks], dtype=np.float32) / 40.0
    )
    parts.extend(
        _legacy_noble(board.nobles[slot])
        if slot < len(board.nobles)
        else np.zeros(6, dtype=np.float32)
        for slot in range(3)
    )
    parts.append(np.asarray([board.current_player], dtype=np.float32))
    return np.concatenate(parts)


def test_versioned_action_schema_descriptors_are_contiguous_and_frozen():
    expected = {
        cs.ActionEncoderCpp: (
            1,
            48,
            [10, 5, 12, 3, 12, 3, 3],
        ),
        cs.ActionEncoderV2: (
            2,
            4869,
            [840, 140, 84, 21, 3024, 756, 3, 1],
        ),
        cs.ActionEncoderV3: (
            3,
            3133,
            [840, 140, 84, 21, 2035, 12, 1],
        ),
    }
    for encoder, (version, size, section_sizes) in expected.items():
        sections = encoder.schema_sections()
        assert encoder.schema_version() == version
        assert [section["size"] for section in sections] == section_sizes
        offset = 0
        for section in sections:
            assert section["offset"] == offset
            offset += section["size"]
        assert offset == size
        public_size = getattr(encoder, "BASE_ACTION_COUNT", None)
        if public_size is None:
            public_size = encoder.ACTION_SIZE
        assert public_size == size
        assert f"v{version};size={size}" in encoder.schema_fingerprint()


def test_state_schema_descriptor_owns_shape_sections_and_color_order():
    assert cs.StateEncoder.schema_version() == 1
    assert cs.StateEncoder.feature_shape() == [196]
    assert cs.StateEncoder.feature_size() == 196
    assert cs.StateEncoder.card_feature_size() == 8
    assert cs.StateEncoder.noble_feature_size() == 6
    assert cs.StateEncoder.player_feature_size() == 36
    assert cs.StateFeaturizer.CARD_FEATURE_SIZE == 8
    assert cs.StateFeaturizer.NOBLE_FEATURE_SIZE == 6
    assert cs.StateEncoder.gem_color_ids() == [0, 1, 2, 3, 4, 5]
    assert cs.StateEncoder.gem_color_names() == [
        "diamond",
        "sapphire",
        "emerald",
        "ruby",
        "onyx",
        "gold",
    ]
    assert [
        (section["name"], section["offset"], section["size"])
        for section in cs.StateEncoder.schema_sections()
    ] == [
        ("bank", 0, 6),
        ("player_0", 6, 36),
        ("player_1", 42, 36),
        ("visible_cards", 78, 96),
        ("deck_counts", 174, 3),
        ("nobles", 177, 18),
        ("current_player", 195, 1),
    ]


def test_native_state_wrapper_matches_independent_legacy_reference():
    featurizer = cs.StateFeaturizer()
    checked = 0
    for seed in range(8):
        game = cs.Game(seed)
        for ply in range(32):
            for observer in (-1, 0, 1):
                np.testing.assert_array_equal(
                    featurizer.featurize(game, observer),
                    _legacy_featurize(game, observer),
                )
                checked += 1
            codes = game.legal_action_codes
            if not codes:
                break
            selected = (seed * 37 + ply * 11) % len(codes)
            assert game.apply_action_code_trusted(codes[selected], False)
    assert checked > 700

    editor = cs.Game(99)
    editor.board.bank = [1, 2, 3, 4, 0, 5]
    player = editor.board.players[1]
    player.gems = [5, 4, 3, 2, 1, 0]
    player.bonuses = [0, 1, 2, 3, 4]
    player.points = 14
    player.reserved = [0, 40, 70]
    player.reserved_is_hidden = [True, False, True]
    editor.board.set_player(1, player)
    np.testing.assert_array_equal(
        featurizer.featurize(editor, 0), _legacy_featurize(editor, 0)
    )


def test_python_action_wrapper_preserves_invalid_fallbacks():
    game = cs.Game(42)
    encoder = cs.ActionEncoder()
    offsets = {
        section["name"]: section["offset"]
        for section in cs.ActionEncoderCpp.schema_sections()
    }

    action = cs.Action()
    action.type = cs.ActionType.TAKE_DIFFERENT
    action.take = [1, 1, 1, 1, 0]
    assert encoder.encode(action, game) == offsets["take_different"]

    action = cs.Action()
    action.type = cs.ActionType.TAKE_SAME
    assert encoder.encode(action, game) == offsets["take_same"]

    action.type = cs.ActionType.RESERVE_VISIBLE
    assert encoder.encode(action, game) == offsets["reserve_visible"]

    action.type = cs.ActionType.RESERVE_DECK
    action.deck_level = -1
    assert encoder.encode(action, game) == offsets["reserve_deck"] - 1

    action.type = cs.ActionType.PURCHASE
    action.from_reserved = False
    assert encoder.encode(action, game) == offsets["purchase_visible"]
    action.from_reserved = True
    assert encoder.encode(action, game) == offsets["purchase_reserved"]

    action.type = cs.ActionType.VISIT_NOBLE
    assert encoder.encode(action, game) == offsets["visit_noble"]
    assert encoder.decode(-1, game) is None
    assert encoder.decode(48, game) is None
