"""Repetition draw (千日手): a position occurring for the fourth time draws.

The position is complete (bank, cards, decks, nobles, both players' holdings,
the player to move and any pending decision); the occurrences need not be
consecutive. The game ends with winner -2 and ``GameEndReason.REPETITION``
(doc/engine_specs.md, 千日手).
"""

from __future__ import annotations

import pytest

import csplendor as cs
from csplendor.api.game_presenter import get_game_state
from csplendor.api.game_service import GameSessionService
from csplendor.api.usi_kifu import action_to_usi
from tests.test_forced_pass import _forced_pass_game

DRAW_COUNT = 4
# Both players exchanging without change: the start position recurs every
# second ply, so the fourth occurrence is at ply 6.
PASSIVE_DRAW_PLY = 6

# Version 2 snapshots written before the rule existed (2026-10-07 main).
V2_MIDGAME = bytes.fromhex(
    "4353504c534e5000020002005a000c0001b7ca4404d4b5d49500000000010b00000000ff01000003020511240b063f2e2f424d5649471e181221221d1b0310230c1409000e1c07040a251f01080d151a1e05270f1718402938442b433d33362c30452841393e3532373b3a2d2a340e4e4c4f4a554b5848504659525453030a0401000002010100000101000100205751000000030303260213000304020001000100000000003c3116010100030101190008f2b4add3adc93a"
)

# Version 3 snapshots of the passive-streak rule (2026-10-14 main): the
# exchange position below after three passive turns, and after six (a draw).
V3_STREAK_3 = bytes.fromhex(
    "4353504c534e5000030003005a000c0001b7ca4404d4b5d49d00000000010100000000ff03000100000100032208020e28292b2e4d46574f241d201b270b061c1e1223172509000d102405150c0f1f032111180a26131607141a0119041a402f32343736453b3d3a2c383c3130414435432d423e392a3f33104b514c524e485559585654475053494a03020504030302010001000000000000ffffff00000000000000000102020401000000000000ffffff0000000000000065fa3472bc531dc2"
)
V3_REPETITION = bytes.fromhex(
    "4353504c534e5000030003005a000c0001b7ca4404d4b5d49d00000000000300000000fe06030100000100032208020e28292b2e4d46574f241d201b270b061c1e1223172509000d102405150c0f1f032111180a26131607141a0119041a402f32343736453b3d3a2c383c3130414435432d423e392a3f33104b514c524e485559585654475053494a03020504030302010001000000000000ffffff00000000000000000102020401000000000000ffffff00000000000000fb7ecacf22e9e8b0"
)


def _shuffle_game() -> cs.Game:
    """Both players hold ten tokens; the bank holds one white and one red.

    Each player's moves take white and red and return two tokens.
    """
    game = cs.Game(seed=3)
    board = game.board
    board.bank = [1, 0, 0, 1, 0, 3]
    for index, gems in ((0, [3, 3, 2, 1, 0, 1]), (1, [0, 1, 2, 2, 4, 1])):
        player = board.get_player(index)
        player.gems = gems
        board.set_player(index, player)
    return game


def _passive(game: cs.Game) -> cs.Action:
    return next(a for a in game.legal_actions if a.is_token_noop())


def _take_to(game: cs.Game, gems: list[int]) -> cs.Action:
    """The take after which the player to move holds ``gems``."""
    actor = game.current_player
    for action in game.legal_actions:
        if action.type not in (cs.ActionType.TAKE_DIFFERENT, cs.ActionType.TAKE_SAME):
            continue
        trial = game.clone_light()
        assert trial.apply(action, False)
        if list(trial.board.get_player(actor).gems) == gems:
            return action
    raise AssertionError(f"no take reaches {gems}")


def _progress(game: cs.Game) -> cs.Action:
    return next(a for a in game.legal_actions if a.type == cs.ActionType.RESERVE_VISIBLE)


