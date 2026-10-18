"""Reservation token return phase across the public surfaces.

A reservation (board card or deck) that leaves the mover with eleven tokens is
followed by a separate RETURN_GEM decision of the same player, made after the
refilled board card or the drawn card is known.  These tests pin the USI, SPN,
API, V5 action-space, search-feature and solver contracts of that phase.
"""

import numpy as np
import pytest

import csplendor as cs
from csplendor.api.application_errors import InvalidRequest
from csplendor.api.game_presenter import get_game_state
from csplendor.api.game_service import GameSessionService
from csplendor.api.usi_kifu import (
    action_to_usi,
    expand_usi_move,
    find_legal_action_index_by_usi,
    game_to_spn,
    position_to_game,
    spn_to_game,
)
from csplendor.api.usi_parser import parse_usi_move

E5 = cs.ActionEncoderV5
E3 = cs.ActionEncoderV3
COLORS = "WUGRKD"


def _ten_token_game(seed=42):
    game = cs.Game(seed=seed)
    player = game.board.players[0]
    player.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, player)
    return game


def _deck_reserve(game, level=1):
    return next(
        action
        for action in game.legal_actions
        if action.type == cs.ActionType.RESERVE_DECK
        and int(action.deck_level) == level
    )


def _visible_reserve(game, level=0, slot=0):
    card = int(game.board.visible[level][slot])
    return next(
        action
        for action in game.legal_actions
        if action.type == cs.ActionType.RESERVE_VISIBLE and int(action.card_id) == card
    )


def _reserve(game, kind):
    return _deck_reserve(game) if kind == "deck" else _visible_reserve(game)


def _return_phase_game(seed=42, kind="deck"):
    game = _ten_token_game(seed)
    assert game.apply(_reserve(game, kind), True)
    assert game.board.waiting_return
    return game


def test_visible_reservation_enters_the_return_phase_after_the_refill():
    game = _ten_token_game()
    card = int(game.board.visible[0][0])
    reserves = [a for a in game.legal_actions if a.type == cs.ActionType.RESERVE_VISIBLE]
    assert len(reserves) == 12
    assert all(list(map(int, a.return_gems)) == [0] * 6 for a in reserves)
    assert game.apply(_visible_reserve(game), True)
    board = game.board
    assert board.waiting_return and board.pending_decision == 1
    assert int(board.current_player) == 0
    assert int(board.visible[0][0]) not in (-1, card)  # refilled before the return
    assert list(map(int, board.players[0].gems)) == [2, 2, 2, 2, 2, 1]
    assert [a.type for a in game.legal_actions] == [cs.ActionType.RETURN_GEM] * 6


def test_return_actions_serialize_and_resolve_as_single_letters():
    game = _return_phase_game()
    usi = [action_to_usi(action, game=game) for action in game.legal_actions]
    assert usi == [f"return:{letter}" for letter in COLORS]
    for index, move in enumerate(usi):
        assert find_legal_action_index_by_usi(game, move) == index
        assert find_legal_action_index_by_usi(game, move.lower()) == index
    parsed = parse_usi_move("return:D")
    assert parsed.kind == "return" and parsed.return_gems == [0, 0, 0, 0, 0, 1]
    with pytest.raises(ValueError):
        parse_usi_move("return:WU")


def test_return_moves_are_illegal_outside_the_return_phase():
    game = _ten_token_game()
    with pytest.raises(ValueError, match="no legal return action"):
        find_legal_action_index_by_usi(game, "return:W")


def test_reservations_are_one_ply_and_reject_inline_returns():
    game = _ten_token_game()
    card = int(game.board.visible[0][0])
    assert action_to_usi(_deck_reserve(game), game=game) == "reserve:L2"
    assert action_to_usi(_visible_reserve(game), game=game) == f"reserve:C{card}"
    for move in ("reserve:L2/return:W", f"reserve:C{card}/return:W"):
        with pytest.raises(ValueError, match="separate ply"):
            find_legal_action_index_by_usi(game, move)


def test_legacy_reservation_shorthand_expands_to_two_plies():
    assert expand_usi_move("reserve:L2/return:W") == ["reserve:L2", "return:W"]
    assert expand_usi_move("reserve:l3/return:d") == ["reserve:l3", "return:d"]
    assert expand_usi_move("reserve:C12/return:W") == ["reserve:C12", "return:W"]
    assert expand_usi_move("reserve:c7/return:u") == ["reserve:c7", "return:u"]
    for unchanged in (
        "reserve:L2",
        "reserve:C12",
        "take:WUG/return:K",
        "reserve:L2/return:WU",
        "reserve:C12/return:WU",
    ):
        assert expand_usi_move(unchanged) == [unchanged]


