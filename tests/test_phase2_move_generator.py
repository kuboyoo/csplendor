"""Phase 2 contracts for the MoveGenerator emitter and capped consumers."""

import hashlib

import numpy as np
import pytest

from csplendor import (
    Action,
    ActionEncoderCpp,
    ActionEncoderV2,
    ActionEncoderV3,
    ActionType,
    Game,
    get_card,
    get_noble,
)


def _digest_ints(values):
    rendered = ",".join(str(int(value)) for value in values)
    return hashlib.sha256(rendered.encode()).hexdigest()


def _digest_mask(mask):
    return hashlib.sha256(np.asarray(mask, dtype=np.uint8).tobytes()).hexdigest()


def _action_signature(action):
    return (
        int(action.type),
        tuple(map(int, action.take)),
        int(action.card_id),
        int(action.deck_level),
        bool(action.from_reserved),
        tuple(map(int, action.gold_as)),
        tuple(map(int, action.return_gems)),
        int(action.noble_choice),
    )


def _set_player(game, *, gems=None, bonuses=None, reserved=None):
    player = game.board.players[game.current_player]
    if gems is not None:
        player.gems = list(gems)
    if bonuses is not None:
        player.bonuses = list(bonuses)
    if reserved is not None:
        player.reserved = list(reserved)
    game.board.set_player(game.current_player, player)


def _player_signature(player):
    return (
        int(player.points),
        tuple(map(int, player.gems)),
        tuple(map(int, player.bonuses)),
        tuple(map(int, player.reserved)),
        tuple(map(bool, player.reserved_is_hidden)),
        int(player.reserved_count),
        int(player.purchased_count),
        tuple(map(int, player.purchased_cards)),
        tuple(map(int, player.acquired_nobles)),
    )


def _board_signature(game):
    board = game.board
    return (
        tuple(map(int, board.bank)),
        tuple(tuple(map(int, level)) for level in board.visible),
        tuple(tuple(map(int, level)) for level in board.decks),
        tuple(map(int, board.nobles)),
        tuple(_player_signature(player) for player in board.players),
        int(board.turn),
        int(board.current_player),
        bool(board.final_round),
        bool(board.waiting_noble),
        int(board.winner),
    )


def _uncapped_exchange_prefix(game, limit):
    """Independently expand ordered take/reserve returns up to ``limit``."""
    player_gems = list(map(int, game.board.players[game.current_player].gems))
    codes = []

    for base in game.base_actions:
        if base.type not in (
            ActionType.TAKE_DIFFERENT,
            ActionType.TAKE_SAME,
            ActionType.RESERVE_VISIBLE,
            ActionType.RESERVE_DECK,
        ):
            continue

        next_gems = player_gems.copy()
        if base.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
            for color in range(5):
                next_gems[color] += int(base.take[color])
        elif game.board.bank[5] > 0:
            next_gems[5] += 1

        excess = max(0, sum(next_gems) - 10)
        if excess == 0:
            codes.append(int(base.pack()))
            if len(codes) >= limit:
                return codes
            continue

        returned = [0] * 6

        def visit(color, remaining):
            if remaining == 0:
                action = Action.unpack(int(base.pack()))
                action.return_gems = returned.copy()
                codes.append(int(action.pack()))
                return len(codes) < limit
            if color == 6:
                return True
            for amount in range(min(remaining, next_gems[color]) + 1):
                returned[color] = amount
                if not visit(color + 1, remaining - amount):
                    return False
            returned[color] = 0
            return True

        if not visit(0, excess):
            return codes

    return codes


