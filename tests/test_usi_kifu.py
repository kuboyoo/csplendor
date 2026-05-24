from csplendor import Game
from csplendor.api.app import _build_replay_from_kifu_text
from csplendor.api.usi_kifu import (
    action_to_usi,
    build_kifu_text,
    find_legal_action_index_by_usi,
    game_to_spn,
    parse_kifu_text,
    position_to_game,
    spn_to_game,
)
import pytest


def _signature(action):
    return (
        int(action.type),
        tuple(int(v) for v in action.take),
        int(action.card_id),
        int(action.deck_level),
        bool(action.from_reserved),
        tuple(int(v) for v in action.gold_as),
        tuple(int(v) for v in action.return_gems),
        int(action.noble_choice),
    )


def _state_signature(game):
    board = game.board
    players = []
    for idx in range(2):
        player = board.get_player(idx)
        players.append((
            tuple(int(v) for v in player.gems),
            tuple(int(v) for v in player.bonuses),
            int(player.points),
            tuple(int(v) for v in player.reserved),
            tuple(bool(v) for v in player.reserved_is_hidden),
            int(player.reserved_count),
            int(player.purchased_count),
            tuple(int(v) for v in player.purchased_cards),
        ))
    return (
        tuple(int(v) for v in board.bank),
        tuple(tuple(int(v) for v in row) for row in board.visible),
        tuple(tuple(sorted(int(v) for v in deck)) for deck in board.decks),
        tuple(sorted(int(v) for v in board.nobles)),
        int(board.current_player),
        tuple(players),
    )


def test_canonical_usi_round_trips_all_initial_legal_actions():
    game = Game(seed=42)
    game.simple_payment_mode = True

    for action in game.legal_actions:
        usi = action_to_usi(action, game=game)
        idx = find_legal_action_index_by_usi(game, usi)
        assert idx >= 0
        assert _signature(game.legal_actions[idx]) == _signature(action)


def test_kifu_text_parse_and_replay_round_trip():
    game = Game(seed=42)
    game.simple_payment_mode = True
    moves = []

    for ply in range(4):
        action = game.legal_actions[0]
        moves.append({
            "player": int(game.board.current_player),
            "usi": action_to_usi(action, game=game),
            "time_ms": 10 + ply,
            "comment": f"move-{ply}",
        })
        assert game.apply(action) is True

    text = build_kifu_text(
        headers={
            "Format": "Splendor KIFU v1.0",
            "Players": "2",
            "Player0": "A",
            "Player1": "B",
            "Seed": "42",
            "SimplePaymentMode": "1",
        },
        position="startpos 2",
        moves=moves,
        result="ONGOING",
        final_scores=game.scores,
        total_turns=game.turn,
    )

    parsed = parse_kifu_text(text)
    assert parsed["headers"]["Seed"] == "42"
    assert parsed["position"] == "startpos 2"
    assert len(parsed["moves"]) == len(moves)
    assert parsed["moves"][0]["comment"] == "move-0"

    replay = _build_replay_from_kifu_text(text)
    assert replay["seed"] == 42
    assert replay["simple_payment_mode"] is True
    assert len(replay["states"]) == len(moves) + 1
    assert [move["usi"] for move in replay["moves"]] == [move["usi"] for move in moves]


def test_spn_to_game_round_trips_observable_state_and_unseen_sets():
    game = Game(seed=7)
    spn = game_to_spn(game)

    parsed = spn_to_game(spn)

    assert _state_signature(parsed) == _state_signature(game)


def test_position_startpos_with_moves_uses_seed_and_applies_usi_moves():
    expected = Game(seed=42)
    move = action_to_usi(expected.legal_actions[0], game=expected)
    assert expected.apply(expected.legal_actions[0], False)

    parsed = position_to_game(f"position startpos 2 moves {move}", seed=42)

    assert _state_signature(parsed) == _state_signature(expected)


def test_position_spn_with_moves_applies_move_from_given_position():
    base = Game(seed=11)
    expected = spn_to_game(game_to_spn(base))
    move = action_to_usi(expected.legal_actions[0], game=expected)
    assert expected.apply(expected.legal_actions[0], False)

    parsed = position_to_game(f"position {game_to_spn(base)} moves {move}", seed=999)

    assert _state_signature(parsed) == _state_signature(expected)


def test_spn_hidden_reserved_cards_are_rejected_for_exact_solver_state():
    spn = game_to_spn(Game(seed=3)).replace("reserved:[]", "reserved:[?L1]", 1)

    with pytest.raises(ValueError, match="hidden reserved"):
        spn_to_game(spn)


def test_editor_spn_with_unknown_bought_cards_keeps_non_visible_non_reserved_cards_in_decks():
    spn = (
        "bank:W1U3G3R3K0D4 | "
        "visible:L1[35,33,20,24]L2[46,61,51,66]L3[80,86,87,88] | "
        "decks:36,23,15 | "
        "nobles:[1,10,6] | "
        "P0:name:Player0;gems:W3U1G1R1K2D0;bonuses:W2U2G1R3K3;"
        "points:0;nobles:[-,-,-];reserved:[68];bought:[_,_,_,_,_,_,_,_,_,_,_] | "
        "P1:name:Player1;gems:W0U0G0R0K2D1;bonuses:W3U1G0R0K3;"
        "points:0;nobles:[-,-,-];reserved:[85,44,43];bought:[_,_,_,_,_,_,_] | 0"
    )

    game = spn_to_game(spn)

    assert [len(deck) for deck in game.board.decks] == [36, 23, 15]
    assert int(game.board.current_player) == 0
    assert int(game.board.get_player(0).purchased_count) == 11
    assert int(game.board.get_player(1).purchased_count) == 7
    assert list(game.board.get_player(0).purchased_cards) == []
    assert list(game.board.get_player(1).purchased_cards) == []
    assert 68 not in game.board.decks[1]
    assert 85 not in game.board.decks[2]
    assert 35 not in game.board.decks[0]