@pytest.mark.parametrize("kind", ["deck", "visible"])
def test_position_replay_accepts_canonical_and_legacy_returns(kind):
    start = _ten_token_game()
    reserve = "reserve:L2" if kind == "deck" else f"reserve:C{int(start.board.visible[0][0])}"
    base = game_to_spn(start)
    canonical = position_to_game(f"position {base} moves {reserve} return:W")
    legacy = position_to_game(f"position {base} moves {reserve}/return:W")
    for game in (canonical, legacy):
        assert not game.board.waiting_return
        assert int(game.board.current_player) == 1
        assert list(map(int, game.board.players[0].gems)) == [1, 2, 2, 2, 2, 1]
    assert canonical.board.hash() == legacy.board.hash()


@pytest.mark.parametrize("kind", ["deck", "visible"])
def test_spn_infers_the_return_phase_from_the_movers_token_count(kind):
    game = _return_phase_game(kind=kind)
    restored = spn_to_game(game_to_spn(game, reveal_hidden_reserved_ids=True))
    assert restored.board.waiting_return
    assert restored.board.pending_decision == 1
    assert [a.type for a in restored.legal_actions] == [cs.ActionType.RETURN_GEM] * 6

    settled = spn_to_game(game_to_spn(_ten_token_game()))
    assert not settled.board.waiting_return
    assert settled.board.pending_decision == 0


def test_game_state_schema_exposes_the_pending_decision():
    state = get_game_state(_return_phase_game())
    assert state.board.waiting_return is True
    assert state.board.pending_decision == 1
    assert state.board.waiting_noble is False
    assert {a.type for a in state.legal_actions} == {int(cs.ActionType.RETURN_GEM)}

    idle = get_game_state(_ten_token_game())
    assert idle.board.waiting_return is False
    assert idle.board.pending_decision == 0


def _service_with_ten_tokens():
    service = GameSessionService({}, {}, id_factory=lambda: "s")
    session = service.create_game(
        seed=42,
        simple_payment_mode=True,
        player0_name="A",
        player1_name="B",
    )
    game = service.require_game(session)
    player = game.board.players[0]
    player.gems = [2, 2, 2, 2, 2, 0]
    game.board.set_player(0, player)
    return service, session


def test_service_records_legacy_shorthand_as_two_canonical_plies():
    service, session = _service_with_ten_tokens()
    assert service.require_record(session)["meta"]["Format"] == "Splendor KIFU v1.2"
    index, canonical, game = service.apply_usi(session, "reserve:L2/return:W")
    assert canonical == "reserve:L2 return:W"
    assert index == 0  # return:W is the first of the six return choices
    assert int(game.board.current_player) == 1
    moves = service.require_record(session)["moves"]
    assert [move["usi"] for move in moves] == ["reserve:L2", "return:W"]
    assert [move["player"] for move in moves] == [0, 0]


def test_service_rejects_unresolvable_shorthand_without_side_effects():
    service, session = _service_with_ten_tokens()
    game = service.require_game(session)
    player = game.board.players[0]
    player.gems = [3, 3, 2, 2, 0, 0]  # no black token to give back
    game.board.set_player(0, player)
    before = game.board.hash()
    with pytest.raises(InvalidRequest, match="no legal return action"):
        service.apply_usi(session, "reserve:L2/return:K")
    assert game.board.hash() == before
    assert not game.board.waiting_return
    assert service.require_record(session)["moves"] == []


def test_v5_layout():
    assert E5.ACTION_SIZE == 3049
    assert (E5.OFFSET_TAKE_DIFFERENT, E5.OFFSET_TAKE_SAME) == (0, 840)
    assert (E5.OFFSET_RESERVE_VISIBLE, E5.OFFSET_RESERVE_DECK, E5.OFFSET_PURCHASE) == (
        980,
        992,
        995,
    )
    assert (E5.OFFSET_VISIT_NOBLE, E5.OFFSET_RETURN_GEM, E5.OFFSET_PASS) == (
        3030,
        3042,
        3048,
    )
    assert E5.schema_version() == 5
    assert E5.schema_fingerprint() == (
        "csplendor.action.v5;size=3049;layout=840,140,12,3,2035,12,6,1"
    )
    assert not hasattr(cs, "ActionEncoderV4")