def test_the_fourth_occurrence_of_a_position_draws():
    game = _shuffle_game()
    assert game.board.repetition_keys == []
    assert game.board.repetition_count == 1
    assert game.board.end_reason == cs.GameEndReason.NONE
    counts = []
    for _ in range(PASSIVE_DRAW_PLY - 1):
        assert game.apply(_passive(game), True)
        counts.append(game.board.repetition_count)
        assert not game.is_game_over()
    # Plies 1..5 alternate between the two positions of the exchange.
    assert counts == [1, 2, 2, 3, 3]
    assert game.apply(_passive(game), True)
    assert game.is_game_over()
    assert game.winner == -2
    assert game.board.repetition_count == DRAW_COUNT == cs.Board.REPETITION_DRAW_COUNT
    assert len(game.board.repetition_keys) == PASSIVE_DRAW_PLY + 1
    assert game.board.end_reason == cs.GameEndReason.REPETITION
    assert game.legal_actions == []

    assert game.undo()
    assert not game.is_game_over()
    assert game.board.repetition_count == 3
    assert game.board.end_reason == cs.GameEndReason.NONE


def test_non_consecutive_repetition_with_different_returns_draws():
    """The stuck self-play games: one player exchanges different colours.

    White takes white and red and returns white and blue, the opponent
    exchanges without change, White takes them back; the start position
    recurs every fourth ply and the fourth occurrence (ply 12) draws.
    """
    game = _shuffle_game()
    start = game.board.repetition_position_key()
    cycle = [
        lambda g: _take_to(g, [3, 2, 2, 2, 0, 1]),
        _passive,
        lambda g: _take_to(g, [3, 3, 2, 1, 0, 1]),
        _passive,
    ]
    for ply in range(1, 13):
        action = cycle[(ply - 1) % 4](game)
        assert not action.is_token_noop() or ply % 2 == 0
        assert game.apply(action, True)
        assert game.is_game_over() == (ply == 12)
    assert game.board.repetition_position_key() == start
    assert game.board.repetition_keys.count(start) == DRAW_COUNT
    assert game.board.end_reason == cs.GameEndReason.REPETITION


def test_trusted_application_sees_the_same_terminal():
    game = _shuffle_game()
    for _ in range(PASSIVE_DRAW_PLY):
        assert game.apply_trusted(_passive(game), False)
    assert game.board.end_reason == cs.GameEndReason.REPETITION


def test_a_card_decision_restarts_the_window():
    game = _shuffle_game()
    for _ in range(PASSIVE_DRAW_PLY - 1):
        game.apply(_passive(game), True)
    assert game.apply(_progress(game), True)
    assert game.board.repetition_keys == []
    assert game.board.repetition_count == 1
    assert not game.is_game_over()


def test_a_take_that_adds_tokens_restarts_the_window():
    # Holdings never fall outside a pending return, so a position with fewer
    # tokens cannot recur: the window only keeps positions of equal totals.
    game = cs.Game(seed=3)
    take = next(a for a in game.legal_actions if a.type == cs.ActionType.TAKE_DIFFERENT)
    assert game.apply(take, True)
    assert game.board.repetition_keys == []

    shuffled = _shuffle_game()
    assert shuffled.apply(_passive(shuffled), True)
    assert len(shuffled.board.repetition_keys) == 2


def test_a_noble_gained_after_an_exchange_restarts_the_window():
    game = _shuffle_game()
    noble = cs.get_noble(int(game.board.nobles[0]))
    player = game.board.get_player(0)
    player.bonuses = [int(v) for v in noble.requirement]
    game.board.set_player(0, player)
    assert game.apply(_passive(game), True)
    if game.board.waiting_noble:  # several eligible nobles: choose one
        assert len(game.board.repetition_keys) == 2
        assert game.apply(game.legal_actions[0], True)
    assert len(game.board.get_player(0).acquired_nobles) == 1
    assert game.board.repetition_keys == []


def test_a_forced_pass_extends_the_window_and_a_two_sided_one_is_a_stalemate():
    game = _forced_pass_game()
    assert game.apply_forced_pass(True)
    assert len(game.board.repetition_keys) == 2
    assert not game.is_game_over()

    stalemate = _forced_pass_game()
    visible, decks = stalemate.board.visible, stalemate.board.decks
    visible[0][0], decks[0][decks[0].index(22)] = 22, visible[0][0]
    stalemate.board.visible, stalemate.board.decks = visible, decks
    # The position after the pass has occurred three times already.
    current = stalemate.board.repetition_position_key()
    after_pass = current ^ 0b100  # the player-to-move bit
    stalemate.board.repetition_keys = [after_pass, current] * 3
    assert stalemate.apply_forced_pass(True)
    assert stalemate.winner == -2
    # Stalemate takes precedence over the repetition it would also complete.
    assert stalemate.board.repetition_count == DRAW_COUNT
    assert stalemate.board.end_reason == cs.GameEndReason.STALEMATE


