"""Phase 6.3 transition and reveal-search structural regression tests."""

import hashlib
import json
from collections import Counter

import pytest

import csplendor as cs

TARGET_TYPES = {
    cs.ActionType.RESERVE_VISIBLE,
    cs.ActionType.RESERVE_DECK,
    cs.ActionType.PURCHASE,
    cs.ActionType.VISIT_NOBLE,
}


def _player_signature(player):
    return [
        int(player.points),
        list(map(int, player.gems)),
        int(player.packed_gems),
        list(map(int, player.bonuses)),
        int(player.packed_bonuses),
        int(player.reserved_count),
        list(map(int, player.reserved)),
        list(map(bool, player.reserved_is_hidden)),
        int(player.purchased_count),
        list(map(int, player.purchased_cards)),
        list(map(int, player.acquired_nobles)),
    ]


def _game_signature(game):
    board = game.board
    return [
        list(map(int, board.bank)),
        [list(map(int, level)) for level in board.visible],
        [list(map(int, level)) for level in board.decks],
        list(map(int, board.nobles)),
        [_player_signature(board.get_player(i)) for i in range(2)],
        int(board.current_player),
        int(board.turn),
        bool(board.final_round),
        bool(board.waiting_noble),
        int(board.winner),
        bool(game.simple_payment_mode),
        bool(game.blank_refill_mode),
        int(game.board_hash()),
    ]


def _sha256(payload):
    encoded = json.dumps(payload, separators=(",", ":"), sort_keys=False)
    return hashlib.sha256(encoded.encode()).hexdigest()


def test_reserve_and_purchase_full_state_corpus_digest():
    records = []
    coverage = Counter()
    for seed in range(16):
        game = cs.Game(seed=seed)
        for ply in range(36):
            actions = game.legal_actions
            for action in actions:
                if action.type not in TARGET_TYPES:
                    continue
                key = (
                    int(action.type),
                    bool(action.from_reserved)
                    if action.type == cs.ActionType.PURCHASE
                    else False,
                )
                coverage[key] += 1
                blank_modes = [False]
                if action.type in (
                    cs.ActionType.RESERVE_VISIBLE,
                    cs.ActionType.PURCHASE,
                ) and not action.from_reserved:
                    blank_modes.append(True)
                for blank_refill in blank_modes:
                    child = game.clone_light()
                    child.blank_refill_mode = blank_refill
                    applied = child.apply_trusted(action, False)
                    records.append(
                        [
                            seed,
                            ply,
                            int(action.pack()),
                            blank_refill,
                            applied,
                            _game_signature(child),
                        ]
                    )

            if not actions or game.is_game_over():
                break
            preferred = [a for a in actions if a.type in TARGET_TYPES]
            choices = preferred or actions
            action = choices[(seed + ply * 7) % len(choices)]
            assert game.apply_trusted(action, False)

    assert coverage == Counter(
        {
            (int(cs.ActionType.RESERVE_VISIBLE), False): 1512,
            (int(cs.ActionType.RESERVE_DECK), False): 378,
            (int(cs.ActionType.PURCHASE), False): 449,
            (int(cs.ActionType.PURCHASE), True): 67,
        }
    )
    assert len(records) == 4367
    assert _sha256(records) == (
        "c2876911b854f766a95d6137586016202d1bae95c4a5ce5843b5c7b3c0076b44"
    )


def test_noble_and_end_turn_full_state_corpus_digest():
    records = []
    for current_player in (0, 1):
        for noble_count in (0, 1, 2, 3):
            for points in (0, 14, 15):
                for other_points in (0, 14, 15, 18):
                    for purchased_counts in ((0, 0), (2, 3), (3, 2), (3, 3)):
                        game = cs.Game(seed=11)
                        game.board.current_player = current_player
                        game.board.nobles = list(range(noble_count))
                        game.board.turn = 7
                        for player_index in range(2):
                            player = game.board.get_player(player_index)
                            player.bonuses = (
                                [10] * 5
                                if player_index == current_player
                                else [0] * 5
                            )
                            player.points = (
                                points
                                if player_index == current_player
                                else other_points
                            )
                            player.purchased_count = purchased_counts[
                                player_index
                            ]
                            player.purchased_cards = list(
                                range(purchased_counts[player_index])
                            )
                            game.board.set_player(player_index, player)

                        action = next(
                            (
                                candidate
                                for candidate in game.legal_actions
                                if candidate.type
                                in (
                                    cs.ActionType.TAKE_DIFFERENT,
                                    cs.ActionType.TAKE_SAME,
                                )
                            ),
                            None,
                        )
                        if action is None:
                            continue
                        applied = game.apply_trusted(action, False)
                        records.append(
                            [
                                current_player,
                                noble_count,
                                points,
                                other_points,
                                purchased_counts,
                                int(action.pack()),
                                applied,
                                _game_signature(game),
                            ]
                        )
                        if game.board.waiting_noble:
                            for noble_action in game.legal_actions:
                                child = game.clone_light()
                                applied = child.apply_trusted(noble_action, False)
                                records.append(
                                    [
                                        "visit",
                                        current_player,
                                        noble_count,
                                        points,
                                        other_points,
                                        purchased_counts,
                                        int(noble_action.pack()),
                                        applied,
                                        _game_signature(child),
                                    ]
                                )

    assert len(records) == 864
    assert _sha256(records) == (
        "68c150a01838d0c8bb803f76af6150a012d30c6addfccd2851e5d6baa4391c04"
    )