def test_v5_visible_reservations_are_keyed_by_board_slot():
    game = cs.Game(seed=11)
    for level in range(3):
        for slot in range(4):
            action_id = E5.OFFSET_RESERVE_VISIBLE + level * 4 + slot
            action = E5.decode(action_id, game)
            assert action.type == cs.ActionType.RESERVE_VISIBLE
            assert int(action.card_id) == int(game.board.visible[level][slot])
            assert list(map(int, action.return_gems)) == [0] * 6
            assert E5.encode(action, game) == action_id
    with_return = E5.decode(E5.OFFSET_RESERVE_VISIBLE, game)
    with_return.return_gems = [1, 0, 0, 0, 0, 0]
    assert E5.encode(with_return, game) == -1


def _walk_positions(seeds=(3, 5, 8), plies=40):
    for seed in seeds:
        game = _ten_token_game(seed)
        rng = np.random.default_rng(seed)
        yield game
        reserve = _deck_reserve(game, seed % 3) if seed % 2 else _visible_reserve(game)
        assert game.apply(reserve, True)
        for _ in range(plies):
            yield game
            if game.is_game_over():
                break
            actions = game.legal_actions
            assert game.apply(actions[int(rng.integers(len(actions)))], True)


def test_v5_mask_ids_and_decode_agree_including_the_return_phase():
    seen_return_phase = False
    for game in _walk_positions():
        mask = np.asarray(E5.get_action_mask(game), dtype=bool)
        ids = list(E5.legal_action_ids(game))
        assert sorted(ids) == np.flatnonzero(mask).tolist()
        assert len(ids) == len({E5.encode(a, game) for a in game.legal_actions})
        for v5_id in ids:
            action = E5.decode_and_match(v5_id, game)
            assert action is not None
            assert E5.encode(action, game) == v5_id
        if game.board.waiting_return:
            seen_return_phase = True
            gems = game.board.players[game.board.current_player].gems
            assert sorted(ids) == [
                E5.OFFSET_RETURN_GEM + color for color in range(6) if gems[color]
            ]
            assert not np.asarray(E3.get_action_mask(game)).any()
    assert seen_return_phase


def _noble_choice_game():
    game, _ = _two_noble_game([2, 2, 2, 2, 2, 0])
    take = next(a for a in game.legal_actions if a.type == cs.ActionType.TAKE_DIFFERENT)
    assert game.apply(take, True)
    assert game.board.waiting_noble
    return game


def test_search_state_dim_adds_two_pending_decision_features():
    plain = cs.V3SearchConfig()
    plain.pending_decision_features = False
    flagged = cs.V3SearchConfig()
    assert flagged.pending_decision_features is True
    assert (plain.state_dim, flagged.state_dim) == (314, 316)


@pytest.mark.parametrize(
    "make_game, flags",
    [
        (_ten_token_game, (0.0, 0.0)),
        (_return_phase_game, (1.0, 0.0)),
        (lambda: _return_phase_game(kind="visible"), (1.0, 0.0)),
        (_noble_choice_game, (0.0, 1.0)),
    ],
)
def test_search_features_flag_the_pending_decision(make_game, flags):
    config = cs.V3SearchConfig()
    config.num_simulations = 8
    config.leaf_batch_size = 1
    session = cs.V3SearchSession(config)
    game = make_game()
    session.add_game(game, game.current_player, seed=1)
    features, ids, offsets, slots = session.collect()
    assert features.shape == (1, 316)
    # index 314 = waiting_return, index 315 = waiting_noble
    assert tuple(features[0, 314:].tolist()) == flags
    if flags[0]:
        assert sorted(ids.tolist()) == [E5.OFFSET_RETURN_GEM + c for c in range(6)]
        assert cs.v5_semantic_group_id(int(ids[0])) >= 133


