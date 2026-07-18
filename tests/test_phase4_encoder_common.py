import pytest

from csplendor import Action, ActionEncoderV2, ActionEncoderV3, ActionType, Game


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


@pytest.mark.parametrize("pattern", range(84))
def test_v2_v3_return_codec_ids_and_round_trips_are_stable(pattern):
    game = Game(seed=42)
    v2 = ActionEncoderV2.decode(pattern, game)
    v3 = ActionEncoderV3.decode(pattern, game)

    assert _action_signature(v2) == _action_signature(v3)
    assert ActionEncoderV2.encode(v2, game) == pattern
    assert ActionEncoderV3.encode(v3, game) == pattern


@pytest.mark.parametrize("combo", range(10))
def test_v2_v3_take_different_combination_ids_are_stable(combo):
    game = Game(seed=42)
    action_id = combo * ActionEncoderV2.TAKE_DIFF_RETURN_PATTERNS
    v2 = ActionEncoderV2.decode(action_id, game)
    v3 = ActionEncoderV3.decode(action_id, game)

    assert _action_signature(v2) == _action_signature(v3)
    assert ActionEncoderV2.encode(v2, game) == action_id
    assert ActionEncoderV3.encode(v3, game) == action_id


def test_v2_purchase_slot_ids_change_while_v3_card_ids_do_not():
    game = Game(seed=42)
    action = Action()
    action.type = ActionType.PURCHASE
    action.card_id = int(game.board.visible[0][0])
    action.from_reserved = False
    before_v2 = ActionEncoderV2.encode(action, game)
    before_v3 = ActionEncoderV3.encode(action, game)

    moved = game.clone_light()
    visible = moved.board.visible
    source = next(
        index
        for index, card_id in enumerate(card for level in visible for card in level)
        if card_id == action.card_id
    )
    target = (source + 1) % 12
    source_level, source_slot = divmod(source, 4)
    target_level, target_slot = divmod(target, 4)
    visible[source_level][source_slot], visible[target_level][target_slot] = (
        visible[target_level][target_slot],
        visible[source_level][source_slot],
    )
    moved.board.visible = visible

    assert ActionEncoderV2.encode(action, moved) != before_v2
    assert ActionEncoderV3.encode(action, moved) == before_v3


def test_v2_reserved_slot_ids_change_while_v3_card_ids_do_not():
    game = Game(seed=42)
    card_id = int(game.board.visible[0][0])
    player = game.board.get_player(0)
    player.reserved = [card_id, -1, -1]
    game.board.set_player(0, player)

    action = Action()
    action.type = ActionType.PURCHASE
    action.card_id = card_id
    action.from_reserved = True
    before_v2 = ActionEncoderV2.encode(action, game)
    before_v3 = ActionEncoderV3.encode(action, game)

    moved = game.clone_light()
    player = moved.board.get_player(0)
    player.reserved = [-1, card_id, -1]
    moved.board.set_player(0, player)

    assert ActionEncoderV2.encode(action, moved) != before_v2
    assert ActionEncoderV3.encode(action, moved) == before_v3
