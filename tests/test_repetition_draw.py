"""Repetition draw: three passive turns by each player end the game.

A passive turn is a PASS or a token take that returns exactly the taken
tokens (``Action.is_token_noop``). Six consecutive passive turns draw the game
with ``GameEndReason.REPETITION`` (doc/engine_specs.md, 千日手).
"""

from __future__ import annotations

import pytest

import csplendor as cs
from csplendor.api.game_presenter import get_game_state
from csplendor.api.game_service import GameSessionService
from csplendor.api.usi_kifu import action_to_usi
from tests.test_forced_pass import _forced_pass_game

REPETITION_TURNS = 6

# Version 2 snapshots written before the rule existed (2026-10-07 main).
V2_MIDGAME = bytes.fromhex(
    "4353504c534e5000020002005a000c0001b7ca4404d4b5d49500000000010b00000000ff01000003020511240b063f2e2f424d5649471e181221221d1b0310230c1409000e1c07040a251f01080d151a1e05270f1718402938442b433d33362c30452841393e3532373b3a2d2a340e4e4c4f4a554b5848504659525453030a0401000002010100000101000100205751000000030303260213000304020001000100000000003c3116010100030101190008f2b4add3adc93a"
)
V2_TERMINAL = bytes.fromhex(
    "4353504c534e5000020002005a000c0001b7ca4404d4b5d4980000000000200001000000040404040405091a24113d2c2b294c564e51031c0325133935453e31322e2f303f3c373b4344342d38330c544f5847554a4952504d4b59010500000000000004040603030f4853ff000000021414230205121b0116401857211f42190f0b0e284117010700000000000003050404050b2affff0000000115151e14081d2010360c0722270d3a0a061326041546000106c22a0c1ee3f83f64"
)


def _shuffle_game() -> cs.Game:
    """Both players hold ten tokens; the bank holds one white and one red.

    Each player's only passive move takes white and red and returns them.
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


def _progress(game: cs.Game) -> cs.Action:
    return next(a for a in game.legal_actions if a.type == cs.ActionType.RESERVE_VISIBLE)


def test_six_passive_turns_draw_by_repetition():
    game = _shuffle_game()
    assert game.board.passive_streak == 0
    assert game.board.end_reason == cs.GameEndReason.NONE
    for turn in range(1, REPETITION_TURNS):
        assert game.apply(_passive(game), True)
        assert game.board.passive_streak == turn
        assert not game.is_game_over()
    assert game.apply(_passive(game), True)
    assert game.is_game_over()
    assert game.winner == -2
    assert game.board.passive_streak == REPETITION_TURNS
    assert game.board.end_reason == cs.GameEndReason.REPETITION
    assert game.legal_actions == []

    assert game.undo()
    assert not game.is_game_over()
    assert game.board.passive_streak == REPETITION_TURNS - 1
    assert game.board.end_reason == cs.GameEndReason.NONE


def test_trusted_application_sees_the_same_terminal():
    game = _shuffle_game()
    for _ in range(REPETITION_TURNS):
        assert game.apply_trusted(_passive(game), False)
    assert game.board.end_reason == cs.GameEndReason.REPETITION


def test_a_turn_with_progress_resets_the_streak():
    game = _shuffle_game()
    for _ in range(REPETITION_TURNS - 1):
        game.apply(_passive(game), True)
    assert game.apply(_progress(game), True)
    assert game.board.passive_streak == 0
    assert not game.is_game_over()


def test_a_returning_take_of_other_colours_is_not_passive():
    game = _shuffle_game()
    game.apply(_passive(game), True)
    other = next(
        a for a in game.legal_actions
        if a.type in (cs.ActionType.TAKE_DIFFERENT, cs.ActionType.TAKE_SAME)
        and not a.is_token_noop()
    )
    assert game.apply(other, True)
    assert game.board.passive_streak == 0


def test_a_noble_gained_after_a_passive_take_resets_the_streak():
    game = _shuffle_game()
    noble = cs.get_noble(int(game.board.nobles[0]))
    player = game.board.get_player(0)
    player.bonuses = [int(v) for v in noble.requirement]
    game.board.set_player(0, player)
    game.board.passive_streak = 3
    assert game.apply(_passive(game), True)
    if game.board.waiting_noble:  # several eligible nobles: choose one
        assert game.board.passive_streak == 4
        assert game.apply(game.legal_actions[0], True)
    assert len(game.board.get_player(0).acquired_nobles) == 1
    assert game.board.passive_streak == 0


def test_a_forced_pass_is_passive_and_a_two_sided_one_is_a_stalemate():
    game = _forced_pass_game()
    game.board.passive_streak = 2
    assert game.apply_forced_pass(True)
    assert game.board.passive_streak == 3
    assert not game.is_game_over()

    stalemate = _forced_pass_game()
    visible, decks = stalemate.board.visible, stalemate.board.decks
    visible[0][0], decks[0][decks[0].index(22)] = 22, visible[0][0]
    stalemate.board.visible, stalemate.board.decks = visible, decks
    stalemate.board.passive_streak = REPETITION_TURNS - 1
    assert stalemate.apply_forced_pass(True)
    assert stalemate.winner == -2
    # Stalemate takes precedence over the repetition it would also complete.
    assert stalemate.board.end_reason == cs.GameEndReason.STALEMATE


def test_a_normal_finish_is_reported_as_normal():
    snapshot_game = cs.Game.deserialize_snapshot(V2_TERMINAL)
    assert snapshot_game.is_game_over()
    assert snapshot_game.board.end_reason == cs.GameEndReason.NORMAL


def test_streak_is_part_of_the_exact_and_observable_hash_only():
    one = _shuffle_game()
    two = _shuffle_game()
    two.board.passive_streak = 4
    assert one.board_hash() != two.board_hash()
    assert one.board.observable_hash(0) != two.board.observable_hash(0)
    assert one.board.observable_repetition_hash(0) == two.board.observable_repetition_hash(0)


def test_snapshot_round_trips_streak_and_reason():
    game = _shuffle_game()
    for _ in range(3):
        game.apply(_passive(game), True)
    restored = cs.Game.deserialize_snapshot(game.serialize_snapshot())
    assert restored.board.passive_streak == 3
    assert restored.board_hash() == game.board_hash()
    for _ in range(3):
        game.apply(_passive(game), True)
    final = cs.Game.deserialize_snapshot(game.serialize_snapshot())
    assert final.board.end_reason == cs.GameEndReason.REPETITION
    assert final.serialize_snapshot()[8] == 3  # format version byte


def test_version_2_snapshots_still_load_with_a_zero_streak():
    game = cs.Game.deserialize_snapshot(V2_MIDGAME)
    assert game.board.passive_streak == 0
    assert game.board.end_reason == cs.GameEndReason.NONE
    assert game.serialize_snapshot()[8] == 3


@pytest.mark.parametrize("streak", [-1, 7])
def test_out_of_range_streak_is_rejected(streak):
    game = _shuffle_game()
    with pytest.raises((ValueError, TypeError)):
        game.board.passive_streak = streak


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
    for _ in range(REPETITION_TURNS):
        service.apply_usi(session, action_to_usi(_passive(game), game=game))
    assert service.result_from_game(game) == "DRAW"
    assert service.result_detail_from_game(game) == "REPETITION"
    assert "\nResult: DRAW REPETITION\n" in service.build_kifu_text(session)
    board = get_game_state(game).board
    assert (board.passive_streak, board.end_reason) == (REPETITION_TURNS, 3)