def _mate_fixture(kind="deck"):
    """Player 0 needs one gold for reserved card 15 (W4, 1 point).

    The gold is reachable only through a reservation (of the deck, or of the
    single board card 4), which reaches eleven tokens and must be followed by
    a return.  The return belongs to the reservation and is not a move.
    """
    game = cs.Game(seed=0)
    board = game.board
    board.bank = [0, 0, 0, 0, 0, 5]
    first = [4, -1, -1, -1] if kind == "visible" else [-1] * 4
    board.visible = [first, [-1] * 4, [-1] * 4]
    board.decks = [[0, 1, 2, 3], [], []]
    board.nobles = []
    board.current_player = 0
    board.turn = 0
    for index, gems, points, reserved in (
        (0, [3, 2, 2, 2, 1, 0], 14, [15, -1, -1]),
        (1, [1, 2, 2, 2, 3, 0], 0, [-1, -1, -1]),
    ):
        player = board.get_player(index)
        player.gems = gems
        player.bonuses = [0] * 5
        player.points = points
        player.acquired_nobles = []
        player.reserved = reserved
        player.reserved_is_hidden = [False] * 3
        player.purchased_cards = []
        board.set_player(index, player)
    return game


@pytest.mark.parametrize("kind", ["deck", "visible"])
@pytest.mark.parametrize("solver_name", ["exhaustive", "dfpn"])
def test_python_solvers_do_not_count_the_return_as_a_move(solver_name, kind):
    from scripts.dfpn_mate_solver import solve_game_dfpn
    from scripts.mate_solver import MATE, SolverOptions, solve_game

    solve = solve_game if solver_name == "exhaustive" else solve_game_dfpn
    options = SolverOptions(
        allow_deck_reserve=True,
        max_nodes=100000,
        time_limit=30.0,
        include_proof=False,
    )
    assert solve(_mate_fixture(kind), 0, 1, options).status != MATE
    result = solve(_mate_fixture(kind), 0, 2, options)
    assert result.status == MATE
    assert result.depth == 2


def _two_noble_game(gems):
    """Player 0 qualifies for the first two nobles, so its turn ends in a choice."""
    game = cs.Game(seed=5)
    nobles = [int(n) for n in game.board.nobles[:2]]
    player = game.board.players[0]
    player.bonuses = [
        max(int(cs.get_noble(n).requirement[color]) for n in nobles)
        for color in range(5)
    ]
    player.gems = gems
    game.board.set_player(0, player)
    return game, nobles


@pytest.mark.parametrize("kind", ["deck", "visible"])
def test_return_precedes_the_noble_choice_of_the_same_player(kind):
    game, nobles = _two_noble_game([2, 2, 2, 2, 2, 0])
    assert game.apply(_reserve(game, kind), True)
    assert game.board.pending_decision == 1 and int(game.current_player) == 0

    assert game.apply(game.legal_actions[0], True)
    assert game.board.pending_decision == 2 and int(game.current_player) == 0
    choices = game.legal_actions
    assert {int(a.noble_choice) for a in choices} == set(nobles)
    assert sorted(E5.legal_action_ids(game)) == sorted(
        E5.OFFSET_VISIT_NOBLE + n for n in nobles
    )

    assert game.apply(choices[0], True)
    assert game.board.pending_decision == 0 and int(game.current_player) == 1
    for expected in (2, 1, 0):
        assert game.undo() and game.board.pending_decision == expected


def _drive(session):
    while not session.all_done():
        features, ids, offsets, slots = session.collect()
        if len(slots) == 0:
            break
        assert features.shape == (len(slots), session.state_dim)
        priors = np.zeros(len(ids), dtype=np.float32)
        for row in range(len(slots)):
            width = offsets[row + 1] - offsets[row]
            priors[offsets[row] : offsets[row + 1]] = 1.0 / width
        session.apply(ids, offsets, priors, np.zeros(len(slots), dtype=np.float32))


@pytest.mark.parametrize("phase", ["noble", "return", "visible_return", "before_reserve"])
def test_search_handles_consecutive_decisions_of_one_player(phase):
    game, nobles = _two_noble_game([2, 2, 2, 2, 2, 0])
    if phase == "noble":
        take = next(a for a in game.legal_actions if a.type == cs.ActionType.TAKE_DIFFERENT)
        assert game.apply(take, True)
        assert game.board.waiting_noble
    elif phase == "return":
        assert game.apply(_deck_reserve(game), True)
    elif phase == "visible_return":
        assert game.apply(_visible_reserve(game), True)
    config = cs.V3SearchConfig()
    config.num_simulations = 64
    config.leaf_batch_size = 4
    session = cs.V3SearchSession(config)
    session.add_game(game, game.current_player, seed=3)
    _drive(session)
    visits = session.root_visits(0)
    legal = set(E5.legal_action_ids(game))
    assert visits and set(visits) <= legal
    assert sum(visits.values()) == 63
    if phase == "noble":
        assert legal == {E5.OFFSET_VISIT_NOBLE + n for n in nobles}