def test_a_normal_finish_is_reported_as_normal():
    game = cs.Game(seed=4)
    while not game.is_game_over():
        # Buying whenever possible ends the game through the final round.
        actions = game.legal_actions
        buys = [a for a in actions if a.type == cs.ActionType.PURCHASE]
        assert game.apply((buys or actions)[0], True)
    assert game.board.end_reason == cs.GameEndReason.NORMAL
    restored = cs.Game.deserialize_snapshot(game.serialize_snapshot())
    assert restored.is_game_over()
    assert restored.board.end_reason == cs.GameEndReason.NORMAL


def test_window_is_part_of_the_exact_and_observable_hash_only():
    one = _shuffle_game()
    two = _shuffle_game()
    current = two.board.repetition_position_key()
    two.board.repetition_keys = [current, current]
    assert one.board_hash() != two.board_hash()
    assert one.board.observable_hash(0) != two.board.observable_hash(0)
    assert one.board.observable_repetition_hash(0) == two.board.observable_repetition_hash(0)


def test_the_window_is_a_multiset_for_hashing():
    game = _shuffle_game()
    first = game.board.repetition_position_key()
    game.apply(_passive(game), True)
    game.apply(_passive(game), True)
    other = game.clone_light()
    keys = list(game.board.repetition_keys)
    other.board.repetition_keys = [keys[1], keys[0], keys[2]]
    assert keys[0] == keys[2] == first
    assert other.board_hash() == game.board_hash()


def test_snapshot_round_trips_window_and_reason():
    game = _shuffle_game()
    for _ in range(3):
        game.apply(_passive(game), True)
    restored = cs.Game.deserialize_snapshot(game.serialize_snapshot())
    assert restored.board.repetition_keys == game.board.repetition_keys
    assert restored.board.repetition_count == 2
    assert restored.board_hash() == game.board_hash()
    for _ in range(3):
        game.apply(_passive(game), True)
        restored.apply(_passive(restored), True)
    final = cs.Game.deserialize_snapshot(game.serialize_snapshot())
    assert restored.board.end_reason == cs.GameEndReason.REPETITION
    assert final.board.end_reason == cs.GameEndReason.REPETITION
    assert final.serialize_snapshot()[8] == 5  # format version byte


@pytest.mark.parametrize("snapshot", [V2_MIDGAME, V3_STREAK_3, V3_REPETITION])
def test_older_snapshots_are_rejected(snapshot):
    with pytest.raises(ValueError, match="unsupported csplendor game snapshot version"):
        cs.Game.deserialize_snapshot(snapshot)


def test_invalid_windows_are_rejected():
    game = _shuffle_game()
    current = game.board.repetition_position_key()
    for keys in ([current], [current, current + 8], [current + 8, current], [1 << 63, current]):
        with pytest.raises((ValueError, TypeError)):
            game.board.repetition_keys = keys
    game.board.repetition_keys = [current, current]
    player = game.board.get_player(0)
    game.board.set_player(0, player)  # any other edit restarts the window
    assert game.board.repetition_keys == []


def test_kifu_result_names_the_draw_reason():
    service = GameSessionService({}, {}, id_factory=lambda: "s")
    session = service.create_game(
        seed=3, simple_payment_mode=True, player0_name="A", player1_name="B"
    )
    game = service.require_game(session)
    shuffled = _shuffle_game()
    game.board.bank = list(shuffled.board.bank)
    for index in range(2):
        game.board.set_player(index, shuffled.board.get_player(index))
    for _ in range(PASSIVE_DRAW_PLY):
        service.apply_usi(session, action_to_usi(_passive(game), game=game))
    assert service.result_from_game(game) == "DRAW"
    assert service.result_detail_from_game(game) == "REPETITION"
    assert "\nResult: DRAW REPETITION\n" in service.build_kifu_text(session)
    board = get_game_state(game).board
    assert (board.repetition_count, board.end_reason) == (DRAW_COUNT, 3)