def _assert_full_consumer_parity(game, expected_count, expected_digest):
    actions = game.legal_actions
    codes = list(map(int, game.legal_action_codes))
    assert [int(action.pack()) for action in actions] == codes
    assert game.legal_action_count == expected_count == len(codes)
    assert _digest_ints(codes) == expected_digest

    if codes:
        for index in (0, len(codes) // 2, len(codes) - 1):
            assert int(game.legal_action_code_at(index)) == codes[index]
    assert int(game.legal_action_code_at(len(codes))) == 0


@pytest.mark.parametrize(
    ("gems", "expected_count", "expected_digest"),
    [
        (
            [1, 1, 1, 1, 1, 0],
            30,
            "f15139b8be58bb3829aa1c7db39e4e69eb03b7ce27ffd8052f88e574d3db50e8",
        ),
        (
            [2, 2, 2, 1, 1, 0],
            70,
            "c07b22a2d95cb425196df96c6c8c2ef78981096a90b97f35c24eec48599654f0",
        ),
        (
            [2, 2, 2, 2, 1, 0],
            186,
            "def5ddd63e9e10316265ccc959b7709c58a96e40fa014de02558abcc1dd5adbb",
        ),
        (
            [2, 2, 2, 2, 2, 0],
            496,
            "93416ba04036ce67352655c84d9bf56f6bb873c6636794f1ce39ca5fb956babe",
        ),
    ],
)
def test_return_expansion_order_is_golden(gems, expected_count, expected_digest):
    game = Game(seed=42)
    _set_player(game, gems=gems)
    _assert_full_consumer_parity(game, expected_count, expected_digest)


@pytest.mark.parametrize(
    (
        "simple_payment_mode",
        "expected_count",
        "expected_digest",
        "expected_base_count",
        "expected_base_digest",
        "expected_v2_mask_digest",
        "expected_v3_mask_digest",
    ),
    [
        (
            False,
            647,
            "d1e5988132c881cd0d1a968476349347137d289dc9f6c4a26cecf9bdb18f6d8d",
            37,
            "a50cb74b62aec86fa1ad80f2078fe0ce2454ba1651933755ea16cd67a1db94e2",
            "3f2ed95426352755d91be0e9ce594395ed8945e6eb3ec760f9759ea3d97f3bb2",
            "d7556294542ea00581b879d8190582462c0708feb603edb68f3d169781e24292",
        ),
        (
            True,
            643,
            "69d31bec388cd7699b2e67eb7bfc272c5923ad427f54478bc19312084e253ee2",
            33,
            "dabd092b2f7e6655fc640e6a2ebdfc2e81118884ce4ff54cdaf8e495246c28ac",
            "802ee633cbf3061a0ca7645db925a1bf3bb268348550e291a462ff6ba6fedc17",
            "db2d577e5d4ad472e3aae21af5d19a2f3c613aa364bc210431f5775394abc922",
        ),
    ],
)
def test_payment_base_and_encoder_order_is_golden(
    simple_payment_mode,
    expected_count,
    expected_digest,
    expected_base_count,
    expected_base_digest,
    expected_v2_mask_digest,
    expected_v3_mask_digest,
):
    game = Game(seed=42)
    _set_player(game, gems=[2, 2, 2, 2, 1, 1], reserved=[5, -1, -1])
    game.simple_payment_mode = simple_payment_mode

    _assert_full_consumer_parity(game, expected_count, expected_digest)
    base_codes = [int(action.pack()) for action in game.base_actions]
    assert len(base_codes) == expected_base_count
    assert _digest_ints(base_codes) == expected_base_digest

    cpp_mask = np.asarray(ActionEncoderCpp.get_action_mask(game), dtype=np.uint8)
    scored_mask, scores = ActionEncoderCpp.get_action_mask_with_scores(game)
    np.testing.assert_array_equal(scored_mask, cpp_mask)
    assert _digest_mask(cpp_mask) == (
        "3accc04aed17f72d2d7d65d312897160da067cd886be2095b4cdd5756c118da8"
    )

    expected_scores = np.zeros(ActionEncoderCpp.BASE_ACTION_COUNT, dtype=np.float32)
    player = game.board.players[game.current_player]
    for action in game.legal_actions:
        action_id = ActionEncoderCpp.encode(action, game)
        if action_id < 0:
            continue
        if action.type == ActionType.PURCHASE:
            card = get_card(int(action.card_id))
            total_cost = sum(int(value) for value in card.cost)
            shortage = sum(
                max(
                    0,
                    max(0, int(card.cost[color]) - int(player.bonuses[color]))
                    - int(player.gems[color]),
                )
                for color in range(5)
            )
            cp = 100.0 if total_cost == 0 else (
                (int(card.points) * 5.0 + 1.0)
                / (0.5 * total_cost + 2.0 * shortage + 1.0)
            )
            expected_scores[action_id] = 1.0 + cp * 2.0
        elif action.type in (ActionType.RESERVE_VISIBLE, ActionType.RESERVE_DECK):
            expected_scores[action_id] = 0.5
        elif action.type in (ActionType.TAKE_DIFFERENT, ActionType.TAKE_SAME):
            expected_scores[action_id] = 0.2
        elif action.type == ActionType.VISIT_NOBLE:
            expected_scores[action_id] = 5.0
    np.testing.assert_allclose(scores, expected_scores, rtol=1e-6, atol=0.0)

    v2_mask = ActionEncoderV2.get_action_mask(game)
    v3_mask = ActionEncoderV3.get_action_mask(game)
    assert _digest_mask(v2_mask) == expected_v2_mask_digest
    assert _digest_mask(v3_mask) == expected_v3_mask_digest


def test_waiting_noble_order_and_uncapped_base_compatibility_are_golden():
    game = Game(seed=5)
    noble_ids = [int(noble_id) for noble_id in game.board.nobles[:2]]
    requirements = [get_noble(noble_id).requirement for noble_id in noble_ids]
    bonuses = [
        max(int(requirement[color]) for requirement in requirements)
        for color in range(5)
    ]
    _set_player(game, bonuses=bonuses)

    regular = next(
        action
        for action in game.legal_actions
        if action.type == ActionType.TAKE_DIFFERENT
    )
    assert game.apply_action_code_trusted(int(regular.pack()), False)
    assert game.board.waiting_noble is True

    _assert_full_consumer_parity(
        game,
        2,
        "aa5ae16c0bbb1395166828bad291271c665dda0919efae8c282263d4f07b8580",
    )
    assert [int(action.noble_choice) for action in game.legal_actions] == noble_ids

    cpp_ids = []
    for encoder in (ActionEncoderCpp, ActionEncoderV2, ActionEncoderV3):
        mask = np.asarray(encoder.get_action_mask(game), dtype=np.uint8)
        encoded = [encoder.encode(action, game) for action in game.legal_actions]
        assert np.flatnonzero(mask).tolist() == sorted(encoded)
        if encoder is ActionEncoderCpp:
            cpp_ids = encoded
        for action_id, action in zip(encoded, game.legal_actions):
            decoded = (
                encoder.decode(action_id, game)
                if encoder is ActionEncoderCpp
                else encoder.decode_and_match(action_id, game)
            )
            assert _action_signature(decoded) == _action_signature(action)

    score_mask, scores = ActionEncoderCpp.get_action_mask_with_scores(game)
    np.testing.assert_array_equal(
        score_mask, ActionEncoderCpp.get_action_mask(game)
    )
    assert all(scores[action_id] == pytest.approx(5.0) for action_id in cpp_ids)

    # This unusual public compatibility contract intentionally ignores the
    # waiting-noble state and still returns ordinary, unexpanded base actions.
    base_codes = [int(action.pack()) for action in game.base_actions]
    assert len(base_codes) == 31
    assert _digest_ints(base_codes) == (
        "78355dd547d7e8e9354181b7697a711a098ccb2ff2c102cb3b68c30955b0d666"
    )


def test_editor_boundary_keeps_the_existing_2048_action_prefix():
    game = Game(seed=42)
    _set_player(game, gems=[3, 3, 3, 3, 3, 3])

    _assert_full_consumer_parity(
        game,
        2048,
        "c64f458a2ba3ca506e76d6cc9f751d2cd8b9e980e621ff1fbdeb1ce9f9879904",
    )
    codes = list(map(int, game.legal_action_codes))
    uncapped_prefix = _uncapped_exchange_prefix(game, 2049)
    assert len(uncapped_prefix) == 2049
    assert uncapped_prefix[:2048] == codes
    # The public legal list remains capped, while semantic validation accepts
    # an otherwise valid action beyond that materialization boundary.
    assert game.is_legal(Action.unpack(uncapped_prefix[2048])) is True
    assert codes[:3] == [27485274280, 27453817000, 26950500520]
    assert codes[-3:] == [26378373160, 18325309480, 10272245800]

    actions = game.legal_actions
    for index in (0, 1024, 2047):
        assert game.is_legal(actions[index]) is True
        by_index = game.clone_light()
        by_random = game.clone_light()
        assert by_index.apply_legal_action_index(index, False)
        assert by_random.apply_random_action(index + 2048 * 3, False)
        assert _board_signature(by_random) == _board_signature(by_index)
    assert game.apply_legal_action_index(2048, False) is False

    base_codes = [int(action.pack()) for action in game.base_actions]
    assert len(base_codes) == 91
    assert _digest_ints(base_codes) == (
        "eae73ba015c46e69cb6b0fcabd30cd00cf4e0f24b9c0bd43a240f27883964a90"
    )

    assert _digest_mask(ActionEncoderCpp.get_action_mask(game)) == (
        "2cd31e44d7d56a2e1773aaac6915f915d1963f03ba225158ae7247bb48691e8a"
    )
    assert _digest_mask(ActionEncoderV2.get_action_mask(game)) == (
        # The capped editor actions need eight returned tokens and are outside
        # V2's finite return-pattern space. PASS is no longer used as a generic
        # policy fallback; it is enabled only for a legal forced pass.
        "c8efd835f2b27b732fe9fa88742a306e953c583eb5a55eb46dd60e64610b0dcb"
    )
    assert _digest_mask(ActionEncoderV3.get_action_mask(game)) == (
        "5d86f808a3ed971a92e640de044877d3c6c20fa9c2712e79ac3d34482e251c37"
    )


def test_uncapped_base_adapter_and_capped_full_adapter_stay_distinct():
    game = Game(seed=1)
    game.board.bank = [0, 0, 0, 0, 0, 0]
    game.board.visible = [[86, 86, 86, 86] for _ in range(3)]
    _set_player(
        game,
        gems=[5, 3, 0, 3, 3, 14],
        bonuses=[0, 0, 0, 0, 0],
        reserved=[86, 86, 86],
    )

    base_actions = game.base_actions
    full_actions = game.legal_actions
    assert len(base_actions) == 5760
    assert game.legal_action_count == len(full_actions) == 2048
    assert list(map(int, game.legal_action_codes)) == [
        int(action.pack()) for action in base_actions[:2048]
    ]
    assert all(tuple(map(int, action.return_gems)) == (0, 0, 0, 0, 0, 0)
               for action in full_actions)


def test_random_modulo_index_and_semantic_is_legal_contract_are_unchanged():
    game = Game(seed=42)
    _set_player(game, gems=[2, 2, 2, 2, 2, 0])
    codes = list(map(int, game.legal_action_codes))

    for index in range(len(codes)):
        expected = game.clone_light()
        actual = game.clone_light()
        assert expected.apply_legal_action_index(index, False)
        assert actual.apply_random_action(index + len(codes) * 7, False)
        assert _board_signature(actual) == _board_signature(expected)

    expected = game.clone_light()
    actual = game.clone_light()
    assert expected.apply_legal_action_index((2**64 - 1) % len(codes), False)
    assert actual.apply_random_action(2**64 - 1, False)
    assert _board_signature(actual) == _board_signature(expected)

    assert game.apply_legal_action_index(len(codes), False) is False

    canonical = Action.unpack(codes[0])
    noncanonical = Action.unpack(codes[0])
    noncanonical.card_id = 0  # Irrelevant to TAKE semantics.
    assert int(noncanonical.pack()) == int(canonical.pack())
    assert game.is_legal(canonical) is True
    assert game.is_legal(noncanonical) is True


def test_is_legal_ignores_irrelevant_fields_for_every_action_type():
    game = Game(seed=42)
    _set_player(game, gems=[2, 2, 2, 2, 1, 1], reserved=[5, -1, -1])
    by_type = {}
    for action in game.legal_actions:
        by_type.setdefault(action.type, action)

    for action_type in (
        ActionType.TAKE_DIFFERENT,
        ActionType.TAKE_SAME,
        ActionType.RESERVE_VISIBLE,
        ActionType.RESERVE_DECK,
        ActionType.PURCHASE,
    ):
        canonical = Action.unpack(int(by_type[action_type].pack()))
        noncanonical = Action.unpack(int(canonical.pack()))
        noncanonical.noble_choice = 0
        assert int(noncanonical.pack()) == int(canonical.pack())
        assert game.is_legal(canonical) is True
        assert game.is_legal(noncanonical) is True

    noble_game = Game(seed=5)
    noble_id = int(noble_game.board.nobles[0])
    _set_player(
        noble_game,
        bonuses=list(map(int, get_noble(noble_id).requirement)),
    )
    noble_game.board.waiting_noble = True
    noble = next(
        action
        for action in noble_game.legal_actions
        if action.type == ActionType.VISIT_NOBLE
    )
    canonical = Action.unpack(int(noble.pack()))
    noncanonical = Action.unpack(int(noble.pack()))
    noncanonical.card_id = 0
    assert int(noncanonical.pack()) == int(canonical.pack())
    assert noble_game.is_legal(canonical) is True
    assert noble_game.is_legal(noncanonical) is True


def test_early_stop_consumers_preserve_invalid_card_id_exceptions():
    clean_action = Action.unpack(int(Game(seed=42).legal_action_codes[0]))

    consumers = [
        lambda game: game.legal_actions,
        lambda game: game.base_actions,
        lambda game: game.legal_action_count,
        lambda game: game.legal_action_codes,
        lambda game: game.legal_action_code_at(0),
        lambda game: game.apply_legal_action_index(0, False),
        lambda game: game.apply_random_action(0, False),
        lambda game: ActionEncoderCpp.get_action_mask(game),
        lambda game: ActionEncoderCpp.get_action_mask_with_scores(game),
        lambda game: ActionEncoderV2.get_action_mask(game),
        lambda game: ActionEncoderV2.decode_and_match(0, game),
        lambda game: ActionEncoderV3.get_action_mask(game),
        lambda game: ActionEncoderV3.decode_and_match(0, game),
    ]

    for consume in consumers:
        game = Game(seed=42)
        # This public setter intentionally accepts any int8 card ID.
        _set_player(game, gems=[3, 3, 3, 3, 3, 3], reserved=[127, -1, -1])
        with pytest.raises(IndexError, match="card id out of range"):
            consume(game)

    # Semantic validation checks only fields relevant to the candidate action
    # and therefore does not traverse an unrelated malformed reserve. It must
    # still agree with apply() and leave the editor state unchanged.
    game = Game(seed=42)
    _set_player(game, gems=[3, 3, 3, 3, 3, 3], reserved=[127, -1, -1])
    before = _board_signature(game)
    assert game.is_legal(clean_action) is False
    assert game.apply(clean_action, False) is False
    assert _board_signature(game) == before


def test_depleted_bank_take_is_available_in_every_encoder():
    game = Game(seed=42)
    game.board.bank = [1, 0, 0, 0, 0, 0]
    _set_player(game, gems=[0, 0, 0, 0, 0, 0], reserved=[1, 2, 3])

    assert list(map(int, game.legal_action_codes)) == [8]
    action = game.legal_actions[0]
    assert ActionEncoderCpp.encode(action, game) == 0
    assert ActionEncoderV2.encode(action, game) == 0
    assert ActionEncoderV3.encode(action, game) == 0

    cpp_mask = np.asarray(ActionEncoderCpp.get_action_mask(game))
    v2_mask = np.asarray(ActionEncoderV2.get_action_mask(game))
    v3_mask = np.asarray(ActionEncoderV3.get_action_mask(game))
    assert np.flatnonzero(cpp_mask).tolist() == [0]
    assert np.flatnonzero(v2_mask).tolist() == [0]
    assert np.flatnonzero(v3_mask).tolist() == [0]

    for encoder in (ActionEncoderV2, ActionEncoderV3):
        matched = encoder.decode_and_match(0, game)
        assert _action_signature(matched) == _action_signature(action)
        assert game.is_legal(matched) is True
