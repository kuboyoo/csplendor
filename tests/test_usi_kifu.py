from csplendor import Game
from csplendor.api.app import _build_replay_from_kifu_text
from csplendor.api.usi_kifu import (
    action_to_usi,
    build_kifu_text,
    find_legal_action_index_by_usi,
    parse_kifu_text,
)


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