def _solver_payload(game, attacker, depth, *, include_proof_dag=False):
    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=attacker,
        depth=depth,
        include_proof_dag=include_proof_dag,
        proof_dag_node_limit=200000,
        proof_dag_edge_limit=500000,
    )
    stats = {
        key: value
        for key, value in result["stats"].items()
        if key != "elapsed_ms"
    }
    payload = {
        "proven": result["proven"],
        "reason": result["reason"],
        "unknown_reason": result["unknown_reason"],
        "memoized_states": result["memoized_states"],
        "stats": stats,
        "line": result["line"],
        "proof_dag": result["proof_dag"],
    }
    return result, hashlib.sha256(
        json.dumps(payload, sort_keys=True, separators=(",", ":")).encode()
    ).hexdigest()


@pytest.mark.parametrize(
    ("fixture", "expected_digest"),
    [
        (
            "seed0_d1",
            "14f0d044fde184c43d0aac65977e6e70024186d49b8201175397d87d990187f9",
        ),
        (
            "seed7_d2",
            "69ce0311114cbd040afe929f3849a5da1634e0252316f0bbb46f0bbc63e512b8",
        ),
        (
            "purchase_dag",
            "29bc5c0c9f0ea209c6965837eb6303935367d4771f6404626830c3258e5e1290",
        ),
        (
            "oracle_blank",
            "3b025a800bef7ae148479e28071a5cd5183e2b0a4915df8711f30e87de451786",
        ),
    ],
)
def test_reveal_solver_structure_and_node_count_digest(fixture, expected_digest):
    if fixture == "seed0_d1":
        game, attacker, depth, include_dag = cs.Game(seed=0), 0, 1, False
    elif fixture == "seed7_d2":
        game, attacker, depth, include_dag = cs.Game(seed=7), 0, 2, False
    elif fixture == "purchase_dag":
        game = cs.Game(seed=0)
        game.board.current_player = 1
        player = game.board.get_player(1)
        player.points = 14
        player.bonuses = [10] * 5
        game.board.set_player(1, player)
        attacker, depth, include_dag = 1, 1, True
    else:
        game = cs.Game(seed=3)
        visible = game.board.visible
        visible[0][0] = -1
        game.board.visible = visible
        game.board.current_player = 1
        attacker, depth, include_dag = 0, 1, False

    result, digest = _solver_payload(
        game, attacker, depth, include_proof_dag=include_dag
    )
    assert digest == expected_digest
    if include_dag:
        assert result["proof_dag"]["complete"] is True
        assert result["proof_dag"]["validated"] is True


def test_deck_reserve_reveal_edges_replay_to_final_round_children():
    game = cs.Game(seed=5)
    game.board.current_player = 1
    player = game.board.get_player(1)
    player.points = 15
    game.board.set_player(1, player)
    action = next(
        candidate
        for candidate in game.legal_actions
        if candidate.type == cs.ActionType.RESERVE_DECK
    )

    result = cs.solve_reveal_verified_mate_cpp(
        game,
        attacker=1,
        depth=1,
        required_root_action=int(action.pack()),
        include_proof_dag=True,
        proof_dag_node_limit=5000,
        proof_dag_edge_limit=20000,
    )

    dag = result["proof_dag"]
    root_edges = dag["nodes"][dag["root"]]["children"]
    assert result["proven"] is True
    assert dag["complete"] is True
    assert dag["validated"] is True
    assert result["stats"]["nodes"] == 37
    assert result["stats"]["deck_reserve_candidates"] == 36
    assert result["stats"]["deck_reserve_branches"] == 36
    assert len(root_edges) == 36
    assert {edge["action_code"] for edge in root_edges} == {int(action.pack())}
    assert all(edge["reveal_card"] is not None for edge in root_edges)
    assert all(edge["oracle_card"] is None for edge in root_edges)
    assert all(edge["oracle_reserve"] is False for edge in root_edges)
