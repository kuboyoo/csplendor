import pytest

from csplendor import Game


def _board_editor_signature(board):
    return (
        tuple(tuple(int(card_id) for card_id in level) for level in board.visible),
        tuple(tuple(int(card_id) for card_id in level) for level in board.decks),
        tuple(int(noble_id) for noble_id in board.nobles),
    )


@pytest.mark.parametrize("field", ["visible", "nobles", "decks"])
def test_invalid_board_collection_setter_is_atomic_and_preserves_hash(field):
    game = Game(seed=42)
    board = game.board
    before_signature = _board_editor_signature(board)
    before_hash = int(board.hash())

    if field == "visible":
        payload = [list(level) for level in board.visible]
        payload[0][0] = -1
        payload[2][3] = 90
    elif field == "nobles":
        payload = list(board.nobles)
        payload[0] = (int(payload[0]) + 1) % 12
        payload[-1] = 12
    else:
        payload = [list(level) for level in board.decks]
        payload[0][0], payload[0][1] = payload[0][1], payload[0][0]
        payload[2][-1] = 0  # A valid card ID, but invalid in the level-3 deck.

    with pytest.raises(ValueError):
        setattr(board, field, payload)

    assert _board_editor_signature(board) == before_signature
    assert int(board.hash()) == before_hash


def test_full_and_observable_hash_distinguish_noble_slot_order():
    game = Game(seed=42)
    board = game.board
    original = list(board.nobles)
    reordered = list(original)
    reordered[0], reordered[1] = reordered[1], reordered[0]

    full_before = int(board.hash())
    observable_before = int(board.observable_hash(0))
    board.nobles = reordered

    assert int(board.hash()) != full_before
    assert int(board.observable_hash(0)) != observable_before


def test_noble_hash_does_not_cancel_duplicate_ids_across_slots():
    game = Game(seed=42)
    board = game.board
    board.nobles = []
    empty_full = int(board.hash())
    empty_observable = int(board.observable_hash(0))

    board.nobles = [0, 0]

    assert int(board.hash()) != empty_full
    assert int(board.observable_hash(0)) != empty_observable


@pytest.mark.parametrize(
    ("field", "lower", "upper"),
    [
        ("bank", 13, 14),
        ("gems", 13, 14),
        ("bonuses", 16, 17),
        ("reserved", 90, 91),
        ("reserved_count", 4, 5),
        ("purchased_count", 91, 92),
    ],
)
def test_hash_distinguishes_public_editor_values_outside_canonical_tables(
    field, lower, upper
):
    game = Game(seed=42)
    board = game.board

    def set_value(value):
        if field == "bank":
            bank = list(board.bank)
            bank[0] = value
            board.bank = bank
            return
        player = board.get_player(0)
        if field in ("gems", "bonuses", "reserved"):
            values = list(getattr(player, field))
            values[0] = value
            setattr(player, field, values)
        else:
            setattr(player, field, value)
        board.set_player(0, player)

    set_value(lower)
    lower_full = int(board.hash())
    lower_observable = int(board.observable_hash(0))
    set_value(upper)

    assert int(board.hash()) != lower_full
    assert int(board.observable_hash(0)) != lower_observable
